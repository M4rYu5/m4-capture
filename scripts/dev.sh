#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${WAYLANDCONNECT_BUILD_DIR:-$project_dir/build}"
plugin_dir="${WAYLANDCONNECT_PLUGIN_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/obs-studio/plugins/waylandconnect}"

usage() {
    cat <<'EOF'
Usage: ./scripts/dev.sh <build|install|update|help>

  build    Compile with debug symbols; do not install.
  install  Build and install for the current user's native OBS.
  update   Rebuild and replace the installed plugin (same as install).

Close OBS before install/update, then reopen it to load the new library.
No sudo is needed. Flatpak OBS is not supported by this script.

Optional environment variables:
  WAYLANDCONNECT_BUILD_DIR   Build directory (default: project/build)
  WAYLANDCONNECT_PLUGIN_DIR  Individual plugin install directory
                            (default: $XDG_CONFIG_HOME/obs-studio/plugins/waylandconnect,
                             falling back to ~/.config/obs-studio/plugins/waylandconnect)
EOF
}

require_obs_closed() {
    if pgrep -u "$(id -u)" -x obs >/dev/null; then
        echo "Close OBS before installing or updating KCow." >&2
        exit 1
    fi
}

build() {
    cmake -S "$project_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build "$build_dir" --parallel
}

case "${1:-help}" in
    build)
        build
        ;;
    install|update)
        require_obs_closed
        build
        require_obs_closed
        cmake --install "$build_dir" --prefix "$plugin_dir"
        printf '\nInstalled KCow to %s\n' "$plugin_dir"
        echo "Open OBS, then Sources > + > KCow (KWin Capture)."
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac
