#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ANDROID_DIR="$(dirname "$SCRIPT_DIR")/android"
WEBSITE_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")/website"

if [ -z "${ANDROID_HOME:-}" ] && [ -z "${ANDROID_SDK_ROOT:-}" ]; then
  echo "ERROR: ANDROID_HOME or ANDROID_SDK_ROOT is not set."
  exit 1
fi

if [ -z "${BTCPC_KEYSTORE:-}" ]; then
  echo "ERROR: BTCPC_KEYSTORE not set (path to release keystore)."
  echo "Generate one with:"
  echo "  keytool -genkey -v -keystore btcpc-release.jks -keyalg RSA -keysize 2048 -validity 10000 -alias btcpc"
  exit 1
fi

echo "Building release APK..."
cd "$ANDROID_DIR"
./gradlew assembleRelease

APK="app/build/outputs/apk/release/app-release.apk"
if [ -f "$APK" ]; then
  if [ -d "$WEBSITE_DIR" ]; then
    cp "$APK" "$WEBSITE_DIR/btcpc.apk"
    echo "Copied to $WEBSITE_DIR/btcpc.apk"
  fi
  echo ""
  echo "Release APK: $ANDROID_DIR/$APK"
  ls -lh "$APK"
else
  echo "ERROR: Release APK not found at $APK"
  exit 1
fi
