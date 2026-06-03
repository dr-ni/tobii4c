/*
gcc -O2 -Wall -Wextra \
     -I/usr/include \
     eyemouse.c \
     -o eyemouse \
     -L/usr/lib/tobii \
     -Wl,-rpath,/usr/lib/tobii \
     -ltobii_stream_engine \
     -lX11 -lXtst -lm -lpthread
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>

#include <linux/input.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <tobii/tobii.h>
#include <tobii/tobii_streams.h>

#define DEADZONE 3

static int debug = 0;
static int debug_gaze = 0;
static int debug_gyro = 0;

#define DBG(...) \
    do { if(debug) { fprintf(stderr, "[DBG] " __VA_ARGS__); fflush(stderr); } } while(0)

#define DBGGAZE(...) \
    do { if(debug_gaze) { fprintf(stderr, "[GAZE] " __VA_ARGS__); fflush(stderr); } } while(0)

#define DBGGYRO(...) \
    do { if(debug_gyro) { fprintf(stderr, "[GYRO] " __VA_ARGS__); fflush(stderr); } } while(0)

#define DBGBLINK(...) \
    do { if(debug_blink) { fprintf(stderr, "[BLINK] " __VA_ARGS__); fflush(stderr); } } while(0)

/*
    blink click configuration
*/

static int use_blink = 0;
static int debug_blink = 0;

/*
    thresholds in milliseconds
    left click  : blink_ms_left  <= duration < blink_ms_right
    right click : blink_ms_right <= duration < blink_ms_hold
    hold toggle : blink_ms_hold  <= duration
*/

static int blink_ms_left  = 80;
static int blink_ms_right = 300;
static int blink_ms_hold  = 600;

/*
    blink state
*/

typedef enum
{
    BLINK_IDLE,
    BLINK_CLOSED

} blink_state_t;

static blink_state_t blink_state = BLINK_IDLE;
static struct timespec blink_t0;
static int blink_hold_active = 0;

static Display* blink_display = NULL;

typedef struct
{
    float raw_x;
    float raw_y;

    float target_x;
    float target_y;

} point_t;

typedef struct
{
    float rx[3];
    float ry[3];

    float tx[3];
    float ty[3];

} tri_t;

point_t p[3][3];

float gx=0.5f;
float gy=0.5f;

float sx=0.5f;
float sy=0.5f;

float cx=0.0f;
float cy=0.0f;

float gyro_dx=0.0f;
float gyro_dy=0.0f;

float sensor_offset_x=0.0f;
float sensor_offset_y=0.0f;

int screen_w=0;
int screen_h=0;

/*
    global Quha fd for signal handler cleanup
*/

static volatile int quha_fd = -1;
static volatile int quha_grabbed = 0;

static void signal_handler(int sig)
{
    (void)sig;

    if(quha_fd >= 0)
    {
        if(quha_grabbed)
        {
            ioctl(quha_fd,EVIOCGRAB,0);
            printf("\nQuha released\n");
        }

        close(quha_fd);
        quha_fd = -1;
    }

    _exit(0);
}


void print_help(const char* prog)
{
    printf(
        "Usage:\n"
        "  %s [options] [config_file]\n"
        "\n"
        "Options:\n"
        "  -h --help                show help\n"
        "  --usegyro                use Quha gyro (grabs Quha exclusively)\n"
        "  --leaveXgyro             use Quha gyro but leave Quha as X11 mouse\n"
        "  --blink                  enable blink click\n"
        "  --blink-left  <ms>       left click threshold  (default: 80)\n"
        "  --blink-right <ms>       right click threshold (default: 300)\n"
        "  --blink-hold  <ms>       hold toggle threshold (default: 600)\n"
        "  --debug                  verbose debug output on stderr\n"
        "  --debuggaze              verbose gaze output on stderr\n"
        "  --debuggyro              verbose gyro event output on stderr\n"
        "  --debugblink             verbose blink event output on stderr\n"
        "\n"
        "Examples:\n"
        "  %s\n"
        "  %s --blink\n"
        "  %s --blink --blink-left 100 --blink-right 350 --blink-hold 650\n"
        "  %s --usegyro --blink\n"
        "  %s --debug --debugblink\n",
        prog,
        prog,
        prog,
        prog,
        prog,
        prog);
}

