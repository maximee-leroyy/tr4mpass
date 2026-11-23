#!/bin/sh
# palera1n_fakefs.sh -- helper for the verified iOS 15+ SSV bypass.
#
# The C Path A pipeline still owns checkm8 and the SSH ramdisk. This helper
# covers the part that the ramdisk-only flow cannot do on iOS 15+: creating a
# rootful palera1n fakefs, installing a patched mobileactivationd signed with
# the stock iOS 15 entitlements, booting the fakefs, and verifying activation.
#
# Usage:
#   sudo env PALERA1N_BIN=/path/to/palera1n.app/Contents/MacOS/palera1n \
#       scripts/palera1n_fakefs.sh create
#   TR4MPASS_STOCK_MAD=/path/to/stock \
#   TR4MPASS_PATCHED_MAD=/path/to/patched \
#       scripts/palera1n_fakefs.sh patch
#   sudo env PALERA1N_BIN=/path/to/palera1n.app/Contents/MacOS/palera1n \
#       scripts/palera1n_fakefs.sh boot
#   scripts/palera1n_fakefs.sh verify
#
# create/boot need a cold DFU and root. patch assumes the SSH restore ramdisk is
# already booted (dropbear on 127.0.0.1:2222), the fakefs volume exists and it
# is a complete copy of the rootfs. verify runs on the host while the fakefs iOS
# is booted and must report FactoryActivated/false before setup is attempted.
set -eu

PALERA1N_BIN=${PALERA1N_BIN:-palera1n}
ROOT_DEV=${TR4MPASS_ROOT_DEV:-/dev/disk0s1s1}
FAKEFS_DEV=${TR4MPASS_FAKEFS_DEV:-/dev/disk0s1s8}
FAKEFS_MNT=${TR4MPASS_FAKEFS_MNT:-/mnt1/mnt8}
SSH_HOST=${TR4MPASS_SSH_HOST:-127.0.0.1}
SSH_PORT=${TR4MPASS_SSH_PORT:-2222}
SSH_USER=${TR4MPASS_SSH_USER:-root}
SSH_PASS=${TR4MPASS_SSH_PASSWORD:-alpine}
STOCK_MAD=${TR4MPASS_STOCK_MAD:-}
PATCHED_MAD=${TR4MPASS_PATCHED_MAD:-}
MAD_PATH="${FAKEFS_MNT}/usr/libexec/mobileactivationd"

die() {
    printf '[-] %s\n' "$*" >&2
    exit 1
}

need_root() {
    [ "$(id -u)" -eq 0 ] || die "run this command with sudo"
}

need_file() {
    [ -n "$1" ] || die "$2 is not set"
    [ -f "$1" ] || die "$2 does not exist: $1"
}

need_palera1n() {
    if [ ! -x "$PALERA1N_BIN" ]; then
        command -v "$PALERA1N_BIN" >/dev/null 2>&1 || \
            die "palera1n binary not found: $PALERA1N_BIN (set PALERA1N_BIN)"
    fi
}

ssh_rd() {
    sshpass -p "$SSH_PASS" ssh \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 \
        -o HostKeyAlgorithms=+ssh-rsa \
        -o PubkeyAcceptedAlgorithms=+ssh-rsa \
        -o KexAlgorithms=+diffie-hellman-group14-sha1,diffie-hellman-group1-sha1 \
        -p "$SSH_PORT" "${SSH_USER}@${SSH_HOST}" "$@"
}

create_fakefs() {
    need_root
    need_palera1n
    printf '[*] Put the device in a clean cold DFU, then press Enter.\n'
    read -r _ || true
    printf '[*] Creating the rootful fakefs (up to 10 minutes; do not interrupt).\n'
    "$PALERA1N_BIN" --cli -c -f -d
    printf '[+] Fakefs creation command finished. Check the volume is complete\n'
    printf '    from the ramdisk before installing the patched daemon.\n'
}

boot_fakefs() {
    need_root
    need_palera1n
    printf '[*] Put the device in a clean cold DFU, then press Enter.\n'
    read -r _ || true
    printf '[*] Booting the fakefs (semi-tethered).\n'
    "$PALERA1N_BIN" --cli -f -d
    printf '[+] Fakefs boot sent. Wait for iOS, then run: %s verify\n' "$0"
}

