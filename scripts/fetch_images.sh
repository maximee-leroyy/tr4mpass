#!/usr/bin/env bash
#
# fetch_images.sh -- Path A phase 2: pull the images path_a_ramdisk.c
# needs out of the IPSW of the INSTALLED iOS version, via HTTP Range
# requests only (no multi-GB download).
#
# Entry names verified against real central directories (read-only HTTP):
#   iPhone8,1 @ 19H422 (iOS 15.8.8), zip64 layout:
#     Firmware/dfu/iBSS.n71.RELEASE.im4p     (255679 B comp)
#     Firmware/dfu/iBSS.n71m.RELEASE.im4p    (254639 B comp)
#     Firmware/dfu/iBEC.n71.RELEASE.im4p     (451695 B comp)
#     Firmware/dfu/iBEC.n71m.RELEASE.im4p    (446543 B comp)
#     Firmware/all_flash/DeviceTree.n71ap.im4p   (25946 B comp)
#     Firmware/all_flash/DeviceTree.n71map.im4p
#     098-68700-067.dmg   = RestoreRamDisk (Erase install), 113 MB comp
#     098-68739-067.dmg   = RestoreRamDisk (Update install), 116 MB comp
#     098-68805-067.dmg   = OS image, 5.0 GB (NOT the ramdisk)
#     Firmware/098-68700-067.dmg.trustcache (5173 B comp) + one per dmg
#   iPhone8,1 @ 13G35 (iOS 9.3.4), classic zip (no zip64 extras):
#     Firmware/dfu/iBSS.n71{,m}.RELEASE.im4p
#     Firmware/all_flash/all_flash.n71ap.production/DeviceTree.n71ap.im4p
#     Firmware/all_flash/all_flash.n71map.production/DeviceTree.n71map.im4p
#     RestoreRamDisk: 058-48911-035.dmg (erase, n71ap) / 058-48723-035.dmg
#     (erase, n71map); OS 058-49357-035.dmg; NO trustcache entries (iOS <14).
#
# IMPORTANT: the restore ramdisk is NOT simply "the largest root *.dmg".
# In 19H422 the OS dmg (5 GB) dwarfs both ramdisks, and two ramdisk
# variants exist. BuildManifest.plist maps each board + install variant
# to its RestoreRamDisk; this script parses it (zip64-aware) and picks
# the Erase-variant ramdisk, then fetches the matching <dmg>.trustcache
# when one exists (searching the central directory for "trustcache").
#
# WHAT IS NOT DONE here (Phase 3 territory):
#   - no img4/IM4P decoding: the iBSS/iBEC .im4p files are raw IM4P
#     payloads, not bootable IMG4 containers;
#   - no decryption: the restore ramdisk / OS dmg are AES-encrypted (WKEY
#     on the A9); a custom ramdisk needs the ipsw(1) `ramdisk ssh
#     --decrypt` pipeline or equivalent (plan Phase 3);
#   - no personalized-IMG4 rebuild / ApNonce handling.
#   The iBSS smoke test works on the raw IM4P because pwned DFU's stage
#   loader accepts the payload sent by irecovery as-is.
#
# Board note: n71 = TSMC A9, n71m = Samsung A9 (dual-sourced on the 6s).
# The device BDID picks the right one; a wrong-board image simply does
# not boot, so both are fetched and path_a takes the primary (n71) while
# the iBSS test script tries both in turn.
#
# Usage: fetch_images.sh [--model iPhone8,1] [--build BUILDID]
#                        [--out-dir DIR]
# Writes files into the out dir (default "$(state_dir)/images", env
# override TR4MPASS_IMAGES_DIR) plus env.sh there, and prints sha256 +
# size for every artifact. Never touches /tmp.
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$SCRIPT_DIR/lib.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }
info() { echo "[fetch-images] $*"; }