int load_config(const char* path)
{
    FILE* f =
        fopen(path,"r");

    if(!f)
    {
        perror(path);
        return 0;
    }

    printf(
        "Loading calibration: %s\n",
        path);

    DBG("load_config: opened %s\n", path);

    char line[256];

    int idx = 0;

    float sensor_offset_x_local = 0.0f;
    float sensor_offset_y_local = 0.0f;

    while(fgets(line,sizeof(line),f))
    {
        /*
            skip comments
        */

        if(line[0] == '#')
        {
            DBG("load_config: skip comment: %s", line);
            continue;
        }

        /*
            skip empty lines
        */

        if(line[0] == '\n')
            continue;

        /*
            key value parameters
        */

        char key[64];
        float val;

        if(sscanf(line, "%63s %f", key, &val) == 2 &&
           !(key[0] >= '0' && key[0] <= '9') &&
           key[0] != '-')
        {
            if(!strcmp(key, "sensor_offset_x"))
            {
                sensor_offset_x_local = val;
                DBG("load_config: sensor_offset_x=%.6f\n", val);
            }
            else if(!strcmp(key, "sensor_offset_y"))
            {
                sensor_offset_y_local = val;
                DBG("load_config: sensor_offset_y=%.6f\n", val);
            }
            else
            {
                DBG("load_config: skip key: %s = %.6f\n", key, val);
            }

            continue;
        }

        float rx,ry,tx,ty;

        /*
            calibration point
        */

        if(sscanf(
            line,
            "%f %f %f %f",
            &rx,
            &ry,
            &tx,
            &ty) == 4)
        {
            int y = idx / 3;
            int x = idx % 3;

            if(idx < 9)
            {
                p[y][x].raw_x = rx;
                p[y][x].raw_y = ry;

                p[y][x].target_x = tx;
                p[y][x].target_y = ty;

                printf(
                    "CAL %d "
                    "raw=%f,%f "
                    "target=%f,%f\n",
                    idx,
                    rx,
                    ry,
                    tx,
                    ty);

                DBG("load_config: CAL[%d] grid[%d][%d] "
                    "raw=(%.6f,%.6f) target=(%.6f,%.6f)\n",
                    idx, y, x, rx, ry, tx, ty);

                idx++;
            }
        }
        else
        {
            printf(
                "Ignoring line: %s",
                line);

            DBG("load_config: unparseable line: %s", line);
        }
    }

    fclose(f);

    sensor_offset_x = sensor_offset_x_local;
    sensor_offset_y = sensor_offset_y_local;

    printf(
        "Loaded %d calibration points\n",
        idx);

    printf(
        "Sensor offset: x=%.6f y=%.6f\n",
        sensor_offset_x,
        sensor_offset_y);

    DBG("load_config: total points loaded: %d\n", idx);

    if(idx != 9)
    {
        fprintf(
            stderr,
            "Need exactly 9 calibration points\n");

        return 0;
    }

    return 1;
}


float clampf(float v,float a,float b)
{
    if(v<a) return a;
    if(v>b) return b;

    return v;
}

int bary(
    float px,float py,
    float ax,float ay,
    float bx,float by,
    float cx_,float cy_,
    float* u,
    float* v,
    float* w)
{
    float d =
        (by-cy_)*(ax-cx_) +
        (cx_-bx)*(ay-cy_);

    if(fabs(d) < 1e-6f)
        return 0;

    *u =
        ((by-cy_)*(px-cx_) +
         (cx_-bx)*(py-cy_)) / d;

    *v =
        ((cy_-ay)*(px-cx_) +
         (ax-cx_)*(py-cy_)) / d;

    *w =
        1.0f - *u - *v;

    return 1;
}

