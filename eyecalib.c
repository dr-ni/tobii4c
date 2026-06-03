/*
gcc eyecalib.c -o eyecalib \
-O2 -Wall -Wextra \
-I/usr/include \
-L/usr/lib/tobii \
-Wl,-rpath,/usr/lib/tobii \
-ltobii_stream_engine \
-lX11 -lm
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <tobii/tobii.h>
#include <tobii/tobii_streams.h>

/*
    tobii_calibration.h not available on this system
    tobii_enabled_eye_t is already defined in tobii.h
    functions are present in libtobii_stream_engine.so
*/

extern tobii_error_t tobii_calibration_start(
    tobii_device_t* device,
    tobii_enabled_eye_t enabled_eye);

extern tobii_error_t tobii_calibration_stop(
    tobii_device_t* device);

extern tobii_error_t tobii_calibration_collect_data_2d(
    tobii_device_t* device,
    float x,
    float y);

extern tobii_error_t tobii_calibration_compute_and_apply(
    tobii_device_t* device);

typedef void (*tobii_calibration_data_fn)(
    const void* data,
    size_t size,
    void* user_data);

extern tobii_error_t tobii_calibration_retrieve(
    tobii_device_t* device,
    tobii_calibration_data_fn callback,
    void* user_data);

#define POINTS 9
#define SAMPLE_COUNT 50
#define LOOP_LIMIT 2000

#define TARGET_OUTER 22
#define TARGET_MIDDLE 20
#define TARGET_INNER 6
#define TARGET_CROSS 36

#define MAX_RETRIES 5
#define MIN_VALID_SAMPLES 20

static int debug = 0;
static int debug_gaze = 0;

#define DBG(...) \
    do { if(debug) { fprintf(stderr, "[DBG] " __VA_ARGS__); fflush(stderr); } } while(0)

#define DBGGAZE(...) \
    do { if(debug_gaze) { fprintf(stderr, "[GAZE] " __VA_ARGS__); fflush(stderr); } } while(0)

typedef struct
{
    float raw_x;
    float raw_y;

    float target_x;
    float target_y;

} point_t;

typedef struct
{
    int version;

    float sensor_offset_x;
    float sensor_offset_y;

    float screen_distance;
    float screen_tilt;

    float gaze_smooth;
    float cursor_smooth;

    float edge_zone;
    float edge_power;

} runtime_config_t;

runtime_config_t cfg =
{
    .version = 2,

    .sensor_offset_x = 0.0f,
    .sensor_offset_y = 0.0f,

    .screen_distance = 63.0f,
    .screen_tilt = 0.0f,

    .gaze_smooth = 0.12f,
    .cursor_smooth = 0.22f,

    .edge_zone = 0.08f,
    .edge_power = 1.35f
};

point_t p[3][3];

float gx=0.5f;
float gy=0.5f;

int valid=0;

void get_config_path(char* out,size_t size)
{
    const char* home = getenv("HOME");

    snprintf(
        out,
        size,
        "%s/.local/tobii_4c/calx11.conf",
        home ? home : ".");
}

void url_cb(
    const char* url,
    void* user_data)
{
    snprintf(
        (char*)user_data,
        256,
        "%s",
        url);
}

void gaze_cb(
    const tobii_gaze_point_t* g,
    void* user_data)
{
    (void)user_data;

    valid = 0;

    if(g->validity != TOBII_VALIDITY_VALID)
    {
        DBGGAZE("invalid sample\n");
        return;
    }

    float rx = g->position_xy[0] + cfg.sensor_offset_x;
    float ry = g->position_xy[1] + cfg.sensor_offset_y;

    if(rx < 0.0f || rx > 1.0f ||
       ry < 0.0f || ry > 1.0f)
    {
        DBGGAZE("out of range: (%.4f,%.4f) - accepted unclamped\n",
            rx, ry);
    }

    gx = rx;
    gy = ry;

    DBGGAZE("raw=(%.4f,%.4f)\n", gx, gy);

    valid = 1;
}