DEVICE_ID="iPhone8,1"
BUILD=""
OUT_DIR_OVERRIDE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --model)   [ $# -ge 2 ] || fail "--model needs a value"
                   DEVICE_ID="$2";        shift 2 ;;
        --build)   [ $# -ge 2 ] || fail "--build needs a value"
                   BUILD="$2";            shift 2 ;;
        --out-dir) [ $# -ge 2 ] || fail "--out-dir needs a value"
                   OUT_DIR_OVERRIDE="$2"; shift 2 ;;
        -h|--help) sed -n '2,60p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

command -v python3 >/dev/null 2>&1 || fail "python3 not found"
command -v shasum  >/dev/null 2>&1 || fail "shasum not found"

if [ "$DEVICE_ID" != "iPhone8,1" ]; then
    fail "only iPhone8,1 (boards n71/n71m) entry patterns are implemented; got $DEVICE_ID"
fi

STATE="$(state_dir)"
if [ -z "$BUILD" ]; then
    BUILD_FILE="$STATE/ios_build.txt"
    [ -s "$BUILD_FILE" ] || fail "no build given and $BUILD_FILE is missing/empty -- run scripts/detect_ios.sh first (or pass --build BUILDID)"
    BUILD="$(cat "$BUILD_FILE")"
    info "using detected build from $(basename "$BUILD_FILE"): $BUILD"
fi

# Images live under the state dir layout (never /tmp).
OUT_DIR="${OUT_DIR_OVERRIDE:-${TR4MPASS_IMAGES_DIR:-$(state_dir)/images}}"
mkdir -p "$OUT_DIR" || fail "cannot create output dir: $OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

IPSW_URL="$(python3 "$SCRIPT_DIR/ipsw_pull.py" url-for "$DEVICE_ID" "$BUILD")" \
    || fail "could not resolve IPSW URL for $DEVICE_ID build $BUILD"
info "IPSW: $IPSW_URL"

fetch_one() {
    # $1 = exact-ish entry regex, $2 = expected output basename
    local regex="$1" want="$2"
    info "fetching $want ..."
    python3 "$SCRIPT_DIR/ipsw_pull.py" fetch "$IPSW_URL" "$regex" "$OUT_DIR" >/dev/null 2>&1 \
        || fail "fetch failed for pattern $regex"
    require_file "$want" "$OUT_DIR/$want"
}

# Two board variants for the dual-sourced A9: n71 (TSMC) / n71m (Samsung).
fetch_one 'Firmware/dfu/iBSS\.n71\.RELEASE\.im4p$'  iBSS.n71.RELEASE.im4p
fetch_one 'Firmware/dfu/iBSS\.n71m\.RELEASE\.im4p$' iBSS.n71m.RELEASE.im4p
fetch_one 'Firmware/dfu/iBEC\.n71\.RELEASE\.im4p$'  iBEC.n71.RELEASE.im4p
fetch_one 'Firmware/dfu/iBEC\.n71m\.RELEASE\.im4p$' iBEC.n71m.RELEASE.im4p
# iOS <=14: Firmware/all_flash/all_flash.<board>.production/DeviceTree.<board>.im4p
# iOS 15+:  Firmware/all_flash/DeviceTree.<board>.im4p  (basename unique either way)
fetch_one 'DeviceTree\.n71ap\.im4p$'                DeviceTree.n71ap.im4p

