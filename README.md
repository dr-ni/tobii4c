# tobii4c

Standalone Tobii Eye Tracker 4C support for Linux/X11.

`tobii4c` provides realtime gaze tracking, fullscreen calibration
and eye-controlled mouse interaction for Linux desktops using X11.

The current stable runtime is based on:

* Barycentric triangle mesh calibration
* 3x3 calibration mesh warping
* Tobii Stream Engine
* Direct X11 cursor control
* Low latency gaze smoothing
* Local triangle interpolation
* Optional Quha gyro integration

The current runtime does NOT yet use:

* Full projective warp
* Active bootstrap geometry
* Active C12 mapping
* Full 3D reprojection

These parts exist experimentally in newer branches but are not part
of the current stable runtime.

---

# Runtime Architecture

Current stable runtime pipeline:

```text
Tobii Eye Tracker 4C
        │
        ▼
Tobii Stream Engine
        │
        ▼
raw gaze coordinates
        │
        ▼
triangle selection
        │
        ▼
barycentric interpolation
        │
        ▼
local warped coordinates
        │
        ▼
runtime smoothing
        │
        ▼
optional Quha gyro offset
        │
        ▼
X11 cursor mapping
        │
        ▼
XWarpPointer
```

---

# Installation

## 1. Install Tobii services and libraries

```bash
bash install.sh
```

This installs:

- Tobii USB service (`tobiiusb.service`)
- Tobii Stream Engine service (`tobii_engine.service`)
- Tobii shared libraries into `/usr/lib/tobii/`
- Tobii headers into `/usr/include/tobii/`
- Library config into `/etc/ld.so.conf.d/tobii.conf`

Both systemd services are enabled and started automatically.

Afterwards, update the dynamic linker cache:

```bash
sudo ldconfig
```

## 2. Build eyecalib and eyemouse

```bash
make
```

## 3. Install binaries and man pages

```bash
sudo make install
```

Installs to `/usr/local/bin/` by default.
A custom prefix can be set:

```bash
sudo make install PREFIX=/usr
```

## 4. Run calibration

```bash
eyecalib
```

## 5. Run eye mouse

```bash
eyemouse
```

---

# Uninstall

Remove Tobii services and libraries:

```bash
bash uninstall.sh
```

Remove binaries and man pages:

```bash
sudo make uninstall
```

---

# Calibration

`eyecalib` runs in two phases:

**Phase 1 — Native Tobii calibration** via `tobii_calibration_*` API:
Adapts the tracker firmware to the user's eyes so the Stream Engine
delivers normalized [0..1] gaze coordinates. Without this step, raw
gaze values can be far outside [0..1] and the mesh calibration cannot
work correctly.

**Phase 2 — Mesh calibration** (barycentric warp):
Collects gaze samples at 9 screen positions and builds the correction
mesh that maps raw gaze to accurate screen coordinates.

Run:

```bash
eyecalib
```

Options:

| Option          | Description                                          |
|-----------------|------------------------------------------------------|
| `-h --help`     | Show command line help                               |
| `--debug`       | Verbose debug output on stderr                       |
| `--debuggaze`   | Verbose gaze output on stderr (high frequency)       |

```bash
# debug calibration
eyecalib --debug

# gaze debug
eyecalib --debuggaze

# all to log file
eyecalib --debug --debuggaze 2>eyecalib.log
```

The calibration process:

```text
Phase 1: Native Tobii calibration
    -> tobii_calibration_start
    -> tobii_calibration_collect_data_2d (9 points)
    -> tobii_calibration_compute_and_apply
    -> tobii_calibration_stop
    -> tracker now delivers normalized [0..1] gaze

Phase 2: Mesh calibration
    -> fullscreen targets
    -> gaze sample collection (250 samples per point)
    -> invalid sample rejection
    -> gaze averaging
    -> calibration mesh generation
    -> persistent calibration storage
```

Calibration data is stored in:

```
~/.local/tobii_4c/calx11.conf
```

### Tobii DB bootstrap

If a Tobii Pro Eye Tracker Manager profile exists at:

```
~/.config/TobiiProEyeTrackerManager/db/da-setups.db
```

`eyecalib` will automatically import physical screen geometry
(tracker placement, screen tilt, display dimensions) and derive
a `sensor_offset_y` correction from it.

### Calibration targets

The 9 targets are placed near screen edges to maximize
fullscreen reachability:

