#!/bin/bash
# Android gate: the shared C core on Linux (same POSIX ports as Android) with
# the fake-band loopback test under ASan/UBSan, then the APK when an SDK exists.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
mkdir -p build/tmp
export TMPDIR="$root/build/tmp/"
bash scripts/build-core.sh linux
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 build/core-linux/loopback_test
if [[ -n "${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}" ]]; then
    (cd Android && ./gradlew --no-daemon assembleDebug)
else
    echo "ANDROID_HOME not set: skipped the APK build (needs the Android SDK, NDK and CMake)." >&2
fi