# Resolve the restore-ramdisk dmg (and matching trustcache, if any) from
# BuildManifest.plist. Read-only HTTP; prints shell-ready lines:
#   RAMDISK=<entry name>          (always)
#   TRUSTCACHE=<entry name>       (only when a matching entry exists)
# Zip64-aware with corrected csize/offset unpacking (ipsw_pull.py's CD
# reader returns names/methods correctly but mis-assigns zip64 sizes and
# offsets -- reported separately; keep this helper until that is fixed).
resolve_manifest() {
    python3 - "$IPSW_URL" "$SCRIPT_DIR" <<'PYEOF'
import sys, struct, zlib, plistlib, re, os

script_dir = sys.argv[2]
sys.path.insert(0, script_dir)
import ipsw_pull

url = sys.argv[1]

def cd_fixed():
    """[(name, method, comp_size, local_offset)] with correct zip64."""
    total = ipsw_pull.content_length(url)
    tail_len = min(total, 66560)
    tail = ipsw_pull.get(url, total - tail_len, total - 1)
    pos = tail.rfind(ipsw_pull.EOCD_SIG)
    if pos < 0:
        raise RuntimeError("EOCD not found")
    (_, _, _, _, n, cd_size, cd_off, _) = struct.unpack("<4sHHHHIIH", tail[pos:pos + 22])
    if cd_off == 0xFFFFFFFF or n == 0xFFFF:
        loc = tail.rfind(ipsw_pull.EOCD64_LOC_SIG, 0, pos)
        o = struct.unpack("<Q", tail[loc + 8:loc + 16])[0]
        rec = ipsw_pull.get(url, o, o + 55)
        (_, _, _, _, _, _, _, n, cd_size, cd_off) = struct.unpack(
            "<4sQHHIIQQQQ", rec[:56])
    cd = ipsw_pull.get(url, cd_off, cd_off + cd_size - 1)
    out, i = [], 0
    while i + 46 <= len(cd) and cd[i:i + 4] == b"PK\x01\x02":
        csize = struct.unpack("<I", cd[i + 20:i + 24])[0]
        usize = struct.unpack("<I", cd[i + 24:i + 28])[0]
        nlen, elen, clen = struct.unpack("<HHH", cd[i + 28:i + 34])
        loff = struct.unpack("<I", cd[i + 42:i + 46])[0]
        name = cd[i + 46:i + 46 + nlen].decode("utf-8", "replace")
        ex = cd[i + 46 + nlen:i + 46 + nlen + elen]
        j = 0
        while j + 4 <= len(ex):
            hid, hsz = struct.unpack("<HH", ex[j:j + 4])
            if hid == 1:
                k = j + 4
                if usize == 0xFFFFFFFF:
                    usize = struct.unpack("<Q", ex[k:k + 8])[0]; k += 8
                if csize == 0xFFFFFFFF:
                    csize = struct.unpack("<Q", ex[k:k + 8])[0]; k += 8
                if loff == 0xFFFFFFFF:
                    loff = struct.unpack("<Q", ex[k:k + 8])[0]; k += 8
            j += 4 + hsz
        out.append((name, csize, loff))
        i += 46 + nlen + elen + clen
    return out

def entry_blob(name, comp, loff):
    lh = ipsw_pull.get(url, loff, loff + 29)
    if lh[:4] != b"PK\x03\x04":
        raise RuntimeError("bad local header for %s" % name)
    nl, el = struct.unpack("<HH", lh[26:30])
    blob = ipsw_pull.get(url, loff + 30 + nl + el,
                         loff + 30 + nl + el + comp - 1)
    if struct.unpack("<H", lh[8:10])[0] == 8:
        blob = zlib.decompress(blob, -15)
    return blob

entries = cd_fixed()
bm = next(e for e in entries if e[0] == "BuildManifest.plist")
pl = plistlib.loads(entry_blob(bm[0], bm[1], bm[2]))

ramdisk = None
for ident in pl["BuildIdentities"]:
    info = ident["Info"]
    if info.get("RestoreBehavior") != "Erase":
        continue
    cls = info.get("DeviceClass", "")
    if cls not in ("n71ap", "n71map"):
        continue
    rr = ident.get("Manifest", {}).get("RestoreRamDisk")
    if rr and ramdisk is None:
        ramdisk = rr["Info"]["Path"]

if not ramdisk:
    sys.exit("no RestoreRamDisk found for n71ap/n71map Erase identity")

base = os.path.basename(ramdisk)
print("RAMDISK=%s" % base)

tc_names = [e[0] for e in entries if re.search(r"trustcache", e[0], re.I)]
match = [n for n in tc_names if n.endswith(base + ".trustcache")]
if match:
    print("TRUSTCACHE=%s" % os.path.basename(match[0]))
PYEOF
}