void warp(
    float raw_x,
    float raw_y,
    float* ox,
    float* oy)
{
    tri_t t[] =
    {
        {
            {p[0][0].raw_x,p[0][1].raw_x,p[1][0].raw_x},
            {p[0][0].raw_y,p[0][1].raw_y,p[1][0].raw_y},
            {p[0][0].target_x,p[0][1].target_x,p[1][0].target_x},
            {p[0][0].target_y,p[0][1].target_y,p[1][0].target_y}
        },

        {
            {p[1][0].raw_x,p[0][1].raw_x,p[1][1].raw_x},
            {p[1][0].raw_y,p[0][1].raw_y,p[1][1].raw_y},
            {p[1][0].target_x,p[0][1].target_x,p[1][1].target_x},
            {p[1][0].target_y,p[0][1].target_y,p[1][1].target_y}
        },

        {
            {p[0][1].raw_x,p[0][2].raw_x,p[1][1].raw_x},
            {p[0][1].raw_y,p[0][2].raw_y,p[1][1].raw_y},
            {p[0][1].target_x,p[0][2].target_x,p[1][1].target_x},
            {p[0][1].target_y,p[0][2].target_y,p[1][1].target_y}
        },

        {
            {p[1][1].raw_x,p[0][2].raw_x,p[1][2].raw_x},
            {p[1][1].raw_y,p[0][2].raw_y,p[1][2].raw_y},
            {p[1][1].target_x,p[0][2].target_x,p[1][2].target_x},
            {p[1][1].target_y,p[0][2].target_y,p[1][2].target_y}
        },

        {
            {p[1][0].raw_x,p[1][1].raw_x,p[2][0].raw_x},
            {p[1][0].raw_y,p[1][1].raw_y,p[2][0].raw_y},
            {p[1][0].target_x,p[1][1].target_x,p[2][0].target_x},
            {p[1][0].target_y,p[1][1].target_y,p[2][0].target_y}
        },

        {
            {p[2][0].raw_x,p[1][1].raw_x,p[2][1].raw_x},
            {p[2][0].raw_y,p[1][1].raw_y,p[2][1].raw_y},
            {p[2][0].target_x,p[1][1].target_x,p[2][1].target_x},
            {p[2][0].target_y,p[1][1].target_y,p[2][1].target_y}
        },

        {
            {p[1][1].raw_x,p[1][2].raw_x,p[2][1].raw_x},
            {p[1][1].raw_y,p[1][2].raw_y,p[2][1].raw_y},
            {p[1][1].target_x,p[1][2].target_x,p[2][1].target_x},
            {p[1][1].target_y,p[1][2].target_y,p[2][1].target_y}
        },

        {
            {p[2][1].raw_x,p[1][2].raw_x,p[2][2].raw_x},
            {p[2][1].raw_y,p[1][2].raw_y,p[2][2].raw_y},
            {p[2][1].target_x,p[1][2].target_x,p[2][2].target_x},
            {p[2][1].target_y,p[1][2].target_y,p[2][2].target_y}
        }
    };

    for(int i=0;i<8;i++)
    {
        float u,v,w;

        if(!bary(
            raw_x,raw_y,
            t[i].rx[0],t[i].ry[0],
            t[i].rx[1],t[i].ry[1],
            t[i].rx[2],t[i].ry[2],
            &u,&v,&w))
        {
            DBGGAZE("warp: tri[%d] degenerate, skip\n", i);
            continue;
        }

        if(u >= -0.02f &&
           v >= -0.02f &&
           w >= -0.02f)
        {
            *ox =
                u*t[i].tx[0] +
                v*t[i].tx[1] +
                w*t[i].tx[2];

            *oy =
                u*t[i].ty[0] +
                v*t[i].ty[1] +
                w*t[i].ty[2];

            *ox = clampf(*ox,0.0f,1.0f);
            *oy = clampf(*oy,0.0f,1.0f);

            DBGGAZE("warp: tri[%d] hit  "
                "raw=(%.4f,%.4f) "
                "uvw=(%.4f,%.4f,%.4f) "
                "out=(%.4f,%.4f)\n",
                i, raw_x, raw_y, u, v, w, *ox, *oy);

            return;
        }
    }

    DBGGAZE("warp: no triangle hit for raw=(%.4f,%.4f), using clamped passthrough\n",
        raw_x, raw_y);

    *ox = clampf(raw_x,0.0f,1.0f);
    *oy = clampf(raw_y,0.0f,1.0f);
}