int import_tobii_db()
{
    char db[PATH_MAX];

    snprintf(
        db,
        sizeof(db),
        "%s/.config/TobiiProEyeTrackerManager/db/da-setups.db",
        getenv("HOME"));

    DBG("import_tobii_db: opening %s\n", db);

    printf(
        "Opening Tobii DB: %s\n",
        db);

    FILE* f = fopen(db,"r");

    if(!f)
    {
        printf(
            "No Tobii DB bootstrap found\n");

        DBG("import_tobii_db: file not found\n");

        return 0;
    }

    /*
        collect all matching profiles first
    */

#define MAX_PROFILES 16

    char profiles[MAX_PROFILES][8192];
    char profile_names[MAX_PROFILES][64];
    int  profile_count = 0;

    char line[8192];

    while(fgets(line,sizeof(line),f) &&
          profile_count < MAX_PROFILES)
    {
        if(!(strstr(line,"\"IS4_Large_Peripheral\"") &&
             strstr(line,"\"readonly\":false")))
        {
            continue;
        }

        memcpy(
            profiles[profile_count],
            line,
            sizeof(profiles[0])-1);

        profiles[profile_count][sizeof(profiles[0])-1] = 0;

        /*
            extract name for display
        */

        char* np = strstr(line,"\"name\":\"");
        if(np)
        {
            np += 8;
            char* end = strchr(np,'"');
            if(end)
            {
                int len = end - np;
                if(len > 63) len = 63;
                strncpy(profile_names[profile_count], np, len);
                profile_names[profile_count][len] = 0;
            }
        }
        else
        {
            snprintf(
                profile_names[profile_count],
                sizeof(profile_names[0]),
                "Profile %d",
                profile_count+1);
        }

        DBG("import_tobii_db: found profile '%s'\n",
            profile_names[profile_count]);

        profile_count++;
    }

    fclose(f);

    if(profile_count == 0)
    {
        printf(
            "[DB] no matching local profile found\n");

        DBG("import_tobii_db: no matching profile\n");

        return 0;
    }

    /*
        if multiple profiles found, ask user to choose
    */

    int selected = 0;

    if(profile_count > 1)
    {
        printf(
            "\n[DB] Multiple profiles found:\n");

        for(int i=0; i<profile_count; i++)
        {
            printf(
                "  %d) %s\n",
                i+1,
                profile_names[i]);
        }

        printf(
            "Select profile [1-%d]: ",
            profile_count);

        fflush(stdout);

        int choice = 0;
        if(scanf("%d",&choice) == 1 &&
           choice >= 1 &&
           choice <= profile_count)
        {
            selected = choice - 1;
        }
        else
        {
            printf(
                "Invalid choice, using first profile\n");

            selected = 0;
        }
    }

    strncpy(line, profiles[selected], sizeof(line)-1);
    line[sizeof(line)-1] = 0;

    int found = 1;

    printf(
        "\n[DB ACTIVE PROFILE] %s\n",
        profile_names[selected]);

    DBG("import_tobii_db: using profile '%s'\n",
        profile_names[selected]);

    if(strstr(line,"\"trackerPlacement\":\"under\""))
    {
        printf(
            "[DB] tracker placement: UNDER\n");

        cfg.sensor_offset_y =
            -0.004f;

        DBG("import_tobii_db: sensor_offset_y set to %.4f (under placement)\n",
            cfg.sensor_offset_y);
    }

    char* a =
        strstr(
            line,
            "\"tracker\":{\"angle\":");

    if(a)
    {
        float angle=0.0f;

        sscanf(
            a,
            "\"tracker\":{\"angle\":%f",
            &angle);

        printf(
            "[DB] tracker angle: %.2f deg\n",
            angle);

        cfg.screen_tilt =
            angle;

        DBG("import_tobii_db: screen_tilt=%.2f\n", cfg.screen_tilt);
    }

    char* w =
        strstr(line,"\"width\":");

    if(w)
    {
        float width_mm=0.0f;

        sscanf(
            w,
            "\"width\":%f",
            &width_mm);

        printf(
            "[DB] display width: %.2f mm\n",
            width_mm);

        DBG("import_tobii_db: display width=%.2f mm\n", width_mm);
    }

    char* h =
        strstr(line,"\"height\":");

    if(h)
    {
        float height_mm=0.0f;

        sscanf(
            h,
            "\"height\":%f",
            &height_mm);

        printf(
            "[DB] display height: %.2f mm\n",
            height_mm);

        DBG("import_tobii_db: display height=%.2f mm\n", height_mm);
    }

    float blx,bly,blz;
    float tlx,tly,tlz;
    float trx,try_,trz;

    int matched =
        sscanf(
            line,
            "%*[^[][%f,%f,%f],\"topLeft\":[%f,%f,%f],\"topRight\":[%f,%f,%f]",
            &blx,&bly,&blz,
            &tlx,&tly,&tlz,
            &trx,&try_,&trz);

    DBG("import_tobii_db: geometry sscanf matched=%d\n", matched);

    if(matched == 9)
    {
        printf(
            "[DB] physical geometry parsed\n");

        printf(
            "     bottomLeft : %.2f %.2f %.2f\n",
            blx,bly,blz);

        printf(
            "     topLeft    : %.2f %.2f %.2f\n",
            tlx,tly,tlz);

        printf(
            "     topRight   : %.2f %.2f %.2f\n",
            trx,try_,trz);

        float screen_skew =
            (trz - blz);

        printf(
            "[DB] screen skew z: %.2f\n",
            screen_skew);

        cfg.sensor_offset_y =
            -(screen_skew / 5000.0f);

        printf(
            "[DB] derived sensor_offset_y = %.5f\n",
            cfg.sensor_offset_y);

        DBG("import_tobii_db: screen_skew=%.2f sensor_offset_y=%.5f\n",
            screen_skew, cfg.sensor_offset_y);
    }

    if(!found)
    {
        printf(
            "[DB] no matching local profile found\n");

        DBG("import_tobii_db: no matching profile\n");

        return 0;
    }

    printf(
        "\n[DB] bootstrap import complete\n");

    printf(
        "[DB] runtime config:\n");

    printf(
        "     sensor_offset_x = %.5f\n",
        cfg.sensor_offset_x);

    printf(
        "     sensor_offset_y = %.5f\n",
        cfg.sensor_offset_y);

    printf(
        "     screen_tilt     = %.2f\n",
        cfg.screen_tilt);

    DBG("import_tobii_db: complete  "
        "offset=(%.5f,%.5f) tilt=%.2f\n",
        cfg.sensor_offset_x,
        cfg.sensor_offset_y,
        cfg.screen_tilt);

    return 1;
}

