# Developing M4Capture

For installation and everyday use, see the [README](../README.md).
Run the commands below from the project directory.

## Build requirements

- Linux, OBS Studio 32+ development headers and frontend API, and Qt 6 Core.
- PipeWire 1.2+, Wayland, libdrm, and OpenGL development headers.
- A C++17 compiler, CMake 3.24+, Ninja, pkg-config, `wayland-scanner`, and `pgrep`.
- For unit tests: Qt 6 Test. Protocol integration tests also need Wayland server
  development headers and the `pipewire` executable. OBS tests, live checks and
  benchmarks additionally need Qt 6 Widgets, EGL, and a Wayland graphics session.

See the [installation instructions](../README.md#install) for Arch Linux packages.

## Runtime protocol requirements

- Window and application audio capture require KWin's
  `org_kde_plasma_window_management` version 17+.
- Window and desktop video capture require `zkde_screencast_unstable_v1`.
- Desktop capture also requires XDG output protocol version 2+; capturing all
  displays requires screencast protocol version 3+.

The protocol XML files are included under `protocols/`; bindings are generated
with `wayland-scanner`. These are KDE implementation interfaces and can change
between Plasma versions. The plugin reports when access is unavailable. It does
not disable compositor permission checks or modify desktop security settings.

## Build and install

Close OBS before installing. Choose a build mode:

```sh
./scripts/install.sh --debug
./scripts/install.sh --release
./scripts/install.sh --relwithdebinfo
```

Run only the command for the desired mode. Debug is unoptimized with debug
symbols; Release is optimized; RelWithDebInfo is optimized with debug symbols.
A build mode is required. Run the same command after source changes to update
the installation, then reopen OBS. The installer requires no sudo and targets
native OBS, not Flatpak OBS.

Each mode uses a separate build directory: `out/build-debug-v0.3.1`, `out/build-release-v0.3.1`, or
`out/build-relwithdebinfo-v0.3.1`. The scripts read the version from `CMakeLists.txt`.
`M4CAPTURE_BUILD_DIR` overrides this directory; keep custom paths under `out/`.
`M4CAPTURE_PLUGIN_DIR` overrides the individual plugin install directory, which
defaults to `${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/m4capture`.

The installer calls `scripts/build.sh` to build the plugin and stage a copy in
`<build-directory>/package/m4capture/`, then installs it into OBS. To build only
the ready-to-copy folder, run `./scripts/build.sh --release` (or choose `--debug`
or `--relwithdebinfo`). OBS can remain open for this build-only step.

Installed files:

```text
m4capture/
├── bin/64bit/m4capture.so
└── data/locale/en-US.ini
```

Compile without installing with:

```sh
cmake -S . -B out/build-debug-v0.3.1 -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build-debug-v0.3.1 --parallel
```

Launch `obs --verbose`, or `gdb --args obs --verbose`, for debugging a Debug build.
OBS's current log contains `[M4Capture]` messages.

The saved source IDs are `m4capture_source`, `m4capture_desktop_source`, and
`m4capture_audio_source`. There are no compatibility aliases or settings migration
for earlier plugin names. Old saved startup/reconnection switches are ignored
and removed when the source is saved.

## Development checks

```sh
cmake -S . -B out/build-tests -G Ninja -DCMAKE_BUILD_TYPE=Debug -DM4CAPTURE_BUILD_TESTS=ON
cmake --build out/build-tests --parallel
ctest --test-dir out/build-tests --output-on-failure
```

The test suites are organized by dependencies and responsibility:

| Directory | Scope | CTest labels |
| --- | --- | --- |
| `tests/unit/` | Matching, frame conversion, frame handoff, capture configuration | `unit`, `headless` |
| `tests/integration/` | KWin protocols, controller lifecycle, PipeWire video and audio | `integration`, `headless` |
| `tests/obs/` | Source settings, visibility, audio, focus ordering, rendering and DMA-BUF imports | `obs`, `graphics`; imports also have `gpu` |
| `tests/live/` | Real compositor capture, recovery and performance diagnostics | Explicit command, outside CTest |
| `tests/support/` | Shared servers, producers, scoped state and suite fixtures | Shared libraries |

Each suite has its own executable at the build root, such as
`capture-controller-tests` or `source-settings-tests`. Select groups by label:

```sh
ctest --test-dir out/build-tests -L unit --output-on-failure
ctest --test-dir out/build-tests -L integration --output-on-failure
ctest --test-dir out/build-tests -L obs --output-on-failure
```

Unit and protocol integration tests run without a desktop session. Integration
tests use a simulated KWin Wayland server and an isolated PipeWire daemon with
generated media. `PipeWireServer`, `VideoProducer` and `AudioProducer` can be
used independently; `VideoCaptureFixture` combines the server and video producer
for the complete video capture path. Shared fixtures restore environment variables and release runtime
state between suites. To configure just the headless suites:

```sh
cmake -S . -B out/build-headless -G Ninja -DCMAKE_BUILD_TYPE=Debug -DM4CAPTURE_BUILD_TESTS=ON -DM4CAPTURE_BUILD_OBS_TESTS=OFF
cmake --build out/build-headless --parallel
env -u WAYLAND_DISPLAY -u DISPLAY ctest --test-dir out/build-headless -L headless --output-on-failure
```

The project still requires the plugin's OBS development dependencies at configure
time. The headless test targets do not initialize OBS or require Qt Widgets/EGL
as additional test dependencies. OBS suites use the running Wayland session for
libobs graphics. Renderer and import suites initialize graphics without starting
simulated capture servers.

`M4CAPTURE_BUILD_INTEGRATION_TESTS` and `M4CAPTURE_BUILD_OBS_TESTS` default to
`AUTO`, which follows `M4CAPTURE_BUILD_TESTS` on every configure, including when
enabling tests in an existing build directory. Explicit `ON` or `OFF` overrides
persist independently; set an option back to `AUTO` to restore the default.
Existing cached `ON`/`OFF` values are preserved. To build only unit tests, enable
`M4CAPTURE_BUILD_TESTS` and set both integration options to `OFF`.
Live tests and benchmarks share the opt-in `M4CAPTURE_BUILD_LIVE_TESTS` build
option, off by default. The live executable
requires KDE Plasma Wayland and `M4CAPTURE_LIVE_TEST=1`. Its window reconnection
check captures a temporary window, closes it, and opens a replacement:

```sh
cmake -S . -B out/build-tests -DM4CAPTURE_BUILD_LIVE_TESTS=ON
cmake --build out/build-tests --target m4capture-live-tests --parallel
QT_QPA_PLATFORM=wayland M4CAPTURE_LIVE_TEST=1 ./out/build-tests/m4capture-live-tests liveM4Capture
```

When adding cases, keep one behavior or coherent transition sequence per test.
Use data rows for variations with the same setup and expectations. Keep assertions
in the test body, use observable readiness for asynchronous setup, and reserve
fixed observation periods for proving that an event does not occur. Keep explicit
expected pixel values alongside reference-renderer comparisons.

## Implementation

- `src/kwin-connection.cpp`: window/display discovery, focus events, and KWin streams.
- `src/window-rule.cpp`: application/title matching and stable selection.
- `src/capture.cpp`: capture lifecycle and automatic reconnection, without OBS dependencies.
- `src/capture-runtime.cpp`: shared KWin connection, window snapshots, and queued controller ownership.
- `src/obs-capture.cpp`: OBS settings, persistence, logging, and focus-action callbacks.
- `src/focus-order.cpp`: ordering items inside their existing containers.
- `src/pipewire-capture.cpp`: mapped BGRA/RGBA frames and disconnect handling.
- `src/pipewire-audio.cpp`: application playback discovery, capture links and stereo mixing.
- `src/obs-dmabuf.cpp`: cached GPU buffer imports and synchronization.
- `src/obs-video.cpp`: native CPU texture uploads and shared crop/rotation/flip drawing.
- `src/frame-buffer.cpp`: synchronized frame handoff between capture and rendering.
- `src/m4capture-source.cpp`: OBS properties, saved configuration, and rendering.

Capture configuration and diagnostics are accessed through synchronized snapshots.
The frame buffer has its own synchronization; controller ownership and callbacks
stay on the Qt UI thread. Callbacks hold weak OBS references, so they do not keep
removed sources alive.

The controller tracks matching windows while its source exists and captures only
while OBS considers it showing. Scene previews, properties previews and projectors
count as showing even when the source is absent from the program output.

| Event | Transition |
| --- | --- |
| Startup | Track a unique target; connect when showing. |
| Last view or preview closes | Clear video, close the capture stream and cancel capture retries; keep window tracking. |
| A view or preview opens | Start a new stream for the current target and wait for a fresh frame. |
| First frame | Connecting → Capturing. |
| Stream pauses | Capturing → Paused; clear video and retain the selected window. |
| Frames resume | Paused → Capturing without choosing another target. |
| Target disappears, or an enforced title filter fails | Clear video and wait for a matching target. |
| Stream failure | Failed; retry after two seconds while showing. |
| A window or application is selected | Re-evaluate the target; start capture if showing. |
| Application ID is cleared | Idle; clear video until a target is configured. |
| Source is removed | Release capture and cancel retries. |

There is no manual start/stop state or separate startup/reconnection policy.
Cursor changes restart the stream while retaining the current window; target-rule
edits re-evaluate selection. Unrelated settings edits leave a live stream intact.
Old stream callbacks are ignored using an attempt generation incremented before teardown.

Video prefers PipeWire DMA-BUFs supported by the OBS graphics device. KWin GPU
buffers are imported into OBS; crop, rotation, flipping, channel handling and
opaque-alpha handling happen on the GPU. Frames are drawn into reusable GPU
textures before returning the capture buffers, avoiding CPU pixel copies and
uploads while keeping buffer reuse independent of OBS rendering.

Explicit GPU acquire/release fences are negotiated when supported. With older
producers using implicit synchronization, GPU reads finish before their buffers
are returned. If GPU import is unavailable or fails, capture falls back to mapped
shared-memory capture and native BGRA/RGBA upload. CPU capture copies only the
cropped rows into owned memory; OBS's normal texture draw handles rotation,
flipping and opaque alpha. This uses the same basic graphics renderer as the
previous CPU fallback and does not require DMA-BUF import or GPU compute.
A failed GPU capture path stays disabled for that source's lifetime, including
reconnections; recreating the source or restarting OBS tries GPU capture again.
HDR remains unsupported.

For diagnosis, launch OBS with `M4CAPTURE_FORCE_CPU=1` to force the CPU path.
Logs identify `DMA-BUF GPU capture` or `CPU fallback capture`.

The opt-in benchmark captures its own animated window at 1920×1080 and 3840×2160,
with two seconds of warm-up and ten seconds of measurement per sample:

```sh
cmake -S . -B out/build-release-v0.3.1 -DCMAKE_BUILD_TYPE=Release -DM4CAPTURE_BUILD_LIVE_TESTS=ON
cmake --build out/build-release-v0.3.1 --target m4capture-live-tests --parallel
python3 scripts/benchmark-capture.py --runs 3
```

The script runs `m4capture-live-tests` to compare DMA-BUF and forced CPU fallback,
saves raw logs and JSON under `out/build-release-v0.3.1/`, and reports
median process CPU usage and prepared frame rates. The benchmark defaults to a
60 fps OBS output clock; `M4CAPTURE_TEST_FPS_NUM` and `M4CAPTURE_TEST_FPS_DEN`
override it. Set `M4CAPTURE_BENCHMARK_ANIMATION_MS=2` to update the captured
window independently of the 16 ms source-draw timer and profile a faster producer.
`--baseline /path/to/binary`
also measures a preserved pre-change benchmark executable.

`BENCHMARK_IMPORTS` in the raw logs reports imports created and destroyed during
the measurement window. A stable DMA-BUF buffer pool should reuse its imports
after warm-up. Imports are released on buffer removal, format changes, CPU
fallback, or stream teardown (including hiding the source).

For a short frame-rate diagnostic across an OBS output-rate change, run:

```sh
QT_QPA_PLATFORM=wayland M4CAPTURE_LIVE_TEST=1 ./out/build-release-v0.3.1/m4capture-live-tests liveFrameRateProfile
```

This logs prepared frames/s at two output rates without enforcing a capture FPS
limit. Set `M4CAPTURE_FORCE_CPU=1` to profile CPU fallback.