int open_quha_device(int grab)
{
    DBG("open_quha_device: scanning /dev/input/by-id (grab=%d)\n", grab);

    DIR* dir =
        opendir("/dev/input/by-id");

    if(!dir)
    {
        perror("opendir");
        return -1;
    }

    struct dirent* de;

    while((de = readdir(dir)))
    {
        DBG("open_quha_device: entry %s\n", de->d_name);

        if(!strstr(de->d_name,"Quha"))
            continue;

        if(!strstr(de->d_name,"event"))
            continue;

        DBG("open_quha_device: Quha event entry found: %s\n", de->d_name);

        char linkpath[PATH_MAX];

        snprintf(
            linkpath,
            sizeof(linkpath),
            "/dev/input/by-id/%s",
            de->d_name);

        char target[PATH_MAX];

        ssize_t len =
            readlink(
                linkpath,
                target,
                sizeof(target)-1);

        if(len <= 0)
        {
            DBG("open_quha_device: readlink failed for %s\n", linkpath);
            continue;
        }

        target[len] = 0;

        DBG("open_quha_device: symlink target: %s\n", target);

        char final[PATH_MAX];

        snprintf(
            final,
            sizeof(final),
            "/dev/input/%s",
            strrchr(target,'/')+1);

        printf(
            "Using Quha device: %s\n",
            final);

        DBG("open_quha_device: opening %s\n", final);

        int fd =
            open(final,O_RDONLY);

        if(fd >= 0)
        {
            DBG("open_quha_device: fd=%d opened\n", fd);

            if(grab)
            {
                /*
                    grab device exclusively so X11
                    does not receive Quha mouse events
                */

                if(ioctl(fd,EVIOCGRAB,1) < 0)
                {
                    perror("EVIOCGRAB");
                    fprintf(
                        stderr,
                        "Warning: could not grab Quha exclusively\n"
                        "X11 may still receive mouse events\n");

                    DBG("open_quha_device: EVIOCGRAB failed\n");
                }
                else
                {
                    printf(
                        "Quha grabbed exclusively\n");

                    quha_grabbed = 1;

                    DBG("open_quha_device: EVIOCGRAB success\n");
                }
            }
            else
            {
                DBG("open_quha_device: grab skipped (--leaveXgyro)\n");
            }

            closedir(dir);
            return fd;
        }
        else
        {
            DBG("open_quha_device: open(%s) failed: %m\n", final);
        }
    }

    closedir(dir);

    fprintf(
        stderr,
        "No Quha device found\n");

    DBG("open_quha_device: no Quha device found in /dev/input/by-id\n");

    return -1;
}

void* gyro_thread(void* arg)
{
    (void)arg;

    int fd = quha_fd;

    DBG("gyro_thread: started fd=%d\n", fd);

    if(fd < 0)
        return NULL;

    struct input_event ev;

    while(read(fd,&ev,sizeof(ev)) > 0)
    {
        if(ev.type != EV_REL)
            continue;

        if(ev.code == REL_X)
        {
            gyro_dx += ev.value * 2.0f;

            DBGGYRO("REL_X value=%d  gyro_dx=%.2f\n",
                ev.value, gyro_dx);
        }

        if(ev.code == REL_Y)
        {
            gyro_dy += ev.value * 2.0f;

            DBGGYRO("REL_Y value=%d  gyro_dy=%.2f\n",
                ev.value, gyro_dy);
        }
    }

    DBG("gyro_thread: read loop ended\n");

    return NULL;
}

static long ms_since(const struct timespec* t0)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    return (now.tv_sec  - t0->tv_sec)  * 1000L +
           (now.tv_nsec - t0->tv_nsec) / 1000000L;
}

static void blink_send_click(int button)
{
    if(!blink_display)
        return;

    XTestFakeButtonEvent(blink_display, button, True,  CurrentTime);
    XTestFakeButtonEvent(blink_display, button, False, CurrentTime);
    XFlush(blink_display);
}

static void blink_send_hold(int button, int press)
{
    if(!blink_display)
        return;

    XTestFakeButtonEvent(blink_display, button, press, CurrentTime);
    XFlush(blink_display);
}

static void blink_process(int valid_now)
{
    if(!use_blink)
        return;

    if(blink_state == BLINK_IDLE)
    {
        if(!valid_now)
        {
            /*
                eye closed: record start time
            */

            clock_gettime(CLOCK_MONOTONIC, &blink_t0);
            blink_state = BLINK_CLOSED;

            DBGBLINK("eye closed\n");
        }

        return;
    }

    /*
        BLINK_CLOSED: eye was closed, check if open again
    */

    if(valid_now)
    {
        long ms = ms_since(&blink_t0);

        DBGBLINK("eye open after %ldms\n", ms);

        if(ms < blink_ms_left)
        {
            /*
                too short: noise, ignore
            */

            DBGBLINK("ignored (noise < %dms)\n", blink_ms_left);
        }
        else if(ms < blink_ms_right)
        {
            /*
                left click
            */

            DBGBLINK("left click (%ldms)\n", ms);
            printf("\nBlink: left click\n");
            blink_send_click(1);
        }
        else if(ms < blink_ms_hold)
        {
            /*
                right click
            */

            DBGBLINK("right click (%ldms)\n", ms);
            printf("\nBlink: right click\n");
            blink_send_click(3);
        }
        else
        {
            /*
                hold toggle
            */

            blink_hold_active = !blink_hold_active;

            DBGBLINK("hold toggle -> %s (%ldms)\n",
                blink_hold_active ? "press" : "release", ms);

            printf(
                "\nBlink: hold %s\n",
                blink_hold_active ? "press" : "release");

            blink_send_hold(1, blink_hold_active);
        }

        blink_state = BLINK_IDLE;
    }
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

    int valid_now = (g->validity == TOBII_VALIDITY_VALID);

    blink_process(valid_now);

    if(!valid_now)
    {
        DBGGAZE("invalid gaze sample, skipping\n");
        return;
    }

    DBGGAZE("raw=(%.4f,%.4f)\n",
        g->position_xy[0],
        g->position_xy[1]);

    warp(
        g->position_xy[0],
        g->position_xy[1],
        &gx,
        &gy);

    DBGGAZE("raw=(%.4f,%.4f) warped=(%.4f,%.4f)\n",
        g->position_xy[0], g->position_xy[1],
        gx, gy);
}

void get_config_path(
    char* out,
    size_t size)
{
    const char* home =
        getenv("HOME");

    snprintf(
        out,
        size,
        "%s/.local/tobii_4c/calx11.conf",
        home ? home : ".");
}

