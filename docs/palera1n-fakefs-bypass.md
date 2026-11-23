# Verified iOS 15+ SSV bypass: palera1n rootful fakefs + patched mobileactivationd

**Status:** verified end-to-end on hardware on 2026-11-23. The device completed
activation from the Setup Assistant and reported `ActivationState =
FactoryActivated`, `BrickState = false`.

**Device:** iPhone 6s (`iPhone8,1` / `n71ap`, A9, CPID `0x8000`), iOS **15.8.8 (19H422)**.
**Host:** macOS 27.2.1 arm64 (Apple Silicon).
**Result:** the device boots a complete rootful fakefs whose
`/usr/libexec/mobileactivationd` is the patched daemon, and activation succeeds.
The bypass is **semi-tethered**: every reboot must be followed by `palera1n -f`
from a computer.

This document records the flow that actually worked. It is intentionally
separate from the C `Path A` pipeline: the ramdisk-only Path A stages cannot
finish activation on iOS 15+ for the reasons below.

## The Setup Assistant is not proof of success

An unactivated device shows the Setup Assistant (Hello / language / country /
Wi-Fi) with the **stock** daemon as well. A device with the stock daemon shows
this screen and then fails at the first network activation step with
**"iPhone Locked to Owner"**.

The only reliable check runs on the host while iOS is booted:

```bash
ideviceinfo -k ActivationState   # must be: FactoryActivated
ideviceinfo -k BrickState        # must be: false
```

If they report `Unactivated` / `true`, the patched daemon is **not** running,
even though the Wi-Fi page has a Next button.

## The fakefs must be a complete copy of the rootfs

`palera1n -c` creates the rootful fakefs as a **full copy** of the system
volume. Creation can take 10+ minutes. If it is interrupted, the APFS volume
stays partially populated. A partial fakefs is not usable as a root: iOS falls
back to the sealed rootfs, the patched daemon is never executed, and activation
fails exactly as described above. This is the failure mode that was mistaken
for "the patch does not work" during earlier attempts.

Check the volume from the SSH ramdisk before installing anything:

```sh
mount_apfs /dev/disk0s1s1 /mnt1
mount_apfs /dev/disk0s1s8 /mnt1/mnt8
ls /mnt1/mnt8/Applications          # must list the system apps
ls -d /mnt1/mnt8/Applications /mnt1/mnt8/Library /mnt1/mnt8/private /mnt1/mnt8/System/Library
```

In the verified run the complete fakefs held ~361k files. The broken partial
volume held ~179k files and had no `/Applications` at all.

If the copy was interrupted, finish it from the ramdisk with `tar` (no `-c`
support in the ramdisk `cp`), then reinstall the patched daemon, because a
full copy overwrites it:

```sh
nohup /bin/sh -c '
  /usr/bin/tar cf - -C /mnt1 .ba .file .mb Applications Developer Library \
      cores private dev tmp var | /usr/bin/tar xpf - -C /mnt1/mnt8
  /usr/bin/tar cf - -C /mnt1 System | /usr/bin/tar xpf - -C /mnt1/mnt8
  echo ALLDONE > /mnt1/mnt8/DONE_TAR' >/mnt1/mnt8/tar.log 2>&1 &
```

The restore ramdisk has very little RAM. Heavy copying can get dropbear killed;
run the copy detached with `nohup`, wait for `DONE_TAR`, and only then reboot.
Do not reboot in the middle of the copy.

## Why the ramdisk-only Path A is not enough

1. **SSV (Signed System Volume).** On iOS 15 the system volume is sealed and
   booted from a snapshot. Replacing `mobileactivationd` on the live
   `/mnt1` volume from a restore ramdisk does not survive a normal boot. A
   writable fakefs (or an equivalent SSV bypass) is required.
2. **No `lockdownd` in the restore ramdisk.** `path_a_activate()` submits the
   activation record through libimobiledevice's `mobileactivation` service,
   which needs `lockdownd`. The restore ramdisk does not run `lockdownd`, and
   the DFU-path `device_info_t` never gets a lockdownd handle after the ramdisk
   boots.
3. **NAND enumeration on A9.** The restore ramdisk must be booted **without**
   the `-restore` boot argument. With `-restore`, `disk0` is an IOMedia with
   `Size = 0` and `!reachable`; no GPT slices appear and `mount_apfs` fails.
   Remove both `-restore` and `-progress` from the iBEC boot args. With them
   removed, `disk0s1s1..s8` appear and the rootfs mounts read-write.

## Requirements

- **palera1n 3.0.0 beta** (macOS arm64 nightly from 2026-11-21 or newer).
  The v2.4 release failed at `Booting PongoOS...`/softboot on this macOS/A9/
  USB-C setup. The v3 beta brings up PongoOS reliably.
- A **complete rootful fakefs** created by palera1n (`-c -f`), normally
  `/dev/disk0s1s8`. See the completeness check above.
