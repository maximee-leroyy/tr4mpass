#!/usr/bin/env bash
#
# build_ssh_rd.sh -- Path A phase 3: build the custom SSH ramdisk.
#
# Inputs (verified 2026-11-01, journal E31/E32):
#   images/018-18125-090.dmg   IM4P(rdsk); payload is a RAW HFSX volume
#                              already decrypted -- no GID key needed
#                              (E31: extracts + mounts directly).
#   tools/sshtars/ssh.tar.gz   verygenericname/SSHRD_Script ssh kit
#                              (BSD-3, vendored; README-tr4mpass.md).
#
# Recipe (mirrors SSHRD_Script sshrd.sh, adapted to the tr4mpass
# pongo-upload boot path: no SHSH/IM4M signing needed, pongoOS+KPF
# boots the rdsk without a personalized manifest):
#   1. ipsw img4 im4p extract            -> raw HFSX volume image
#   2. hdiutil attach rw                 -> copy in the ssh kit
#   3. overlay kit dylibs (stock fs has no libncurses/libresolv),
#      trim unused heavy files, add a dropbear LaunchDaemon, verify
#      every kit binary's dylib deps resolve on the result
#   4. detach, wrap back into IM4P(rdsk) via ipsw (128 MiB cap check)
#   5. verify: re-attach the rebuilt IM4P read-only; dropbear present
#
# Size: stock payload 100.4 MiB, ~27 MiB free on the volume; trimmed
# kit ~13 MiB (dropbear+bash+dylibs, no nano/locale/TrollStore) fits
# without growing the volume. hdiutil resize cannot grow a bare HFSX
# volume image, and the final IM4P must stay <= 128 MiB (pongoOS
# UPLOADSZ_MAX, usbloader.c) -- script fails loudly if exceeded.
#
# Output: "$(state_dir)/images/ramdisk_ssh.dmg" (IM4P rdsk); env.sh
# gets TR4MPASS_SSHRD_PATH.

set -u

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib.sh
source "$SELF_DIR/lib.sh"

fail() { echo "FAIL: $*" >&2; exit 1; }
info() { echo "[ssh-rd] $*"; }

SRC_IM4P="$(state_dir)/images/018-18125-090.dmg"
KIT_TAR="$SELF_DIR/../tools/sshtars/ssh.tar.gz"
OUT="$(state_dir)/images/ramdisk_ssh.dmg"
WORK="$(state_dir)/work_sshrd"
MNT="$(state_dir)/mnt_sshrd"

require_file "stock ramdisk im4p" "$SRC_IM4P"
require_file "ssh kit tar" "$KIT_TAR"
command -v ipsw >/dev/null || fail "ipsw not found (brew install ipsw)"
command -v hdiutil >/dev/null || fail "hdiutil not found"
command -v otool >/dev/null || fail "otool not found (xcode CLT)"

rm -rf "$WORK" "$MNT"
mkdir -p "$WORK" "$MNT"

info "extracting IM4P payload (raw HFSX volume image)"
ipsw img4 im4p extract "$SRC_IM4P" -o "$WORK/rd.hfs" >/dev/null \
    || fail "im4p extract failed"
RAW_SZ=$(stat -f %z "$WORK/rd.hfs")
info "raw fs: $RAW_SZ bytes"

# hdiutil only mounts a bare HFS image if it is named .dmg (raw disk
# image); the extension matters, content does not.
cp "$WORK/rd.hfs" "$WORK/rd.dmg"

info "attaching rw at $MNT"
hdiutil attach -mountpoint "$MNT" -owners off "$WORK/rd.dmg" \
    >/dev/null || fail "attach failed"
DETACHED=0
detach() {
    if [ "$DETACHED" -eq 0 ]; then
        hdiutil detach -force "$MNT" >/dev/null 2>&1 || true
        DETACHED=1
    fi
}
trap detach EXIT
R="$MNT"

info "staging + trimming the kit locally (volume has limited free space)"
KIT="$WORK/kit"
mkdir -p "$KIT"
tar xzf "$KIT_TAR" -C "$KIT" || fail "kit untar failed"
rm -rf "$KIT/usr/share/locale" "$KIT/usr/share/nano" \
       "$KIT/usr/share/terminfo" "$KIT/usr/trollstore"
rm -f  "$KIT/usr/share/misc/magic.mgc"
rm -f  "$KIT/usr/bin/nano" "$KIT/usr/bin/rnano" "$KIT/usr/bin/sqlite3" \
       "$KIT/usr/bin/binbag" "$KIT/usr/bin/screen" \
       "$KIT/usr/bin/trollstoreinstaller"
KIT_SZ=$(du -sk "$KIT" | cut -f1)
FREE_KB=$(df -k "$MNT" | tail -1 | awk '{print $4}')
info "trimmed kit: ${KIT_SZ} KiB; volume free: ${FREE_KB} KiB"
if [ "$KIT_SZ" -ge "$FREE_KB" ]; then
    fail "trimmed kit (${KIT_SZ} KiB) does not fit (${FREE_KB} KiB free)"
