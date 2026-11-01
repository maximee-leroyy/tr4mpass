#!/bin/bash
#
# ramdisk_test_ibss.sh -- Path A step-3 smoke test (USB transition only).
#
# What it does, end to end:
#   1. Detect the installed iOS version from the device (normal mode,
#      ideviceinfo). If the device is in DFU instead, print the manual
#      unplug/replug instruction and wait for it to boot iOS.
#   2. Resolve the matching IPSW URL (ipsw.me) and pull ONLY the iBSS
#      image via HTTP Range requests (a few hundred KB, no 2+ GB
#      download).
#   3. Send the iBSS to the pwned DFU device via irecovery.
#   4. Watch the USB PID: 0x1227 (DFU) -> 0x1281 (recovery) proves the
#      transition survives on Apple Silicon USB-C direct connection --
#      the exact risk palera1n documents ("may require manual
#      unplugging and replugging of the lightning cable after checkm8").
#
# This script never touches the checkm8 stage: it ASSUMES the device is
# in pwned DFU ("PWND:[checkm8]" in the serial). Re-pwn first if not
# (./tr4mpass -v).
#
# Requirements: irecovery, idevice_id/ideviceinfo (libimobiledevice),
# python3. All non-fatal details go to stderr; the final line is
# PASS/FAIL so the script can be chained.
#
# Usage: scripts/ramdisk_test_ibss.sh [--device iPhone8,1] [--keep-images]
set -u

DEVICE_ID="iPhone8,1"          # iPhone 6s (CPID 0x8000)
KEEP_IMAGES=0
WORKDIR="${TR4MPASS_IMAGES_DIR:-$HOME/.cache/tr4mpass/images}"

while [ $# -gt 0 ]; do
    case "$1" in
        --device)      DEVICE_ID="$2"; shift 2 ;;
        --keep-images) KEEP_IMAGES=1;   shift ;;
        -h|--help)     sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }
info() { echo "[iBSS-test] $*"; }

command -v irecovery    >/dev/null 2>&1 || fail "irecovery not found (brew install libirecovery)"
command -v idevice_id   >/dev/null 2>&1 || fail "idevice_id not found (brew install libimobiledevice)"
command -v python3      >/dev/null 2>&1 || fail "python3 not found"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ------------------------------------------------------------------
# 0. Preconditions: pwned DFU right now?
# ------------------------------------------------------------------
PWND="$(ioreg -p IOUSB -l -w0 2>/dev/null | grep -i 'USB Serial Number' | grep -c 'PWND:' || true)"
if [ "$PWND" -eq 0 ]; then
    info "device is NOT in pwned DFU. Put it in DFU and run ./tr4mpass -v first."
    exit 1
fi
info "device is in pwned DFU (serial contains PWND:)"

# ------------------------------------------------------------------
# 1. Installed iOS version. The device must boot iOS for ideviceinfo;
#    ask for the cable replug if it is still sitting in DFU.
# ------------------------------------------------------------------
VERSION_FILE=""
BUILD_FILE=""
MODEL_FILE=""
if [ -f /tmp/ios_version.txt ] && [ -f /tmp/ios_build.txt ]; then
    VERSION_FILE=/tmp/ios_version.txt
    BUILD_FILE=/tmp/ios_build.txt
    MODEL_FILE=/tmp/ios_model.txt
    info "reusing cached detection: $(cat "$VERSION_FILE") ($(cat "$BUILD_FILE"))"
else
    info "device must boot iOS once so we can read its version."
    info ">>> UNPLUG and REPLUG the cable now (device boots to iOS)."
    "$SCRIPT_DIR/detect_ios.sh" 300 || \
        fail "could not detect iOS version (no normal-mode device seen)"
    VERSION_FILE=/tmp/ios_version.txt
    BUILD_FILE=/tmp/ios_build.txt
    MODEL_FILE=/tmp/ios_model.txt
fi

IOS_VERSION="$(cat "$VERSION_FILE")"
IOS_BUILD="$(cat "$BUILD_FILE")"
DETECTED_MODEL="$(cat "$MODEL_FILE" 2>/dev/null || echo "$DEVICE_ID")"
info "iOS ${IOS_VERSION} (build ${IOS_BUILD}) on ${DETECTED_MODEL}"

# ------------------------------------------------------------------
# 2. Pull iBSS (and iBEC for the next step) via HTTP Range.
# ------------------------------------------------------------------
mkdir -p "$WORKDIR"
IPSW_URL="$(python3 "$SCRIPT_DIR/ipsw_pull.py" url-for "$DETECTED_MODEL" "$IOS_BUILD")" \
    || fail "could not resolve IPSW URL for build $IOS_BUILD"
info "IPSW: $IPSW_URL"

# NOTE: the iPhone 6s (iPhone8,1) IPSW carries TWO iBSS images --
# iBSS.n71 (TSMC A9) and iBSS.n71m (Samsung A9, dual-sourced chip).
# The device BDID selects the right one; a wrong-board image is simply
# not booted, so the script tries both below.
python3 "$SCRIPT_DIR/ipsw_pull.py" fetch "$IPSW_URL" \
        "Firmware/dfu/iBSS.*\.im4p" "$WORKDIR" \
    || fail "iBSS extraction failed"
IBSS_A="$(ls "$WORKDIR"/iBSS.n71.RELEASE.im4p 2>/dev/null | head -1)"
IBSS_B="$(ls "$WORKDIR"/iBSS.n71m.RELEASE.im4p 2>/dev/null | head -1)"
[ -n "$IBSS_A" ] || [ -n "$IBSS_B" ] || fail "no iBSS file produced"
info "iBSS images: ${IBSS_A:-none} ${IBSS_B:-none}"

# ------------------------------------------------------------------
# 3. Device state check + upload.
# ------------------------------------------------------------------
sleep 1
PWND="$(ioreg -p IOUSB -l -w0 2>/dev/null | grep -i 'USB Serial Number' | grep -c 'PWND:' || true)"
if [ "$PWND" -eq 0 ]; then
    info "device left pwned DFU (it booted iOS for detection)."
    info ">>> Put it back in DFU and re-run ./tr4mpass -v, then re-run this script."
    exit 2
fi

watch_recovery() {
    # $1 = seconds to watch
    START=$(date +%s)
    while :; do
        NOW=$(date +%s)
        [ $((NOW - START)) -ge "$1" ] && return 1
        if ioreg -p IOUSB -l -w0 2>/dev/null | grep -q '"idProduct" = 0x1281'; then
            return 0
        fi
        sleep 1
    done
}

try_upload() {
    # $1 = image path. Sends it and watches for the DFU->recovery flip.
    info "uploading $(basename "$1") to pwned DFU..."
    irecovery -f "$1" 2>&1 | sed 's/^/[irecovery] /'
    sleep 2
    watch_recovery 20 && return 0
    # Not booted: send a reset to get back to a clean DFU before retry.
    irecovery -c reset >/dev/null 2>&1 || true
    sleep 2
    return 1
}

RECOVERY_SEEN=0
for IMG in ${IBSS_A:+$IBSS_A} ${IBSS_B:+$IBSS_B}; do
    [ -f "$IMG" ] || continue
    if try_upload "$IMG"; then
        RECOVERY_SEEN=1
        break
    fi
    info "$(basename "$IMG") did not boot (wrong board variant?), trying next."
done

RESULT="FAIL"
if [ "$RECOVERY_SEEN" -eq 1 ]; then
    RESULT="PASS"
    info "recovery mode reached (PID 0x1281) -- USB-C transition OK."
    info "the iBSS booted; a full path_a run would now upload iBEC/devicetree/ramdisk."
else
    info "no recovery mode within 60 s -- the transition DID NOT happen."
fi

if [ "$KEEP_IMAGES" -eq 0 ]; then
    rm -f "$WORKDIR"/iBSS*.im4p
fi

echo "$RESULT"
[ "$RESULT" = "PASS" ]
