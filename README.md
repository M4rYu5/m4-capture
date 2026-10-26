# WaylandConnect

A native OBS Studio source for Wayland window/screen capture with a saved portal
selection and a manual reconnect button.

## Capture behavior

- Adding a new **WaylandConnect** source opens the desktop's sharing chooser.
  Select one window or screen and allow restoring on future sessions.
- The restore token, selection identifier, and last captured dimensions are saved
  with the source in the OBS scene collection.
- Saved sources load disconnected, without opening a chooser at OBS startup.
- Click the **Connect / retry capture** button immediately before the source's eye
  button to start or retry the saved capture. Visibility remains a separate control.
- A failed/cancelled request keeps the saved selection and leaves the source empty.
  A successful portal restore rotates its single-use token immediately, including
  when the subsequent video connection fails.
- Closing the captured window, losing its PipeWire node, or closing the portal
  session clears the video. The source keeps its last width/height for scene sizing.
- Open source **Properties → Choose window or screen…** to select another target.
  Cancelling keeps the previous selection. Properties also provide Connect and
  the current connection status.

The portal controls whether a saved selection can be restored. If it cannot,
KDE may show its chooser again after you click Connect. There is no portal option
that forces a silent failure instead of that chooser. Keeping a token locally
cannot make an expired, revoked, or consumed token valid again. See the
[ScreenCast portal specification](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html).

Reconnection is user-triggered; there is no automatic retry loop. The source stays
transparent while disconnected. Before its first frame it has a 1280×720 default
size; afterward it retains the actual captured size.

## Requirements

- Native Linux OBS Studio with its development headers, frontend API, and Qt 6 UI.
- A Wayland session with an XDG ScreenCast portal version 4+ (for saved selections).
- PipeWire development headers and Qt 6 Widgets/DBus development libraries.
- A C++17 compiler, CMake 3.24+, Ninja, pkg-config, and `pgrep` (procps-ng).

On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf obs-studio procps-ng qt6-base pipewire xdg-desktop-portal-kde
```

This setup targets the system OBS installation (`/usr/bin/obs`). The install script
does not install into Flatpak OBS; that needs a build against its runtime and
separate packaging.

## Install and add the source

Close OBS, then run from this folder:

```sh
./scripts/dev.sh install
```

Open OBS and select **Sources → + → WaylandConnect**, then choose a capture in the
Wayland sharing dialog. Enable **Allow restoring on future sessions** so the portal
returns a token. If persistence is not granted, the attempt ends without replacing
the previous saved selection.

The script installs only this plugin under:

```text
~/.config/obs-studio/plugins/waylandconnect/
├── bin/64bit/waylandconnect.so
└── data/locale/en-US.ini
```

It respects `XDG_CONFIG_HOME`. No system files or OBS scene collections are changed
by the installer, and it does not need sudo. OBS documents the per-user plugin
location in its [plugins guide](https://obsproject.com/kb/plugins-guide).

## Update while developing

Edit the source, close OBS, then run:

```sh
./scripts/dev.sh update
```

Reopen OBS to load the replacement library. Existing WaylandConnect scene items
continue to use the same source ID (`waylandconnect_source`). OBS does not hot-reload
native plugins. The script refuses to install while your OBS process is running.

To compile without installing:

```sh
./scripts/dev.sh build
```

Builds include debug symbols (`CMAKE_BUILD_TYPE=Debug`). For verbose OBS logs:

```sh
obs --verbose
```

Use **Help → Log Files → View Current Log** and look for `[WaylandConnect]`.
For a debugger, launch `gdb --args obs --verbose` after installing the debug build.

To stage an installation inside the project instead of the OBS configuration:

```sh
WAYLANDCONNECT_PLUGIN_DIR="$PWD/build/staged/waylandconnect" ./scripts/dev.sh install
```

`WAYLANDCONNECT_BUILD_DIR` overrides the build directory. Direct CMake builds also
work; `cmake --install build --prefix <directory>` treats that directory as the
individual plugin folder containing `bin/64bit` and `data`.

## Remove

Close OBS and remove the `waylandconnect` directory from your user plugin folder.
Remove its sources from saved scenes if you no longer want them there.

## Implementation and limits

The plugin uses the standard XDG ScreenCast portal and PipeWire. It does not need
direct access to KWin's private capture interfaces. Capture currently negotiates
mapped BGRA/RGBA shared-memory buffers, copies them to an OBS texture, and supports
stride, crop, and rotation metadata. GPU-only buffers and HDR formats are not
supported. This CPU copy path prioritizes a straightforward implementation; its
performance has not been profiled for high-resolution/high-frame-rate capture.

OBS has no public API for inserting a control beside the source visibility icon.
The row button uses Qt widget introspection for OBS 32's source-list structure,
including source lookup through the accessibility role. It avoids private OBS C++
symbols, but an OBS UI redesign can require updating this integration. The
properties Connect button remains available independently.

## Development checks

Optional integration tests use a private D-Bus portal simulator and a temporary
PipeWire daemon with generated video. They check cancellation, token rotation,
saved-source loading, row-button placement, rendering, disconnect clearing, and
manual reconnection. They do not capture the desktop or open the real chooser.
They need access to a running Wayland session to initialize libobs graphics.

```sh
cmake -S . -B build -DWAYLANDCONNECT_BUILD_TESTS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Project layout

- `src/plugin.cpp`: module initialization and source registration.
- `src/waylandconnect-source.cpp`: source lifecycle, saved settings, and rendering.
- `src/capture.cpp`: asynchronous portal requests and capture session lifecycle.
- `src/pipewire-capture.cpp`: PipeWire video transport and disconnect handling.
- `src/source-list.cpp`: Connect button beside the visibility control.
- `data/locale/en-US.ini`: text shown in OBS.
- `scripts/dev.sh`: debug build and user installation/update.

## License

WaylandConnect is licensed under the GNU General Public License, version 3 or (at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full text.
