#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
plugin_dir="${M4CAPTURE_PLUGIN_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/m4capture}"

usage() {
    cat <<'EOF'
Usage: ./scripts/install.sh <--debug|--release|--relwithdebinfo>

  --debug           Unoptimized build with debug symbols.
  --release         Optimized release build.
  --relwithdebinfo   Optimized build with debug symbols, useful for profiling.
  --help            Show this help.

Choose a build mode. Run the same command to update an installation.
Close OBS before installing, then reopen it to load the new library.
No sudo is needed. Flatpak OBS is not supported by this script.

Optional environment variables:
  M4CAPTURE_BUILD_DIR   Build directory (default: project/out/build-<mode>-v<version>)
  M4CAPTURE_PLUGIN_DIR  Individual plugin install directory
                       (default: $XDG_CONFIG_HOME/obs-studio/plugins/m4capture,
                        falling back to ~/.config/obs-studio/plugins/m4capture)
EOF
}

if (( $# != 1 )); then
    echo "Specify a build mode: --debug, --release, or --relwithdebinfo." >&2
    usage >&2
    exit 2
fi

case "$1" in
    --debug) build_type=Debug ;;
    --release) build_type=Release ;;
    --relwithdebinfo) build_type=RelWithDebInfo ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "Unknown option: $1. Specify --debug, --release, or --relwithdebinfo." >&2
        usage >&2
        exit 2
        ;;
esac

project_version="$(sed -nE 's/^project\(M4Capture VERSION ([^ ]+) .*/\1/p' "$project_dir/CMakeLists.txt")"
build_dir="${M4CAPTURE_BUILD_DIR:-$project_dir/out/build-${1#--}-v$project_version}"

require_obs_closed() {
    if pgrep -u "$(id -u)" -x obs >/dev/null; then
        echo "Close OBS before installing or updating M4Capture." >&2
        exit 1
    fi
}

require_obs_closed
"$project_dir/scripts/build.sh" "$1"
require_obs_closed
cmake --install "$build_dir" --prefix "$plugin_dir"
printf '\nInstalled M4Capture (%s) to %s\n' "$build_type" "$plugin_dir"
echo "Open OBS, then Sources > + > Window Capture, Desktop Capture, or Application Audio Capture (M4Capture)."
