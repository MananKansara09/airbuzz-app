#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
mkdir -p build/tmp
export TMPDIR="$root/build/tmp/"
export CLANG_MODULE_CACHE_PATH="$root/build/ModuleCache"
bash scripts/build-core.sh macos
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 build/core-macos/loopback_test
bash scripts/build-core.sh ios
xcodegen generate --spec App/project.yml
xcodebuild -project App/AirBuzz.xcodeproj -scheme AirBuzz \
    -destination 'generic/platform=iOS' -configuration Debug \
    -derivedDataPath "$root/build/DerivedData" CODE_SIGNING_ALLOWED=NO build