void save_config(
    const char* path)
{
    FILE* f = fopen(path,"w");

    if(!f)
        return;

    fprintf(f,"version %d\n",cfg.version);

    fprintf(
        f,
        "sensor_offset_x %.6f\n",
        cfg.sensor_offset_x);

    fprintf(
        f,
        "sensor_offset_y %.6f\n",
        cfg.sensor_offset_y);

    fprintf(
        f,
        "screen_distance %.3f\n",
        cfg.screen_distance);

    fprintf(
        f,
        "screen_tilt %.3f\n",
        cfg.screen_tilt);

    fprintf(
        f,
        "gaze_smooth %.3f\n",
        cfg.gaze_smooth);

    fprintf(
        f,
        "cursor_smooth %.3f\n",
        cfg.cursor_smooth);

    fprintf(
        f,
        "edge_zone %.3f\n",
        cfg.edge_zone);

    fprintf(
        f,
        "edge_power %.3f\n",
        cfg.edge_power);

    fprintf(f,"# calibration\n");

    for(int y=0;y<3;y++)
    {
        for(int x=0;x<3;x++)
        {
            fprintf(
                f,
                "%.6f %.6f %.6f %.6f\n",
                p[y][x].raw_x,
                p[y][x].raw_y,
                p[y][x].target_x,
                p[y][x].target_y);
        }
    }

    fclose(f);
}

