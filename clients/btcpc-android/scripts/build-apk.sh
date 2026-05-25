#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ANDROID_DIR="$(dirname "$SCRIPT_DIR")/android"

if [ -z "${ANDROID_HOME:-}" ] && [ -z "${ANDROID_SDK_ROOT:-}" ]; then
  echo "ERROR: ANDROID_HOME or ANDROID_SDK_ROOT is not set."
  echo "Install Android command-line tools or Android Studio and set the env var."
  exit 1
fi

echo "Building debug APK..."
cd "$ANDROID_DIR"
./gradlew assembleDebug

APK="app/build/outputs/apk/debug/app-debug.apk"
if [ -f "$APK" ]; then
  echo ""
  echo "APK built: $ANDROID_DIR/$APK"
  ls -lh "$APK"
else
  echo "ERROR: APK not found at $APK"
  exit 1
fi