```text
P00(5%,5%)   P01(50%,5%)   P02(95%,5%)
P10(5%,50%)  P11(50%,50%)  P12(95%,50%)
P20(5%,95%)  P21(50%,95%)  P22(95%,95%)
```

### Calibration file format

Each line stores one calibration point:

```
raw_x raw_y target_x target_y
```

Example:

```
0.052524 0.075656 0.050000 0.050000
```

| Value    | Meaning                    |
|----------|----------------------------|
| raw_x    | Measured Tobii gaze x      |
| raw_y    | Measured Tobii gaze y      |
| target_x | Expected screen x (0–1)    |
| target_y | Expected screen y (0–1)    |

The config file also stores runtime parameters:

| Parameter      | Default | Meaning                          |
|----------------|---------|----------------------------------|
| gaze_smooth    | 0.12    | Gaze low-pass filter strength    |
| cursor_smooth  | 0.22    | Cursor low-pass filter strength  |
| edge_zone      | 0.08    | Edge expansion zone width        |
| edge_power     | 1.35    | Edge expansion nonlinearity      |
| sensor_offset_x| 0.0     | Horizontal sensor offset         |
| sensor_offset_y| 0.0     | Vertical sensor offset           |
| screen_distance| 63.0    | Viewing distance                 |
| screen_tilt    | 0.0     | Screen tilt angle in degrees     |

---

# eyemouse

```bash
eyemouse [options] [config_file]
```

### Options

| Option              | Description                                          |
|---------------------|------------------------------------------------------|
| `-h --help`         | Show command line help                               |
| `--usegyro`         | Enable Quha gyro, grab Quha exclusively              |
| `--leaveXgyro`      | Enable Quha gyro, leave Quha as X11 mouse            |
| `--blink`           | Enable blink click                                   |
| `--blink-left <ms>` | Left click threshold in ms (default: 80)             |
| `--blink-right <ms>`| Right click threshold in ms (default: 300)           |
| `--blink-hold <ms>` | Hold toggle threshold in ms (default: 600)           |
| `--debug`           | Verbose debug output on stderr                       |
| `--debuggaze`       | Verbose gaze output on stderr (high frequency)       |
| `--debuggyro`       | Verbose gyro event output on stderr (high frequency) |
| `--debugblink`      | Verbose blink event output on stderr                 |

### Examples

```bash
# default calibration
eyemouse

# custom calibration file
eyemouse ~/.local/tobii_4c/calx11.conf

# with Quha gyro
eyemouse --usegyro

# gyro with custom calibration
eyemouse --usegyro tobii.conf

# blink click enabled
eyemouse --blink

# blink with custom thresholds
eyemouse --blink --blink-left 100 --blink-right 350 --blink-hold 650

# blink + gyro
eyemouse --blink --usegyro

# debug mode
eyemouse --debug

# debug mode with gyro
eyemouse --debug --usegyro

# gaze debug (high frequency)
eyemouse --debuggaze

# gyro event debug (high frequency)
eyemouse --debuggyro

# blink debug
eyemouse --blink --debugblink

# all debug output to log file
eyemouse --debug --debuggaze --debuggyro --debugblink 2>eyemouse.log
```

---

# Blink Click

`eyemouse --blink` enables eye blink gesture recognition for mouse clicks.

Blink duration determines the action:

| Duration                        | Action          |
|---------------------------------|-----------------|
| < `--blink-left` (default 80ms) | Noise, ignored  |
| 80ms – 300ms                    | Left click      |
| 300ms – 600ms                   | Right click     |
| ≥ 600ms                         | Hold toggle     |

Hold toggle alternately presses and releases the left mouse button,
enabling drag operations without physical input.

All thresholds are configurable:

```bash
eyemouse --blink --blink-left 100 --blink-right 350 --blink-hold 650
```

Requires `libXtst` (`-lXtst`) at build time.

### Example output

```
Screen: 1920x1080
Device: tobii-prp://<serial>
Gyro disabled
Mouse 1365  150
```

---

# 3x3 Barycentric Calibration Mesh

The 9 calibration points form a triangulated mesh:

```text
P00  P01  P02
P10  P11  P12
P20  P21  P22
```

The mesh is split into 8 triangles. For every gaze sample:

1. The containing triangle is selected
2. Barycentric weights are calculated
3. Local warped screen coordinates are interpolated

This improves:

- Fullscreen reachability
- Edge precision
- Corner access
- Asymmetric distortion correction
- Local edge compensation