patch_fakefs() {
    need_file "$STOCK_MAD" "TR4MPASS_STOCK_MAD"
    need_file "$PATCHED_MAD" "TR4MPASS_PATCHED_MAD"
    command -v sshpass >/dev/null 2>&1 || die "sshpass is required"
    command -v codesign >/dev/null 2>&1 || die "codesign is required"

    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT INT TERM

    printf '[*] Extracting stock entitlements from %s\n' "$STOCK_MAD"
    codesign -d --entitlements :- "$STOCK_MAD" > "$tmp/ent.plist" 2>/dev/null
    [ -s "$tmp/ent.plist" ] || die "could not extract entitlements from stock MAD"

    printf '[*] Signing patched MAD with stock entitlements\n'
    cp "$PATCHED_MAD" "$tmp/mad"
    codesign --force --sign - --entitlements "$tmp/ent.plist" "$tmp/mad"
    codesign -v --verbose=2 "$tmp/mad" 2>&1 | sed 's/^/    /'

    printf '[*] Mounting rootfs and fakefs in the SSH ramdisk\n'
    ssh_rd "/bin/ls /mnt1 >/dev/null 2>&1 || /sbin/mount_apfs ${ROOT_DEV} /mnt1"
    ssh_rd "/bin/mkdir -p ${FAKEFS_MNT}; /bin/ls ${MAD_PATH} >/dev/null 2>&1 || /sbin/mount_apfs ${FAKEFS_DEV} ${FAKEFS_MNT}"
    ssh_rd "/bin/ls -l ${MAD_PATH}" >/dev/null 2>&1 || \
        die "fakefs MAD is not visible at ${MAD_PATH}; is the fakefs mounted?"

    printf '[*] Checking the fakefs is a complete rootfs copy\n'
    ssh_rd "/bin/ls ${FAKEFS_MNT}/Applications >/dev/null 2>&1" || \
        die "fakefs has no ${FAKEFS_MNT}/Applications: the copy was interrupted.
    A partial fakefs makes iOS fall back to the sealed rootfs, so the patched
    daemon is never used. Finish the copy (see docs/palera1n-fakefs-bypass.md)
    and run 'patch' again."
    apps=$(ssh_rd "/bin/ls ${FAKEFS_MNT}/Applications | /usr/bin/wc -l" | tr -d ' ')
    printf '[*] fakefs /Applications entries: %s\n' "$apps"

    printf '[*] Backing up the stock fakefs MAD (only if missing)\n'
    ssh_rd "test -f ${MAD_PATH}.bak || /bin/cp ${MAD_PATH} ${MAD_PATH}.bak"

    printf '[*] Uploading signed patched MAD\n'
    cat "$tmp/mad" | ssh_rd "cat > ${MAD_PATH}"
    ssh_rd "/bin/chmod 755 ${MAD_PATH}"
    if ssh_rd "/usr/sbin/chown 0:0 ${MAD_PATH} 2>/dev/null"; then
        :
    else
        printf '[*] chown is unavailable in the ramdisk; %s is owned by root\n' "$MAD_PATH"
    fi

    local_size=$(wc -c < "$tmp/mad" | tr -d ' ')
    remote_size=$(ssh_rd "/usr/bin/wc -c < ${MAD_PATH}" | tr -d ' ')
    [ "$local_size" = "$remote_size" ] || \
        die "size mismatch after upload: local=${local_size} remote=${remote_size}"

    printf '[*] Unmounting fakefs and rootfs\n'
    ssh_rd "/sbin/umount ${FAKEFS_MNT}" || true
    ssh_rd "/sbin/umount /mnt1" || true

    printf '[+] Fakefs MAD installed (%s bytes). Boot the fakefs with: %s boot\n' \
        "$local_size" "$0"
}

verify_activation() {
    command -v ideviceinfo >/dev/null 2>&1 || \
        die "ideviceinfo is required (libimobiledevice)"

    state=$(ideviceinfo -k ActivationState 2>/dev/null | tr -d '\r')
    brick=$(ideviceinfo -k BrickState 2>/dev/null | tr -d '\r')

    printf '[*] ActivationState: %s\n' "${state:-<empty>}"
    printf '[*] BrickState: %s\n' "${brick:-<empty>}"

    if [ "$state" = "FactoryActivated" ] && [ "$brick" = "false" ]; then
        printf '[+] Activation bypass is active.\n'
        return 0
    fi

    printf '[-] The patched mobileactivationd is not active.\n' >&2
    printf '    Expected FactoryActivated/false. The Setup Assistant screen alone\n' >&2
    printf '    does not prove success: a partial fakefs falls back to the sealed\n' >&2
    printf '    rootfs and still shows Hello/language/Wi-Fi before failing.\n' >&2
    exit 1
}

usage() {
    cat <<'USAGE'
Usage: scripts/palera1n_fakefs.sh <command>

Commands:
  create   Create the rootful palera1n fakefs (cold DFU + sudo).
  patch    Install the patched MAD into the fakefs (SSH ramdisk booted).
  boot     Boot the fakefs (cold DFU + sudo).
  verify   Check ActivationState/BrickState over usbmux after boot.

Environment:
  PALERA1N_BIN              palera1n 3.x binary (default: palera1n in PATH)
  TR4MPASS_STOCK_MAD        stock mobileactivationd (for entitlements)
  TR4MPASS_PATCHED_MAD      patched mobileactivationd
  TR4MPASS_FAKEFS_DEV       fakefs device (default /dev/disk0s1s8)
  TR4MPASS_FAKEFS_MNT       fakefs mountpoint (default /mnt1/mnt8)
  TR4MPASS_SSH_HOST/PORT    SSH ramdisk (default 127.0.0.1:2222)
  TR4MPASS_SSH_USER/PASSWORD

The patched MAD must be signed with the STOCK iOS entitlements. The helper
does that with codesign; the older binary's own entitlements are insufficient.

patch refuses to run on an incomplete fakefs (no /Applications). A partial
copy makes iOS boot the sealed rootfs instead, so the patched daemon is never
used even though the Setup Assistant still appears.
USAGE
}

case "${1:-}" in
    create) create_fakefs ;;
    patch)  patch_fakefs ;;
    boot)   boot_fakefs ;;
    verify) verify_activation ;;
    -h|--help|'') usage; [ -n "${1:-}" ] || exit 1 ;;
    *) usage; exit 1 ;;
esac