fi
cp -Rp "$KIT/." "$R/" || fail "kit copy failed"
rm -rf "$KIT"
# note: the kit's own dylibs went into $R/usr/lib with the copy (the
# stock fs lacks libncurses/libresolv, the kit provides them)

printf 'tr4mpass ssh ramdisk\nmount rootfs rw: /usr/bin/mount_filesystems (disk0s1s1 -> /mnt1)\n' \
    > "$R/usr/share/motd.tr4mpass" 2>/dev/null || true

# The kit's restored_external (SSHRD_Script Sep 2022) execl's
# /usr/local/bin/dropbear over the USB mux and runs
# /bin/bash /usr/bin/mount_filesystems; launchd runs it via plist.
info "adding dropbear LaunchDaemon"
cat > "$R/System/Library/LaunchDaemons/com.tr4mpass.dropbear.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>com.tr4mpass.dropbear</string>
	<key>ProgramArguments</key>
	<array>
		<string>/usr/local/bin/restored_external</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>StandardOutPath</key>
	<string>/dev/console</string>
	<key>StandardErrorPath</key>
	<string>/dev/console</string>
</dict>
</plist>
PLIST

info "verifying kit binary dylib deps resolve on the built fs"
MISSING=""
for b in "$R"/usr/local/bin/dropbear "$R"/bin/bash "$R"/bin/launchctl \
         "$R"/usr/sbin/ioreg "$R"/usr/bin/sed "$R"/usr/bin/img4tool \
         "$R"/usr/local/bin/dbclient "$R"/usr/libexec/sftp-server; do
    [ -e "$b" ] || { MISSING="$MISSING (absent $(basename "$b"))"; continue; }
    for lib in $(otool -L "$b" 2>/dev/null \
                 | awk '/dylib/{print $1}' | sed 's|.*/||'); do
        [ "$lib" = "libSystem.B.dylib" ] && continue
        if ! ls "$R/usr/lib/$lib" >/dev/null 2>&1 \
           && ! ls "$R/usr/local/lib/$lib" >/dev/null 2>&1; then
            MISSING="$MISSING ($(basename "$b"):$lib)"
        fi
    done
done
[ -z "$MISSING" ] || fail "missing dylibs:$MISSING"

sync
info "detaching"
detach
trap - EXIT

FINAL_SZ=$(stat -f %z "$WORK/rd.dmg")
info "final raw fs: $FINAL_SZ bytes ($(( FINAL_SZ / 1024 / 1024 )) MiB)"
if [ "$FINAL_SZ" -gt $((128 * 1024 * 1024)) ]; then
    fail "raw fs $FINAL_SZ > 128 MiB pongoOS upload cap"
fi

info "wrapping into IM4P(rdsk)"
ipsw img4 im4p create --type rdsk --compress none -o "$OUT" \
    "$WORK/rd.dmg" || fail "im4p create failed"
OUT_SZ=$(stat -f %z "$OUT")
info "IM4P size: $OUT_SZ bytes"
if [ "$OUT_SZ" -gt $((128 * 1024 * 1024)) ]; then
    fail "IM4P $OUT_SZ > 128 MiB pongoOS upload cap"
fi

info "verify: extract the rebuilt payload and mount it"
ipsw img4 im4p extract "$OUT" -o "$WORK/verify.hfs" >/dev/null \
    || fail "verify extract failed"
cmp -s "$WORK/verify.hfs" "$WORK/rd.dmg" \
    || fail "rebuilt payload differs from what was written"
cp "$WORK/verify.hfs" "$WORK/verify.dmg"
hdiutil attach -readonly -mountpoint "$MNT" "$WORK/verify.dmg" >/dev/null \
    || fail "rebuilt volume does not mount"
[ -e "$MNT/usr/local/bin/dropbear" ] \
    || { hdiutil detach "$MNT"; fail "dropbear missing in rebuilt volume"; }
[ -e "$MNT/System/Library/LaunchDaemons/com.tr4mpass.dropbear.plist" ] \
    || { hdiutil detach "$MNT"; fail "LaunchDaemon missing in rebuilt volume"; }
hdiutil detach "$MNT" >/dev/null

ENV_SH="$(state_dir)/images/env.sh"
if grep -q TR4MPASS_SSHRD_PATH "$ENV_SH" 2>/dev/null; then
    sed -i '' 's|export TR4MPASS_SSHRD_PATH=.*|export TR4MPASS_SSHRD_PATH="'"$OUT"'"|' "$ENV_SH"
else
    echo 'export TR4MPASS_SSHRD_PATH="'"$OUT"'"' >> "$ENV_SH"
fi

echo "OK: $OUT ($OUT_SZ bytes)"