Without mesh correction, raw gaze mapping typically causes:

- Compressed edges
- Unstable corners
- Asymmetric fullscreen reachability
- Poor border access

---

# Runtime Smoothing

The runtime applies two-stage exponential smoothing:

```text
gaze_smooth   = 0.12   (gaze low-pass)
cursor_smooth = 0.22   (cursor low-pass)
```

A deadzone of 3 pixels suppresses micro-jitter when the
eye is stationary.

This improves:

- Cursor stability
- Accessibility usage
- Fullscreen usability
- Small target interaction

---

# Quha Support

The Quha device is automatically detected via:

```
/dev/input/by-id/
```

When `--usegyro` is active, the Quha device is grabbed exclusively
using `EVIOCGRAB`. This prevents X11 from receiving raw Quha mouse
events in parallel while eyemouse controls the cursor.

When `--leaveXgyro` is used instead, the Quha gyro input is read
but the Quha is **not** grabbed — it continues to operate as a
normal X11 mouse alongside the eyetracker.

Without `--usegyro` or `--leaveXgyro`, the Quha is not touched
and operates normally as an X11 mouse.

If the exclusive grab fails, a warning is printed and eyemouse
continues — but X11 may still receive Quha events simultaneously.

On `SIGTERM`, `SIGINT`, or `SIGHUP` the grab is released automatically
before exit, so the Quha resumes normal X11 mouse operation immediately.

## Gyro mode

Both `--usegyro` and `--leaveXgyro` read Quha movement events and apply
them as a relative offset on top of the Tobii gaze position. The offset
decays exponentially (factor 0.92 per frame), so gaze remains the
primary positioning input.

---

# Debug Output

`eyemouse` supports two independent debug flags:

| Flag            | Prefix    | Content                                              |
|-----------------|-----------|------------------------------------------------------|
| `--debug`       | `[DBG]`   | Init, Quha grab, XWarpPointer calls                  |
| `--debuggaze`   | `[GAZE]`  | Every gaze sample, warp/triangle selection, uvw, frame stats |
| `--debuggyro`   | `[GYRO]`  | Every REL_X/REL_Y gyro event with value and accumulated offset |
| `--debugblink`  | `[BLINK]` | Eye open/close events, duration, action taken        |

Both flags write to stderr and can be combined:

```bash
eyemouse --debug 2>debug.log
eyemouse --debuggaze 2>gaze.log
eyemouse --debuggyro 2>gyro.log
eyemouse --debug --debuggaze --debuggyro 2>full.log
eyemouse --debuggaze 2>&1 | grep GAZE
eyemouse --debuggyro 2>&1 | grep GYRO
```

`--debuggaze` and `--debuggyro` are high frequency and should be
redirected to a file when used together with `--debug`.

---

# X11 Requirement

Both tools require an active X11 session.
Wayland is currently not supported.

Verify your session type:

```bash
echo $XDG_SESSION_TYPE
# must output: x11
```

---

# Recalibration

Run `eyecalib` again at any time to recalibrate.
The previous calibration file will be overwritten.

The calibration file is plain text and can also be edited manually:

```
~/.local/tobii_4c/calx11.conf
```

---

# Experimental / Incomplete Features

Partially implemented or experimental:

- Bootstrap geometry import
- C12 runtime
- 3D gaze reprojection
- Head compensated mapping
- Projective warp
- Adaptive mesh refinement
- Binocular convergence estimation
- Advanced geometry correction

These are not fully active in the stable runtime.

---

# Future Direction

Planned future improvements:

- Projective warp / homography
- Bilinear mesh interpolation
- Geometry-aware reprojection
- Headpose compensation
- Dynamic runtime calibration
- Adaptive edge expansion
- Improved corner precision

---

# Files

| Path | Description |
|------|-------------|
| `~/.local/tobii_4c/calx11.conf` | Calibration and runtime config |
| `/usr/lib/tobii/` | Tobii Stream Engine shared libraries |
| `/usr/include/tobii/` | Tobii Stream Engine headers |
| `/etc/ld.so.conf.d/tobii.conf` | Library path config |
| `/etc/systemd/system/tobii_engine.service` | Tobii engine service |
| `/etc/systemd/system/tobiiusb.service` | Tobii USB service |
| `tobii.conf` | Library path source file (copied by install.sh) |
| `examples/` | Usage examples |

---

# License

MIT-style open source / research-oriented project.