MANIFEST_OUT="$(resolve_manifest)" || fail "BuildManifest parse failed"
RAMDISK_NAME="$(printf '%s\n' "$MANIFEST_OUT" | sed -n 's/^RAMDISK=//p')"
TRUSTCACHE_NAME="$(printf '%s\n' "$MANIFEST_OUT" | sed -n 's/^TRUSTCACHE=//p')"
[ -n "$RAMDISK_NAME" ] || fail "could not resolve RestoreRamDisk entry name"
info "restore ramdisk entry: $RAMDISK_NAME"

fetch_one "(^|/)${RAMDISK_NAME}\$" "$RAMDISK_NAME"

if [ -n "$TRUSTCACHE_NAME" ]; then
    info "trustcache entry: $TRUSTCACHE_NAME"
    fetch_one "(^|/)${TRUSTCACHE_NAME}\$" "$TRUSTCACHE_NAME"
else
    info "no trustcache entry for this build (iOS < 14 ships none) -- skipping"
fi

# ------------------------------------------------------------------
# env.sh: absolute paths, sourced by run_path_a.sh / later phases.
# ------------------------------------------------------------------
ENV_SH="$OUT_DIR/env.sh"
{
    echo "# Generated by scripts/fetch_images.sh (IPSW $DEVICE_ID build $BUILD)."
    echo "# Board note: n71 = TSMC A9, n71m = Samsung A9; the device BDID picks"
    echo "# the right one. path_a takes the primary (n71); the iBSS test script"
    echo "# (ramdisk_test_ibss.sh) tries both in turn."
    echo "# NOTE: the .im4p files are raw IM4P payloads, NOT decoded/decrypted"
    echo "# IMG4s -- conversion (and ramdisk patching) happens in Phase 3."
    echo "export TR4MPASS_IBSS_PATH=\"$OUT_DIR/iBSS.n71.RELEASE.im4p\""
    echo "export TR4MPASS_IBSS_PATH_ALT=\"$OUT_DIR/iBSS.n71m.RELEASE.im4p\""
    echo "export TR4MPASS_IBEC_PATH=\"$OUT_DIR/iBEC.n71.RELEASE.im4p\""
    echo "export TR4MPASS_IBEC_PATH_ALT=\"$OUT_DIR/iBEC.n71m.RELEASE.im4p\""
    echo "export TR4MPASS_DEVICETREE_PATH=\"$OUT_DIR/DeviceTree.n71ap.im4p\""
    echo "export TR4MPASS_RAMDISK_PATH=\"$OUT_DIR/$RAMDISK_NAME\""
    if [ -n "$TRUSTCACHE_NAME" ]; then
        echo "export TR4MPASS_TRUSTCACHE_PATH=\"$OUT_DIR/$TRUSTCACHE_NAME\""
    fi
} > "$ENV_SH"

# ------------------------------------------------------------------
# Summary: sha256 + size of every artifact, validated via require_file.
# ------------------------------------------------------------------
info "artifacts:"
while IFS= read -r f; do
    [ -n "$f" ] || continue
    require_file "$(basename "$f")" "$f"
    SIZE=$(wc -c < "$f" | tr -d ' ')
    SHA=$(shasum -a 256 "$f" | awk '{print $1}')
    printf '  %-40s %10s bytes  sha256=%s\n' "$(basename "$f")" "$SIZE" "$SHA"
done <<EOF
$OUT_DIR/iBSS.n71.RELEASE.im4p
$OUT_DIR/iBSS.n71m.RELEASE.im4p
$OUT_DIR/iBEC.n71.RELEASE.im4p
$OUT_DIR/iBEC.n71m.RELEASE.im4p
$OUT_DIR/DeviceTree.n71ap.im4p
$OUT_DIR/$RAMDISK_NAME
${TRUSTCACHE_NAME:+$OUT_DIR/$TRUSTCACHE_NAME}
EOF

info "env file: $ENV_SH  (source it to export TR4MPASS_*_PATH)"
