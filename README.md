# M4Capture

Capture windows, your desktop, and application audio in OBS Studio on KDE Plasma Wayland. M4Capture remembers your target and reconnects automatically when a window reopens or a display returns.

> [!NOTE]
> **AI disclosure:** This plugin was developed with AI assistance.

## Sources

M4Capture adds three sources to OBS Studio, available under **Sources**:

- **Window Capture (M4Capture)**: Capture an application window, optionally with its audio and cursor, and automatically move the capture above other sources in its OBS scene or group when the window gains focus.

  <p align="center">
    <img src="docs/images/window-capture-properties.png" alt="Window Capture (M4Capture) properties in OBS Studio" width="550">
  </p>

- **Desktop Capture (M4Capture)**: Capture one monitor or all displays together, optionally with the cursor.
- **Application Audio Capture (M4Capture)**: Capture an application's playback without video.

## Requirements

- Linux with **KDE Plasma running a Wayland session**.
- **Native OBS Studio 32 or newer**, using Qt 6.
- **PipeWire 1.2 or newer** for application playback and capture.

Flatpak OBS and HDR capture are not supported.

Compatibility depends on the interfaces provided by your Plasma session; see the [detailed compatibility requirements](docs/development-notes.md#runtime-protocol-requirements).

## Install

### OBS plugin location

Install the **m4capture** folder inside your OBS plugin directory. Create the directory if it does not exist.

| Configuration | Plugin directory |
| --- | --- |
| Default | `~/.config/obs-studio/plugins/` |
| With `XDG_CONFIG_HOME` set | `$XDG_CONFIG_HOME/obs-studio/plugins/` |

The installed folder should have this structure:

```text
m4capture/
├── bin/64bit/m4capture.so
└── data/locale/en-US.ini
```

### Download a prebuilt release

Download the ZIP containing the built plugin folder from [Releases](../../releases) and extract it.

Close OBS, then copy the entire **extracted m4capture** folder into your [OBS plugin directory](#obs-plugin-location). Reopen OBS to load the plugin.

### Build from source

Install the build dependencies first. On Arch Linux:

```sh
sudo pacman -S --needed base-devel cmake ninja pkgconf obs-studio procps-ng qt6-base pipewire wayland libdrm libglvnd
```

For other distributions, install the equivalent [build dependencies](docs/development-notes.md#build-requirements).

Download or clone this project's source and open a terminal in its directory. Choose one of the methods below.

#### Build and install automatically

Close OBS, then run:

```sh
./scripts/install.sh --release
```

Run the installer as your normal user, without sudo. Reopen OBS when it finishes.

#### Build and install manually

To build a folder you can copy into OBS yourself, run:

```sh
./scripts/build.sh --release
```

The script creates `out/build-release-v<version>/package/m4capture/`. Close OBS, then copy that folder into your [OBS plugin directory](#obs-plugin-location). Reopen OBS to load the plugin.

### Update or remove

To update, close OBS and repeat your installation method with the newer version, replacing the existing `m4capture` folder. Reopen OBS to load the update.

To remove M4Capture, close OBS and delete the installed `m4capture` folder.

If you previously installed this project under an earlier name, remove that old plugin installation while OBS is closed and recreate its sources with M4Capture. Earlier sources and settings are not migrated automatically.

## Troubleshooting

| Problem | What to check |
| --- | --- |
| M4Capture is missing from the source list | Close and reopen OBS after installation. Check that you are using native OBS 32+ and installed the plugin as the same user who runs OBS. |
| A window capture stays transparent | Open the target application, refresh the window list, and check the application ID and title filter. If several windows match, select one or narrow the filter. |
| A desktop capture stays transparent | Check that the selected monitor is connected, then refresh the display list. |
| Application audio is silent | Keep a matching window open and play sound through PipeWire. For Window Capture, enable **Capture application audio**. Check the source's volume and mute setting in OBS. |
| Audio sounds doubled | Disable other OBS sources recording the same application or desktop playback. |
| A CPU fallback warning appears | Capture can continue with higher CPU usage. Restart OBS or recreate the source to retry GPU capture. |

Capture runs while the source is visible in OBS, including previews and projectors. It resumes automatically when shown again.

For more detail about a problem, open the current OBS log and look for `[M4Capture]` messages.

## Development

See the [development notes](docs/development-notes.md) for build options, tests, benchmarks, and implementation details.

## License

M4Capture is licensed under the GNU General Public License, version 3 or (at your option) any later version (`GPL-3.0-or-later`). See [LICENSE](LICENSE) for the full text.

The bundled Wayland protocol files retain their upstream licenses; see [protocols/README.md](protocols/README.md).
