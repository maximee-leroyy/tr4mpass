#!/bin/sh
# Wait for the iPhone to appear in NORMAL (iOS) mode, read its iOS
# version + build, then locate the matching IPSW on ipsw.me.
#
# Usage: detect_ios.sh [timeout_sec]
set -u

TIMEOUT="${1:-180}"
DEV_ID="iPhone8,1"   # iPhone 6s
START=$(date +%s)

echo "Waiting for the device in NORMAL mode (unplug/replug the cable now)..."

while :; do
    NOW=$(date +%s)
    ELAPSED=$((NOW - START))
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        echo "TIMEOUT after ${TIMEOUT}s: device never showed up in normal mode." >&2
        exit 1
    fi

    if idevice_id -l 2>/dev/null | grep -q .; then
        VER=$(ideviceinfo -k ProductVersion 2>/dev/null)
        BUILD=$(ideviceinfo -k BuildVersion 2>/dev/null)
        MODEL=$(ideviceinfo -k ProductType 2>/dev/null)
        if [ -n "${VER:-}" ] && [ -n "${BUILD:-}" ]; then
            echo "DETECTED version=${VER} build=${BUILD} model=${MODEL:-unknown}"
            printf '%s\n' "$VER" > /tmp/ios_version.txt
            printf '%s\n' "$BUILD" > /tmp/ios_build.txt
            printf '%s\n' "${MODEL:-$DEV_ID}" > /tmp/ios_model.txt
            exit 0
        fi
    fi

    sleep 2
done