- A patched **`mobileactivationd`**. The verified file is the
  `mobileactivationd_ios15` shipped by
  [`conofor/bypass`](https://github.com/conofor/bypass) and
  [`imccs9911/setupapp-mobileactivationd-patch`](https://github.com/imccs9911/setupapp-mobileactivationd-patch)
  (`sha256 8919fd4d3e10f4bbc4ac2cfd56eef75ee9b13521c38f27bc16adac9c4170a6ca`).
  It identifies itself as `MobileActivation-353.260.2` (iOS 12.4 build) but is
  the binary both referenced methods use successfully in a rootful fakefs.
- The **stock** `mobileactivationd` from the same iOS, used only to extract the
  correct iOS 15 entitlements.
- An SSH restore ramdisk with a working dropbear on `127.0.0.1:2222`, built
  without `-restore` (see the NAND note above).
- `sshpass`, `iproxy`, and macOS `codesign`.

## Verified flow

### 1. Create the fakefs and let it finish

Put the device into a clean cold DFU, then run palera1n as root:

```bash
sudo /path/to/palera1n.app/Contents/MacOS/palera1n --cli -c -f -d
```

The fakefs creation can take **up to 10 minutes**. Do not interrupt it, even
if the device looks stuck. When it is done it creates `/dev/disk0s1s8` and may
reboot the device to recovery.

### 2. Boot the SSH ramdisk and verify the fakefs

Boot the restore ramdisk with the no-`-restore` iBEC, start the USB tunnel, and
check that the fakefs is complete:

```bash
iproxy 2222 2222
ssh -p 2222 root@127.0.0.1
mount_apfs /dev/disk0s1s1 /mnt1
mkdir -p /mnt1/mnt8
mount_apfs /dev/disk0s1s8 /mnt1/mnt8
ls /mnt1/mnt8/Applications
ls -l /mnt1/mnt8/usr/libexec/mobileactivationd
```

If `/Applications` is missing or nearly empty, the copy was interrupted: finish
it first (see above), then continue.

### 3. Sign and install the patched daemon

This is the part that is easy to get wrong. **Use the stock iOS 15
entitlements**, not the entitlements of the older patched binary. The patched
binary is an iOS 12 build; its own entitlements are missing keys such as
`com.apple.private.FairPlayIOKitUserClient.access` and
`com.apple.private.ProvInfoIOKitUserClient.access`, and the daemon will not
behave correctly without them.

On the host:

```bash
codesign -d --entitlements :- /path/to/stock/mobileactivationd > /tmp/stock-ent.plist
cp /path/to/patched/mobileactivationd /tmp/mad.signed
codesign --force --sign - --entitlements /tmp/stock-ent.plist /tmp/mad.signed
codesign -v --verbose=2 /tmp/mad.signed   # must be valid on disk
```

Install it into the fakefs (the stock backup is kept):

```bash
ssh -p 2222 root@127.0.0.1 \
  'cp /mnt1/mnt8/usr/libexec/mobileactivationd /mnt1/mnt8/usr/libexec/mobileactivationd.bak'
cat /tmp/mad.signed | ssh -p 2222 root@127.0.0.1 \
  'cat > /mnt1/mnt8/usr/libexec/mobileactivationd'
ssh -p 2222 root@127.0.0.1 \
  'chmod 755 /mnt1/mnt8/usr/libexec/mobileactivationd'
ssh -p 2222 root@127.0.0.1 'umount /mnt1/mnt8; umount /mnt1'
```

The restore ramdisk has no `chown`; the file is created by the root SSH login,
so it already belongs to `root:wheel`.

The helper `scripts/palera1n_fakefs.sh patch` performs steps 2-3, including the
stock-entitlements signing and the fakefs completeness check.

### 4. Boot the fakefs

Power the device off, enter cold DFU again, and boot the fakefs:

```bash
sudo /path/to/palera1n.app/Contents/MacOS/palera1n --cli -f -d
```

### 5. Verify, then complete the Setup Assistant

Once iOS is up:

```bash
scripts/palera1n_fakefs.sh verify
```

It must print `FactoryActivated` / `false`. Only then go through the Setup
Assistant: Hello → language → country → Wi-Fi → Next should complete activation
without the "iPhone Locked to Owner" screen.

## Operational notes

- **Semi-tethered:** every reboot requires `palera1n -f` from the computer.
  Booting normally uses the sealed rootfs and the activation lock returns.
- The fakefs and the stock daemon backup live on the device. Keep a copy of the
  stock and patched binaries on the host.
- `allow-root-hash-mismatch=1` was set in NVRAM in the verified run. It is not
  proven to be required for the fakefs boot, but it is harmless and mirrors the
  other published method.
- On macOS 27 + Apple Silicon + A9, USB-C re-enumeration is flaky. Use the USB
  port/cable where checkm8/palera1n actually enumerates PongoOS; switching
  ports between the checkm8 tool and palera1n is expected.
- If a palera1n run hangs or the device shows a black screen, kill the host
  process, do a hardware power cycle (Power + Home until the Apple logo), and
  start again from a cold DFU. A failed checkm8 corrupts the BootROM heap until
  the next power cycle.
- `Path A` still owns the checkm8 exploit and the SSH-ramdisk stages. The C
  activation step is not used in this flow because it requires `lockdownd`.
  Adding a first-class `--fakefs` mode to tr4mpass is future work.
