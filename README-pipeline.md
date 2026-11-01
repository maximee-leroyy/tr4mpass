# Pipeline Operations Guide — from PWND to activated

> **TLDR** — checkm8 is proven on this machine (E19). This guide documents
> every script and code path that takes the device the rest of the way:
> images → ramdisk → kernel patches → activation. Run commands from the
> repo root. All state lives in `~/.cache/tr4mpass` (never `/tmp`).
>
> Sources: links inline. Anything we could not verify is flagged **UNVERIFIED**.

---

## 1. The one picture

```mermaid
flowchart LR
    A["DFU pwned<br/>PWND:[checkm8]<br/>(proven, E19)"] --> B["Phase 1<br/>iBSS transition test"]
    B -->|PASS| C["Phase 2<br/>fetch_images.sh"]
    B -->|FAIL| X["Pivot: pongoOS route"]
    C --> D["Phase 3<br/>SSH ramdisk (option C)"]
    D --> E["Phase 4/5<br/>run_path_a.sh<br/>--stop-after 4|5|7|8"]
    E --> F["Device activated<br/>signal preserved"]
```

Full plan with gates: [`PLAN-path-a-execution.md`](PLAN-path-a-execution.md).
Route comparison vs palera1n:
[`COMPARISON-palera1n-vs-tr4mpass.md`](COMPARISON-palera1n-vs-tr4mpass.md).

---

## 2. Scripts (all in `scripts/`, no agents needed)

Shared state: every script sources `scripts/lib.sh`, which owns
`TR4MPASS_STATE_DIR` (default `~/.cache/tr4mpass`, override via env).
`state_dir()` and `log_dir()` create-and-print; `require_env_var` and
`require_file` fail fast with a clear message.

| Script | What it does | Verified how |
|---|---|---|
| `lib.sh` | State-dir contract (single source of truth) | contract used by all scripts |
| `detect_ios.sh` | Waits for normal-mode device, records iOS version/build/model into `$(state_dir)` | device run pending (Phase 1) |
| `ipsw_pull.py` | Pulls ONE file from a remote IPSW via HTTP Range (ZIP central directory) — ~300 KB instead of a 2+ GB download. Subcommands: `list` / `url-for` / `probe` / `fetch` | probed 4 real IPSWs (9.0→9.3.5) |
| `ramdisk_test_ibss.sh` | **Phase 1 gate**: uploads iBSS to pwned DFU, watches PID 0x1227→0x1281 | not yet run on device |
| `fetch_images.sh` | **Phase 2**: resolves IPSW by build, extracts iBSS/iBEC (both n71 board variants), DeviceTree, RestoreRamDisk + trustcache via `BuildManifest.plist`; writes `$(state_dir)/images/env.sh` | end-to-end on iOS 9.3.4 IPSW; entry names cross-checked on iOS 15.8.8 |
| `run_path_a.sh` | **Phase 4/5**: validates env, exports `TR4MPASS_STOP_AFTER`, runs `./tr4mpass -v`, tees log, prints final serial | stub-tested arg/exit paths |
| `build_ramdisk_ssh.sh` | **Phase 3**: capability gate for an SSH-ramdisk builder; fails with exact upgrade command today | tool capabilities verified from upstream sources (below) |

---

## 3. Code changes that make re-runs safe

| Change | Where | Why |
|---|---|---|
| **Skip-if-pwned on macOS** | `src/dfu/checkm8.c` (`checkm8_run`) | Second run on a pwned device used to re-run all 4 exploit stages against pwned DFU → guaranteed fail. Now: macOS closes the libusb handle (no USB traffic), reads serial via IOKit (E19-proven), returns 0 if `PWND:` present. Linux verifies on the open handle as before. |
| **Staged execution** | `src/pipeline/path_a.c` + `env_config.{h,c}` | `TR4MPASS_STOP_AFTER=N` (1–8, default 8) stops after step N with success. Lets us prove steps 3–4 (ramdisk+patches) before touching activation. Single source of truth for the name: `TR4MPASS_ENV_STOP_AFTER` in `include/util/env_config.h`. |
| `TR4MPASS_STOP_AFTER` registry entry | `src/util/env_config.c` | kind `ENV_KIND_PLAIN` (same as `SSH_PORT`) — no new kind. |

---

## 4. Workflow, concretely

```sh
# 0. build once
make

# 1. Device in normal mode first (it boots iOS after unplug/replug):
scripts/detect_ios.sh                 # waits, writes $(state_dir)/ios_*.txt

# 2. Put device in DFU, then pwn it:
./tr4mpass -v                          # E19 flow, ends with PWND:[checkm8]

# 3. Pull the images for the detected build:
scripts/fetch_images.sh
source ~/.cache/tr4mpass/images/env.sh

# 4. Phase 1 gate — does the USB-C transition survive?
scripts/ramdisk_test_ibss.sh           # PASS => route confirmed

# 5. Staged runs (device back in DFU, re-pwn if needed):
scripts/run_path_a.sh --stop-after 4   # pwn+verify+ramdisk+patches, no activation
scripts/run_path_a.sh --stop-after 5   # + mobileactivationd replacement (needs TR4MPASS_PATCHED_MAD)
scripts/run_path_a.sh --stop-after 7   # + activation record + deletescript
scripts/run_path_a.sh                  # full [1/8]..[8/8]
```

