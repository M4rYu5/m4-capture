#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
    cat <<'EOF'
Usage: ./scripts/build.sh <--debug|--release|--relwithdebinfo>

  --debug           Unoptimized build with debug symbols.
  --release         Optimized release build.
  --relwithdebinfo   Optimized build with debug symbols, useful for profiling.
  --help            Show this help.

Builds a ready-to-copy m4capture folder in out/build-<mode>-v<version>/package/.
OBS can stay open while building. Close it before copying the folder into
your native OBS plugins directory. No sudo is needed.

Optional environment variable:
  M4CAPTURE_BUILD_DIR  Build directory (default: project/out/build-<mode>-v<version>)
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

cmake -S "$project_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type"
cmake --build "$build_dir" --target m4capture --parallel
package_dir="$(cd -- "$build_dir" && pwd)/package/m4capture"
cmake --install "$build_dir" --prefix "$package_dir"

printf '\nBuilt M4Capture (%s): %s\n' "$build_type" "$package_dir"
echo "Close OBS, then copy this m4capture folder into your native OBS plugins directory."