void draw_target(
    Display* d,
    Window w,
    GC gc,
    Colormap cmap,
    int x,
    int y,
    int active)
{
    XColor red,green,white,gray;

    XParseColor(d,cmap,"#ff3030",&red);
    XAllocColor(d,cmap,&red);

    XParseColor(d,cmap,"#30ff30",&green);
    XAllocColor(d,cmap,&green);

    XParseColor(d,cmap,"#ffffff",&white);
    XAllocColor(d,cmap,&white);

    XParseColor(d,cmap,"#808080",&gray);
    XAllocColor(d,cmap,&gray);

    XClearWindow(d,w);

    XSetLineAttributes(
        d,
        gc,
        1,
        LineSolid,
        CapRound,
        JoinRound);

    XSetForeground(d,gc,gray.pixel);

    XDrawLine(
        d,w,gc,
        x - TARGET_CROSS,
        y,
        x + TARGET_CROSS,
        y);

    XDrawLine(
        d,w,gc,
        x,
        y - TARGET_CROSS,
        x,
        y + TARGET_CROSS);

    XSetForeground(
        d,
        gc,
        white.pixel);

    XFillArc(
        d,w,gc,
        x - TARGET_OUTER,
        y - TARGET_OUTER,
        TARGET_OUTER*2,
        TARGET_OUTER*2,
        0,
        360*64);

    XSetForeground(
        d,
        gc,
        BlackPixel(d,DefaultScreen(d)));

    XFillArc(
        d,w,gc,
        x - TARGET_MIDDLE,
        y - TARGET_MIDDLE,
        TARGET_MIDDLE*2,
        TARGET_MIDDLE*2,
        0,
        360*64);

    XSetForeground(
        d,
        gc,
        active ? green.pixel : red.pixel);

    XFillArc(
        d,w,gc,
        x - TARGET_INNER,
        y - TARGET_INNER,
        TARGET_INNER*2,
        TARGET_INNER*2,
        0,
        360*64);

    XFlush(d);
}

/*
    native Tobii 4C calibration via tobii_calibration_* API

    attempts to run native calibration before the mesh calibration
    so the tracker delivers normalized [0..1] gaze coordinates.

    on tobii-ttp:// devices (Professional series) calibration_start
    may return TOBII_ERROR_NOT_SUPPORTED (2) because calibration is
    managed by the tobii engine service. in that case we check if
    a calibration already exists via tobii_calibration_retrieve.
*/

static void calib_data_cb(
    const void* data,
    size_t size,
    void* user_data)
{
    (void)data;
    int* found = (int*)user_data;
    *found = (size > 0) ? 1 : 0;

    printf(
        "Existing calibration found: %zu bytes\n",
        size);

    DBG("calib_data_cb: size=%zu\n", size);
}

int run_tobii_calibration(
    tobii_device_t* dev,
    Display* d,
    Window win,
    GC gc,
    Colormap cmap,
    int W,
    int H)
{
    tobii_error_t err;

    printf(
        "\n--- Native Tobii calibration ---\n");

    DBG("run_tobii_calibration: start\n");

    err = tobii_calibration_start(dev, TOBII_ENABLED_EYE_BOTH);

    if(err != TOBII_ERROR_NO_ERROR)
    {
        fprintf(
            stderr,
            "tobii_calibration_start: err=%d"
            " (may not be supported on this device)\n",
            err);

        /*
            check if a calibration already exists
        */

        int found = 0;

        tobii_error_t rerr =
            tobii_calibration_retrieve(
                dev,
                calib_data_cb,
                &found);

        DBG("tobii_calibration_retrieve: err=%d found=%d\n",
            rerr, found);

        if(rerr == TOBII_ERROR_NO_ERROR && found)
        {
            printf(
                "Using existing device calibration\n\n");

            return 1;
        }

        fprintf(
            stderr,
            "No existing calibration found.\n"
            "Please calibrate using Tobii Pro Eye Tracker Manager\n"
            "or tobii_engine service before running eyecalib.\n\n");

        return 0;
    }

    DBG("tobii_calibration_start ok\n");

    /*
        9 calibration points matching the mesh targets
    */

    float pts[9][2] =
    {
        {0.05f,0.05f},
        {0.50f,0.05f},
        {0.95f,0.05f},

        {0.05f,0.50f},
        {0.50f,0.50f},
        {0.95f,0.50f},

        {0.05f,0.95f},
        {0.50f,0.95f},
        {0.95f,0.95f}
    };

    for(int i = 0; i < 9; i++)
    {
        float tx = pts[i][0];
        float ty = pts[i][1];

        int px = tx * W;
        int py = ty * H;

        if(d)
            draw_target(d, win, gc, cmap, px, py, 0);

        printf(
            "Native CAL %d/9 : %.2f %.2f\n",
            i+1, tx, ty);

        DBG("native cal point %d: (%.4f,%.4f)\n", i+1, tx, ty);

        sleep(2);

        err = tobii_calibration_collect_data_2d(dev, tx, ty);

        if(err != TOBII_ERROR_NO_ERROR)
        {
            fprintf(
                stderr,
                "tobii_calibration_collect_data_2d failed "
                "point %d: %d\n",
                i+1, err);

            sleep(1);

            err = tobii_calibration_collect_data_2d(dev, tx, ty);

            if(err != TOBII_ERROR_NO_ERROR)
            {
                fprintf(
                    stderr,
                    "retry failed point %d: %d\n",
                    i+1, err);
            }
        }

        DBG("native cal point %d collected err=%d\n", i+1, err);

        if(d)
            draw_target(d, win, gc, cmap, px, py, 1);

        usleep(350000);
    }

    printf(
        "Computing native calibration...\n");

    DBG("tobii_calibration_compute_and_apply: start\n");

    err = tobii_calibration_compute_and_apply(dev);

    if(err != TOBII_ERROR_NO_ERROR)
    {
        fprintf(
            stderr,
            "tobii_calibration_compute_and_apply failed: %d\n",
            err);

        tobii_calibration_stop(dev);

        return 0;
    }

    DBG("tobii_calibration_compute_and_apply ok\n");

    err = tobii_calibration_stop(dev);

    DBG("tobii_calibration_stop err=%d\n", err);

    printf(
        "Native Tobii calibration complete\n\n");

    return 1;
}