Logs land in `~/.cache/tr4mpass/logs/` (timestamped).

---

## 5. Tooling decisions — existing tools first (user directive)

Verified 2026-11-01 from upstream sources; the verification trail is
documented in each script's header.

| Need | Existing tool | Verdict |
|---|---|---|
| Download/extract IPSW | [blacktop/ipsw](https://github.com/blacktop/ipsw) (MIT) — `brew install blacktop/tap/ipsw` | ✅ use it ([docs](https://blacktop.github.io/ipsw)) |
| Build SSH ramdisk | `ipsw ssh` | ❌ **is an SSH client, not a builder** ([source](https://github.com/blacktop/ipsw/blob/master/cmd/ipsw/cmd/ssh/ssh.go), [docs](https://blacktop.github.io/ipsw/docs/cli/ipsw/ssh/ssh)) |
| Mount stock ramdisk | `ipsw mount rdisk IPSW --ident Erase [--key K | --lookup]` ([docs](https://blacktop.github.io/ipsw/docs/cli/ipsw/mount)) | ✅ verified |
| Prebuilt SSH ramdisk | dora2-iOS | ❌ **account inactive, releases 404** — UNVERIFIED→dead |
| Patch iBEC/iBoot/dyld | [iBoot64Patcher](https://github.com/Tihmstar/iBoot64Patcher), [img4tool](https://github.com/Tihmstar/img4tool), [iExtractor](https://github.com/malus-security/iextractor) | ✅ known (README References) |
| Activate ([6/8]) | [ideviceactivation](https://github.com/libimobiledevice/libideviceactivation) `activate` — *"complete activation and deactivation process"* | ✅ candidate to replace our C step for the first device run (LGPL2.1/GPL3 CLI — mind linking) |
| deletescript ([7/8]) | none known | ❌ stays tr4mpass code |

**Consequence**: Phase 3 option C is the only live path (manual pipeline:
`ipsw mount rdisk` → inject dropbear + tools → iBoot64Patcher → img4tool).
`build_ramdisk_ssh.sh` keeps a runtime probe so a future `ipsw ssh ramdisk`
is adopted automatically when it exists.

---

## 6. Honest risks and open blockers

| Risk / blocker | Status |
|---|---|
| `TR4MPASS_PATCHED_MAD` (patched mobileactivationd) | **OPEN** — not in repo, not in IPSW, no known prebuilt (dora2 dead). Options: build from libideviceactivation/go-ios sources, or obtain externally. Decision needed before Phase 4 stage B. |
| USB-C transitions post-pwn (Phase 1) | unmeasured until `ramdisk_test_ibss.sh` runs |
| SSH ramdisk decrypt/patch chain | multi-tool, untested on-device; WKEY lookup via `ipsw` expected but **UNVERIFIED** on A9 |
| `ideviceactivation` compatibility with old iOS | **UNVERIFIED** for iOS 9–12; test before relying on it |
| `fetch_images.sh` on iOS 15 IPSWs | entry names verified, full fetch not exercised (5 GB OS dmg avoided by design) |
| `checkm8_verify` macOS fallback log | fixed to return early; no misleading usbipd warning on fresh runs |

---

## 7. State layout

```
~/.cache/tr4mpass/          (override: TR4MPASS_STATE_DIR)
├── ios_version.txt         from detect_ios.sh
├── ios_build.txt
├── ios_model.txt
├── images/                 from fetch_images.sh
│   ├── env.sh              source this before run_path_a.sh
│   ├── iBSS.n71.RELEASE.im4p        (TSMC A9)
│   ├── iBSS.n71m.RELEASE.im4p       (Samsung A9)
│   ├── iBEC.n71{,m}.RELEASE.im4p
│   ├── DeviceTree.n71ap.im4p
│   └── <ramdisk>.dmg [+ .trustcache on iOS 14+]
└── logs/                   timestamped run logs
```

---

## 8. Sources

- [libideviceactivation README](https://github.com/libimobiledevice/libideviceactivation) — activation/deactivation CLI (fetched 2026-11-01)
- [blacktop/ipsw](https://github.com/blacktop/ipsw) + [CLI docs](https://blacktop.github.io/ipsw) — download/mount/ssh capabilities (fetched 2026-11-01)
- [ipsw ssh source](https://github.com/blacktop/ipsw/blob/master/cmd/ipsw/cmd/ssh/ssh.go) — proves it is a client wrapper
- [ipsw mount docs](https://blacktop.github.io/ipsw/docs/cli/ipsw/mount) — rdisk mounting
- dora2-iOS GitHub — org shows `public_repos: 0`; releases 404 (checked 2026-11-01)
- Internal: E19 proof, script verification logs — [`RESEARCH-apple-silicon-usb-c-dfu.md`](RESEARCH-apple-silicon-usb-c-dfu.md)
