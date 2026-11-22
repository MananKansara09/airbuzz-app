#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
platform="${1:-}"
case "$platform" in
    macos|ios) ;;
    *) echo "Usage: $0 <macos|ios>" >&2; exit 2 ;;
esac
output="$root/build/core-$platform"
mkdir -p "$root/build/tmp"
export TMPDIR="$root/build/tmp/"
args=(
    -S "$root" -B "$output" -G Ninja
    -C "$root/Vendor/golden-gate/xp/config/profiles/macOS_cmdline.cmake"
    -DGG_CONFIG_MBEDTLS_VERSION=2
    -DGG_ENABLE_UNIT_TESTS=OFF
    -DGG_ENABLE_APPS=OFF
    -DGG_ENABLE_EXAMPLES=OFF
    -DGG_BUILD_SHARED_LIBRARY=OFF
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    -DCMAKE_BUILD_TYPE=Release
    -DAB_SANITIZERS=ON
)
if [[ "$platform" == ios ]]; then
    args+=(-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64
           -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0)
fi
cmake "${args[@]}"
cmake --build "$output" --parallel 8
if [[ "$platform" == ios ]]; then
    libraries=()
    while IFS= read -r -d '' library; do
        libraries+=("$library")
    done < <(find "$output" -name '*.a' ! -name libairbuzz.a -print0)
    /usr/bin/libtool -static -o "$output/libairbuzz.a" "${libraries[@]}"
    cp "$root/Core/ab_session.h" "$output/ab_session.h"
fi
