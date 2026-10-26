# WaylandConnect

An OBS Studio plugin project for KDE Plasma Wayland window capture that can
eventually wait silently for an application and reconnect when its window returns.

The current implementation is a **development scaffold**: it registers a video
source named **WaylandConnect**, with a teal placeholder and adjustable dimensions.
It does not capture windows, use PipeWire/KWin, or implement reconnection yet.
It does not open permission dialogs.

## Requirements

- Native Linux OBS Studio with its development headers and CMake package.
- A C compiler, CMake 3.24+, Ninja, and `pgrep` (procps-ng).

On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja obs-studio procps-ng
```

This setup targets the system OBS installation (`/usr/bin/obs`). The install script
does not install into Flatpak OBS; that needs a build against its runtime and
separate packaging.

## Install and add the source

Close OBS, then run from this folder:

```sh
./scripts/dev.sh install
```

Open OBS and select **Sources → + → WaylandConnect**. The properties show the
development status and placeholder size. The initial size is 1280×720.
The source can be added to scenes and saved normally.

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

## Project layout

- `src/plugin.c`: module initialization and source registration.
- `src/waylandconnect-source.c`: source lifecycle, properties, and placeholder.
- `data/locale/en-US.ini`: text shown in OBS.
- `scripts/dev.sh`: debug build and user installation/update.

Future capture work belongs behind this source's lifecycle. The intended behavior
is to remember an application ID/window class and optional title filter, wait
without prompting when no matching window exists, and reconnect when one appears.
No capture backend has been selected or implemented in this scaffold.

## License

WaylandConnect is licensed under the GNU General Public License, version 3 or (at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full text.