int main(int argc,char** argv)
{
    int use_gyro = 0;
    int leave_x_gyro = 0;
    char cfg[PATH_MAX];

    get_config_path(
        cfg,
        sizeof(cfg));

    for(int i=1;i<argc;i++)
    {
        if(!strcmp(argv[i],"-h") ||
           !strcmp(argv[i],"--help"))
        {
            print_help(argv[0]);
            return 0;
        }

        if(!strcmp(argv[i],"--usegyro"))
        {
            use_gyro = 1;
            continue;
        }

        if(!strcmp(argv[i],"--leaveXgyro"))
        {
            use_gyro = 1;
            leave_x_gyro = 1;
            continue;
        }

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

        if(!strcmp(argv[i],"--debuggyro"))
        {
            debug_gyro = 1;
            continue;
        }

        if(!strcmp(argv[i],"--blink"))
        {
            use_blink = 1;
            continue;
        }

        if(!strcmp(argv[i],"--debugblink"))
        {
            debug_blink = 1;
            continue;
        }

        if(!strcmp(argv[i],"--blink-left") && i+1<argc)
        {
            blink_ms_left = atoi(argv[++i]);
            continue;
        }

        if(!strcmp(argv[i],"--blink-right") && i+1<argc)
        {
            blink_ms_right = atoi(argv[++i]);
            continue;
        }

        if(!strcmp(argv[i],"--blink-hold") && i+1<argc)
        {
            blink_ms_hold = atoi(argv[++i]);
            continue;
        }

        /*
            config path
        */

        snprintf(
            cfg,
            sizeof(cfg),
            "%s",
            argv[i]);
    }

    DBG("eyemouse starting\n");
    DBG("config path: %s\n", cfg);
    DBG("use_gyro: %d\n", use_gyro);
    DBG("leave_x_gyro: %d\n", leave_x_gyro);
    DBG("use_blink: %d\n", use_blink);
    DBG("debug_gaze: %d\n", debug_gaze);
    DBG("debug_gyro: %d\n", debug_gyro);
    DBG("debug_blink: %d\n", debug_blink);
    DBG("blink thresholds: left=%dms right=%dms hold=%dms\n",
        blink_ms_left, blink_ms_right, blink_ms_hold);

    if(!load_config(cfg))
    {
        printf("Invalid calibration file\n");
        return 1;
    }

    Display* d =
        XOpenDisplay(NULL);

    if(!d)
    {
        fprintf(stderr,
            "No X11 display\n");

        return 1;
    }

    DBG("X11 display opened\n");

    blink_display = d;

    if(use_blink)
    {
        printf(
            "Blink click enabled  "
            "left<%dms  right<%dms  hold>=%dms\n",
            blink_ms_right,
            blink_ms_hold,
            blink_ms_hold);

        DBG("blink: left=%dms right=%dms hold=%dms\n",
            blink_ms_left, blink_ms_right, blink_ms_hold);
    }
    else
    {
        printf("Blink click disabled\n");
    }

    int screen =
        DefaultScreen(d);

    screen_w =
        DisplayWidth(d,screen);

    screen_h =
        DisplayHeight(d,screen);

    printf(
        "Screen: %dx%d\n",
        screen_w,
        screen_h);

    DBG("screen: %dx%d  screen_index=%d\n", screen_w, screen_h, screen);

    Window root =
        RootWindow(d,screen);

    DBG("root window: 0x%lx\n", root);

    cx = screen_w/2;
    cy = screen_h/2;

    tobii_api_t* api;
    tobii_device_t* dev;

    char url[256]={0};

    tobii_error_t err;

    err = tobii_api_create(
        &api,
        NULL,
        NULL);

    DBG("tobii_api_create: %d\n", err);

    tobii_enumerate_local_device_urls(
        api,
        url_cb,
        url);

    if(url[0] == 0)
    {
        fprintf(stderr, "No Tobii device found\n");
        return 1;
    }

    printf(
        "Device: %s\n",
        url);

    DBG("tobii device url: %s\n", url);

    err = tobii_device_create(
        api,
        url,
        &dev);

    DBG("tobii_device_create: %d\n", err);

    err = tobii_gaze_point_subscribe(
        dev,
        gaze_cb,
        NULL);

    DBG("tobii_gaze_point_subscribe: %d\n", err);

    signal(SIGTERM, signal_handler);
    signal(SIGINT,  signal_handler);
    signal(SIGHUP,  signal_handler);

    if(use_gyro)
    {
        quha_fd = open_quha_device(!leave_x_gyro);

        if(quha_fd >= 0)
        {
            pthread_t gt;

            pthread_create(
                &gt,
                NULL,
                gyro_thread,
                NULL);

            if(leave_x_gyro)
            {
                printf(
                    "Gyro enabled (Quha left as X11 mouse)\n");
            }
            else
            {
                printf(
                    "Gyro enabled (Quha grabbed exclusively)\n");
            }
        }
        else
        {
            printf(
                "Gyro enabled but no Quha device found\n");
        }
    }
    else
    {
        DBG("Quha grab skipped (--usegyro not set)\n");
        printf(
            "Gyro disabled\n");
    }

    int lx=screen_w/2;
    int ly=screen_h/2;

    unsigned long frame = 0;

    while(1)
    {
        tobii_device_process_callbacks(
            dev);

        sx =
            sx*0.88f +
            gx*0.12f;

        sy =
            sy*0.88f +
            gy*0.12f;

        float tx =
            sx * screen_w;

        float ty =
            sy * screen_h;

        cx =
            cx*0.78f +
            (tx + gyro_dx)*0.22f;

        cy =
            cy*0.78f +
            (ty + gyro_dy)*0.22f;

        gyro_dx *= 0.92f;
        gyro_dy *= 0.92f;

        int x =
            clampf(
                cx,
                0,
                screen_w-1);

        int y =
            clampf(
                cy,
                0,
                screen_h-1);

        if(debug_gaze && (frame % 25 == 0))
        {
            fprintf(stderr,
                "[GAZE] frame=%lu "
                "gaze=(%.4f,%.4f) "
                "smooth=(%.4f,%.4f) "
                "gyro=(%.2f,%.2f) "
                "cursor=(%d,%d)\n",
                frame,
                gx, gy,
                sx, sy,
                gyro_dx, gyro_dy,
                x, y);
            fflush(stderr);
        }

        frame++;

        if(abs(x-lx)<DEADZONE &&
           abs(y-ly)<DEADZONE)
        {
            usleep(4000);
            continue;
        }

        DBG("XWarpPointer: (%d,%d) -> (%d,%d)\n", lx, ly, x, y);

        XWarpPointer(
            d,
            None,
            root,
            0,0,0,0,
            x,
            y);

        XFlush(d);

        lx=x;
        ly=y;

        printf(
            "\rMouse %4d %4d",
            x,
            y);

        fflush(stdout);

        usleep(4000);
    }

    return 0;
}