int main(int argc, char** argv)
{
    for(int i=1;i<argc;i++)
    {
        if(!strcmp(argv[i],"--debug"))
        {
            debug = 1;
            continue;
        }

        if(!strcmp(argv[i],"--debuggaze"))
        {
            debug_gaze = 1;
            continue;
        }

        if(!strcmp(argv[i],"-h") ||
           !strcmp(argv[i],"--help"))
        {
            printf(
                "Usage: eyecalib [options]\n"
                "\n"
                "Options:\n"
                "  -h --help       show help\n"
                "  --debug         verbose debug output on stderr\n"
                "  --debuggaze     verbose gaze output on stderr (high frequency)\n");
            return 0;
        }
    }

    DBG("eyecalib starting\n");
    DBG("debug=%d debug_gaze=%d\n", debug, debug_gaze);

    import_tobii_db();

    DBG("runtime config after DB import:\n");
    DBG("  sensor_offset_x = %.5f\n", cfg.sensor_offset_x);
    DBG("  sensor_offset_y = %.5f\n", cfg.sensor_offset_y);
    DBG("  screen_distance = %.3f\n", cfg.screen_distance);
    DBG("  screen_tilt     = %.3f\n", cfg.screen_tilt);
    DBG("  gaze_smooth     = %.3f\n", cfg.gaze_smooth);
    DBG("  cursor_smooth   = %.3f\n", cfg.cursor_smooth);
    DBG("  edge_zone       = %.3f\n", cfg.edge_zone);
    DBG("  edge_power      = %.3f\n", cfg.edge_power);

    Display* d = XOpenDisplay(NULL);

    if(!d)
    {
        fprintf(stderr, "No X11 display\n");
        return 1;
    }

    DBG("X11 display opened\n");

    int screen =
        DefaultScreen(d);

    int W =
        DisplayWidth(d,screen);

    int H =
        DisplayHeight(d,screen);

    DBG("screen: %dx%d  index=%d\n", W, H, screen);

    Window root =
        RootWindow(d,screen);

    float targets[9][2] =
    {
        {0.05f,0.05f},
        {0.50f,0.05f},
        {0.95f,0.05f},

        {0.05f,0.50f},
        {0.50f,0.50f},
        {0.95f,0.50f},

        {0.05f,0.95f},
        {0.50f,0.95f},
        {0.95f,0.95f}
    };

    /*
        init Tobii before creating fullscreen window
        so native calibration can run without being covered
    */

    tobii_api_t* api;
    tobii_device_t* dev;

    char url[256]={0};

    if(tobii_api_create(
        &api,
        NULL,
        NULL) != TOBII_ERROR_NO_ERROR)
    {
        fprintf(stderr, "tobii_api_create failed\n");
        return 1;
    }

    DBG("tobii_api_create ok\n");

    tobii_enumerate_local_device_urls(
        api,
        url_cb,
        url);

    if(strlen(url)==0)
    {
        fprintf(stderr, "No Tobii device found\n");
        return 1;
    }

    printf(
        "Using device: %s\n",
        url);

    DBG("tobii device url: %s\n", url);

    if(tobii_device_create(
        api,
        url,
        &dev) != TOBII_ERROR_NO_ERROR)
    {
        fprintf(stderr, "tobii_device_create failed\n");
        return 1;
    }

    DBG("tobii_device_create ok\n");

    /*
        run native Tobii calibration before fullscreen window
        so the desktop is visible during native calibration
    */

    if(!run_tobii_calibration(dev, NULL, None, None, None, W, H))
    {
        fprintf(
            stderr,
            "Native calibration failed, continuing with mesh calibration only\n");
    }

    /*
        now create fullscreen window for mesh calibration
    */

    XSetWindowAttributes attr;

    attr.override_redirect = True;

    Window win =
        XCreateWindow(
            d,
            root,
            0,
            0,
            W,
            H,
            0,
            CopyFromParent,
            InputOutput,
            CopyFromParent,
            CWOverrideRedirect,
            &attr);

    XMapRaised(d,win);

    XSetWindowBackground(
        d,
        win,
        BlackPixel(d,screen));

    XClearWindow(d,win);

    XGrabKeyboard(
        d,
        win,
        True,
        GrabModeAsync,
        GrabModeAsync,
        CurrentTime);

    XGrabPointer(
        d,
        win,
        True,
        ButtonPressMask,
        GrabModeAsync,
        GrabModeAsync,
        win,
        None,
        CurrentTime);

    XFlush(d);

    GC gc =
        XCreateGC(d,win,0,NULL);

    Colormap cmap =
        DefaultColormap(d,screen);

    tobii_gaze_point_subscribe(
        dev,
        gaze_cb,
        NULL);

    DBG("tobii_gaze_point_subscribe ok\n");

    /*
        quick stream test - check if tracker delivers any data
        retry with tobii_engine restart if no data received
    */

    printf("Testing gaze stream...\n");

    {
        int test_valid = 0;

        for(int attempt = 0; attempt < 2; attempt++)
        {
            int test_loops = 0;
            test_valid = 0;

            while(test_loops < 1000 && test_valid < 3)
            {
                valid = 0;

                tobii_wait_for_callbacks(1, &dev);
                tobii_device_process_callbacks(dev);

                if(valid)
                    test_valid++;

                test_loops++;
                usleep(5000);
            }

            DBG("stream test attempt %d: valid=%d loops=%d\n",
                attempt+1, test_valid, test_loops);

            if(test_valid > 0)
                break;

            if(attempt == 0)
            {
                /*
                    no data — restart tobii_engine and retry
                */

                printf(
                    "No gaze data, restarting tobii_engine...\n");

                tobii_gaze_point_unsubscribe(dev);
                tobii_device_destroy(dev);
                tobii_api_destroy(api);

                system("sudo systemctl restart tobii_engine");

                sleep(3);

                tobii_api_create(&api, NULL, NULL);
                tobii_enumerate_local_device_urls(api, url_cb, url);
                tobii_device_create(api, url, &dev);
                tobii_gaze_point_subscribe(dev, gaze_cb, NULL);

                printf("Retrying gaze stream...\n");
            }
        }

        if(test_valid == 0)
        {
            fprintf(
                stderr,
                "No gaze data from tracker after stream test.\n"
                "Check: tracker powered, USB connected, tobii_engine running.\n"
                "Run: systemctl status tobii_engine\n");

            return 1;
        }

        printf("Gaze stream ok\n");

        DBG("stream test ok: valid=%d\n", test_valid);
    }

    valid = 0;

    printf(
        "Calibration starting\n");

    printf(
        "Screen %dx%d smooth=%.3f cursor=%.3f edge=%.3f\n",
        W,
        H,
        cfg.gaze_smooth,
        cfg.cursor_smooth,
        cfg.edge_zone);

    for(int i=0;i<POINTS;i++)
    {
        p[i/3][i%3].target_x =
            targets[i][0];

        p[i/3][i%3].target_y =
            targets[i][1];

        int px =
            targets[i][0] * W;

        int py =
            targets[i][1] * H;

        draw_target(
            d,
            win,
            gc,
            cmap,
            px,
            py,
            0);

        printf(
            "Point %d/%d : %.2f %.2f\n",
            i+1,
            POINTS,
            targets[i][0],
            targets[i][1]);

        DBG("point %d/%d target=(%.4f,%.4f) screen=(%d,%d)\n",
            i+1, POINTS,
            targets[i][0], targets[i][1],
            px, py);

        sleep(1);

        valid = 0;

        usleep(300000);

        float sx = 0.0f;
        float sy = 0.0f;
        int sx_count = 0;

        int success = 0;

        for(int retry=0;
            retry<MAX_RETRIES;
            retry++)
        {
            int count=0;
            int loops=0;
            float rx=0.0f, ry=0.0f;

            while(count < SAMPLE_COUNT &&
                  loops < LOOP_LIMIT)
            {
                valid = 0;

                tobii_error_t err;

                err =
                    tobii_wait_for_callbacks(
                        1,
                        &dev);

                if(err != TOBII_ERROR_NO_ERROR &&
                   err != TOBII_ERROR_TIMED_OUT)
                {
                    printf(
                        "\n[Tobii] wait failed: %d\n",
                        err);

                    break;
                }

                err =
                    tobii_device_process_callbacks(
                        dev);

                if(err != TOBII_ERROR_NO_ERROR)
                {
                    printf(
                        "\n[Tobii] process failed: %d\n",
                        err);

                    break;
                }

                while(XPending(d))
                {
                    XEvent e;

                    XNextEvent(d,&e);
                }

                if(valid)
                {
                    rx += gx;
                    ry += gy;

                    count++;

                    printf(
                        "\rSamples %3d/%3d gaze %.3f %.3f   ",
                        count,
                        SAMPLE_COUNT,
                        gx,
                        gy);

                    fflush(stdout);
                }
                else
                {
                    if((loops % 100) == 0)
                    {
                        printf(
                            "\rWaiting for valid gaze frames... %-6d",
                            loops);

                        fflush(stdout);
                    }

                    /*
                        if no samples at all after 500 loops
                        (~1s) abort this retry early
                    */

                    if(loops == 500 && count == 0)
                    {
                        printf(
                            "\nNo gaze received, skipping retry\n");

                        break;
                    }
                }

                loops++;

                usleep(2000);
            }

            printf("\n");

            /*
                accumulate into global sx/sy across retries
            */

            sx += rx;
            sy += ry;
            sx_count += count;

            if(count >= MIN_VALID_SAMPLES)
            {
                p[i/3][i%3].raw_x =
                    rx / count;

                p[i/3][i%3].raw_y =
                    ry / count;

                success=1;

                printf(
                    "[OK] valid samples: %d\n",
                    count);

                DBG("point %d ok: raw=(%.6f,%.6f) samples=%d\n",
                    i+1,
                    p[i/3][i%3].raw_x,
                    p[i/3][i%3].raw_y,
                    count);

                break;
            }

            printf(
                "Retry %d/%d valid=%d\n",
                retry+1,
                MAX_RETRIES,
                count);

            DBG("retry %d/%d insufficient samples=%d\n",
                retry+1, MAX_RETRIES, count);

            sleep(1);
        }

        if(!success && sx_count == 0 && i == 0)
        {
            fprintf(
                stderr,
                "No gaze data received at all. "
                "Check tracker connection and position.\n");

            return 1;
        }

        if(!success)
        {
            /*
                use best available samples if any were collected
                across all retries — better than aborting
            */

            if(sx != 0.0f || sy != 0.0f)
            {
                p[i/3][i%3].raw_x = sx / (sx_count > 0 ? sx_count : 1);
                p[i/3][i%3].raw_y = sy / (sx_count > 0 ? sx_count : 1);

                fprintf(
                    stderr,
                    "[WARN] point %d: only %d samples, "
                    "using best-effort average\n",
                    i+1, sx_count);

                DBG("point %d best-effort: raw=(%.6f,%.6f) samples=%d\n",
                    i+1,
                    p[i/3][i%3].raw_x,
                    p[i/3][i%3].raw_y,
                    sx_count);
            }
            else
            {
                p[i/3][i%3].raw_x = p[i/3][i%3].target_x;
                p[i/3][i%3].raw_y = p[i/3][i%3].target_y;

                fprintf(
                    stderr,
                    "[WARN] point %d: no samples collected, "
                    "using target as fallback\n",
                    i+1);

                DBG("point %d fallback to target\n", i+1);
            }
        }

        draw_target(
            d,
            win,
            gc,
            cmap,
            px,
            py,
            1);

        printf(
            "RAW %.6f %.6f -> TARGET %.2f %.2f ERR %.4f %.4f\n",
            p[i/3][i%3].raw_x,
            p[i/3][i%3].raw_y,
            p[i/3][i%3].target_x,
            p[i/3][i%3].target_y,
            p[i/3][i%3].raw_x -
            p[i/3][i%3].target_x,
            p[i/3][i%3].raw_y -
            p[i/3][i%3].target_y);

        usleep(350000);
    }

    char cfgpath[PATH_MAX];

    get_config_path(
        cfgpath,
        sizeof(cfgpath));

    save_config(cfgpath);

    printf(
        "Saved calibration: %s\n",
        cfgpath);

    DBG("calibration saved to %s\n", cfgpath);

    tobii_gaze_point_unsubscribe(dev);

    tobii_device_destroy(dev);

    tobii_api_destroy(api);

    XDestroyWindow(d,win);

    XCloseDisplay(d);

    return 0;
}
