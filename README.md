# KCow

Persistent window capture for OBS Studio on KDE Plasma Wayland.

KCow uses KWin's window-management and screencast interfaces with PipeWire video.

## Choose a window

Add **Sources → + → KCow (KWin Capture)** and open Properties. The **Window** dropdown
lists open windows by application ID and title. Selecting one fills the application
ID. **Refresh window list** refreshes the choices. There is no portal sharing chooser.

You can also enter an application ID while the application is closed. The saved
rule waits for that application instead of depending on a particular process or
window ID. Existing sources from the earlier portal version need a new selection
once; their scene items and last dimensions are preserved.

Properties include:

| Option | Behavior |
| --- | --- |
| Application ID | Exact application match; filled from the window dropdown or entered manually. |
| Title filter | Optional; empty matches any title in that application. |
| Title matching | Contains, exact, wildcard, or regular expression. |
| Case sensitive | Off by default. |
| Disconnect when title no longer matches | Off by default; require the captured window to keep matching the title filter. |
| Capture cursor | On by default; include the pointer in the video. |
| Connect on OBS startup | On by default; wait silently when the application is absent. |
| Automatically reconnect | On by default; reconnect when a matching window returns. |
| Move to top when focused | Off by default; raise the source item within its current scene/group. |
| Connect / retry capture | Start waiting/capture immediately, or retry an error. |
| Stop capture and waiting | Stay stopped until Connect is clicked, or until the next OBS startup when startup connection is enabled. |

By default, the title filter is used to find a target. Once connected, title and
focus changes keep the current window selected. Enable **Disconnect when title
no longer matches** to also enforce the title filter during capture. If the title
stops matching, another matching window can become the target. Without a matching
target, capture becomes transparent and waits when automatic reconnection is
enabled, or stops until Connect is clicked otherwise. A newly opened matching
window does not take over a capture whose title still matches. If the selected
window disappears and several windows match the saved rule, capture waits until
the rule is narrowed or a target is chosen.

Closing a window or losing its stream clears the video immediately. Disconnected
sources stay transparent and keep their last dimensions. A source with no captured
frames yet uses a 1280×720 default. Stream errors retry with a short delay when
automatic reconnection is enabled; normal waiting reacts to KWin window events.

## Move to top when focused

Enable this per source in Properties. When its captured window gains focus:

- Matching items in the current OBS scene move to the top of their immediate
  container. Items inside nested groups move within those groups.
- Parent groups keep their ordering relative to their siblings; they are not
  raised as a side effect of raising a child.
- Visibility, locking, transforms, and group membership are preserved.
- The new ordering remains when the window loses focus.
- A source reused in the current scene can raise each of its matching items.
  Items in other OBS scenes are untouched.

This changes the stacking order in OBS. It does not focus or raise application
windows on the desktop.

## Requirements

- Native Linux OBS Studio with its development headers, frontend API, and Qt 6 UI.
- KDE Plasma Wayland exposing `org_kde_plasma_window_management` version 17+ and
  `zkde_screencast_unstable_v1`.
- PipeWire and Wayland development headers, Qt 6 Widgets, a C++17 compiler,
  CMake 3.24+, Ninja, pkg-config, and `pgrep`.

On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf obs-studio procps-ng qt6-base pipewire wayland
```

The protocol XML files are included under `protocols/`; bindings are generated
with `wayland-scanner`. These are KDE implementation interfaces and can change
between Plasma versions. The plugin reports when access is unavailable. It does
not disable compositor permission checks or modify desktop security settings.
The installer targets native OBS (`/usr/bin/obs`), not Flatpak OBS.

## Install and update

Close OBS, then run from this folder:

```sh
./scripts/dev.sh install
```

After source changes, close OBS and run:

```sh
./scripts/dev.sh update
```

Reopen OBS to load the replacement library. Existing sources keep their source ID
(`waylandconnect_source`). Native plugins are not hot-reloaded. The script refuses
to replace the library while OBS is running and requires no sudo.

The module filename, install directory, and source ID retain their original
internal names so updates keep existing installations and saved scenes working.

Installed files:

```text
~/.config/obs-studio/plugins/waylandconnect/
├── bin/64bit/waylandconnect.so
└── data/locale/en-US.ini
```

`XDG_CONFIG_HOME` is respected. `WAYLANDCONNECT_PLUGIN_DIR` overrides the individual
plugin directory; `WAYLANDCONNECT_BUILD_DIR` overrides the build directory.
Compile without installing with `./scripts/dev.sh build`. Builds include debug
symbols. Launch `obs --verbose`, or `gdb --args obs --verbose`, for debugging.
OBS's current log contains `[KCow]` messages.

To remove the plugin, close OBS and remove its individual plugin directory.

## Development checks

```sh
cmake -S . -B build -DWAYLANDCONNECT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Window matching, frame conversion, frame handoff, and capture-controller tests
run without a desktop session. The controller tests use a simulated KWin Wayland
server and an isolated PipeWire daemon with generated video; they do not link OBS
or initialize graphics. Run these checks with:

```sh
env -u WAYLAND_DISPLAY -u DISPLAY ctest --test-dir build -R 'window-rule|video-frame|frame-buffer|capture-controller' --output-on-failure
```

The `obs-integration` tests exercise settings, cursor modes, rendering, and
focus-driven ordering in real OBS scenes/groups. They use simulated capture
servers and the running Wayland session to initialize libobs graphics.

An opt-in live test captures only its own temporary window, closes it, and opens
a replacement to check real KWin reconnection:

```sh
QT_QPA_PLATFORM=wayland WAYLANDCONNECT_LIVE_TEST=1 ./build/waylandconnect-tests liveKWinCapture
```

## Implementation

- `src/kwin-connection.cpp`: window discovery, focus events, and KWin streams.
- `src/window-rule.cpp`: application/title matching and stable selection.
- `src/capture.cpp`: capture lifecycle and automatic reconnection, without OBS dependencies.
- `src/capture-runtime.cpp`: shared KWin connection, window snapshots, and queued controller ownership.
- `src/obs-capture.cpp`: OBS settings, persistence, logging, and focus-action callbacks.
- `src/focus-order.cpp`: ordering items inside their existing containers.
- `src/pipewire-capture.cpp`: mapped BGRA/RGBA frames and disconnect handling.
- `src/frame-buffer.cpp`: synchronized frame handoff between capture and rendering.
- `src/waylandconnect-source.cpp`: OBS properties, saved configuration, and rendering.

Capture configuration and diagnostics are accessed through synchronized snapshots.
The frame buffer has its own synchronization; controller ownership and callbacks
stay on the Qt UI thread. Callbacks hold weak OBS references, so they do not keep
removed sources alive.

The controller keeps user intent (`connectionEnabled`) separate from stream status:

| Event | Transition |
| --- | --- |
| Startup with automatic start, or Connect | Wait for a unique target, then connect. |
| First frame | Connecting → Capturing. |
| Stream pauses | Capturing → Paused; clear video and retain the selected window. |
| Frames resume | Paused → Capturing without choosing another target. |
| Target disappears, or an enforced title filter fails | Wait/reconnect when enabled; otherwise stop. |
| Stream failure | Failed; retry after a delay when reconnection is enabled. |
| Stop | Any status → Idle; disarm waiting and retries. |

Configuration edits preserve a manual stop. Cursor changes restart the stream
while retaining the current window; target-rule edits re-evaluate selection.
Old stream callbacks are ignored using an attempt generation incremented before teardown.

Video uses a shared-memory CPU copy path; GPU-only buffers and HDR are unsupported.
High-resolution/frame-rate performance has not been profiled.

## License

KCow is licensed under the GNU General Public License, version 3 or (at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full text.

The bundled Wayland protocol files retain their upstream licenses; see [protocols/README.md](protocols/README.md).
