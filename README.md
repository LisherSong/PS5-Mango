# PS5 Nexus

A self-contained web control panel for a jailbroken PS5: manage files on the
console, pull from and push to network shares, unpack archives, install
packages, and back up / restore game saves — all from a browser on the same
network (or from the PS5's own browser).

**Version:** v1.0.0

The payload embeds its whole front-end (HTML/CSS/JS/graphics) and compresses it
into the ELF, so deployment is one file and there is nothing to install
alongside it.

> **Where the version is shown.** The payload manager (and etaHEN's
> *plugins / payload ELFs* menu) lists payloads by **file name**, so the version
> deliberately lives in the file name: the build output is
> `PS5-Nexus-v1.0.0.elf`. Bumping `VERSION_TAG` in the Makefile renames the
> artifact, which is the only thing the console can display.
> The payload also reports its version in the startup notification, on stdout,
> and over `/api/status`.

## Features

### Files

- Browse internal storage (`/data`, `/user/data`), the M.2 drive (`/mnt/ext1`)
  and USB volumes (`/mnt/usb*`), with each storage root offered as one tap.
- Sort by name, type, size, modified time or permissions; the choice is kept in
  browser local storage.
- Copy, move, delete, rename, create folder, create text file; multi-select with
  a selection bar.
- Conflict prompts when a copy/move would overwrite a file or merge a folder.
- Permissions column: edit read/write/execute per class, or type a validated
  four-digit octal mode.
- Built-in text editor for UTF-8 files up to 1 MiB, with an optimistic-concurrency
  token so saving over a file that changed meanwhile is detected instead of
  silently clobbering it.
- Upload single files, multiple files or whole folders (file picker or
  drag-and-drop). Data is written to a `.wfm-upload-*.tmp` file that is renamed
  into place only once the byte count matches, so an interrupted upload never
  leaves a truncated file that looks complete.
- Download a single file directly, or a folder / multi-selection as a `.tar`
  stream generated on the fly.
- The toolbar (breadcrumbs + actions) stays pinned below the top bar while the
  list scrolls.

### Network shares (SMB / NFS)

- **SMB2/3** via a vendored [libsmb2](https://github.com/sahlberg/libsmb2), and
  **NFSv3/v4** via a vendored [libnfs](https://github.com/sahlberg/libnfs).
- Shares are stored as bookmarks in the browser; the server is given
  host / share / path / port / user / password as separate fields.
- The address field accepts whatever you have to hand —
  `192.168.1.10/shared`, `192.168.1.10\shared`, `smb://192.168.1.10/shared`,
  `nfs://192.168.1.10/export` — and is normalised before use. (Windows-style
  backslashes are accepted on both sides of the wire; a `\` inside a *password*
  is never rewritten.)
- Pull a file straight off a share into the console's storage with
  `POST /api/fetch` (resumable, same as upload).

### Archives

Extraction covers `.zip`, `.zipx`, `.7z`, `.rar`, `.001`, `.tar`, `.gz`,
`.bz2`, `.xz`, `.zst`, `.lzh`, `.cab`, `.cpio`, `.arj`, `.pmd`, `.xar` and the
usual double suffixes (`.tar.gz`, `.tbz2`, `.txz`, …), including encrypted
archives and split volumes.

Extraction runs **in-process**: the payload embeds its own engine built from
UnRAR (RAR), the LZMA SDK (7z) and minizip-ng + zlib (zip/zipx) — no external
helper ELF is required. Encrypted archives (ZipCrypto, AES for zip/7z/RAR) are
supported directly: when an archive needs a password the UI asks for one and
passes it to the engine. Progress is reported per entry, and an interrupted
extraction can be resumed after the payload restarts. Split volumes may be
enqueued from any part — the engine normalises them to the first volume.

### Packages

- Scan storage for packages and show them as a library with cover art (read out
  of the package's own `sce_sys/icon0.png`).
- Read package metadata (title, title id, version, size) without installing.
- **fPKG / decrypted folder**: these have no Sony install entry point, so
  "install" means copying them into `/data/homebrew`, where a mounted-folder
  payload picks them up.
- Regular `.pkg` files are handed to the system installer, which accepts both
  local paths and `http://` URLs as the package URI.
- **Install straight from a PC over LAN** (`PKG管理 → 从 PC 安装`): run
  `tools/serve-pkg.py` in the folder with your `.pkg` files, enter the PC
  address in the console UI, and pick from the card list. The system installer
  downloads the file itself (Range requests) — the package never touches the
  console disk. The PC-side server is a single dependency-free Python script
  (adapted from Loopayeh/pkg-sender's `serve_pkg.py`, MIT).

### Saves

- List installed titles and their save data.
- **Backup always snapshots first.** Restoring a save takes a snapshot of the
  current data before writing anything back, and a failed restore rolls back
  automatically — there is no code path that overwrites a save without a
  recoverable copy existing first. Snapshots live in `/data/savesnap` (never
  inside `/data/save_files`, which the system prunes).

### Tasks and diagnostics

- Every long operation (pull, extract, upload, install, backup, restore, fPKG
  copy, delete) runs in one task queue, so progress has a single source of
  truth, and shows up inline in whichever view started it.
- Cancel a running task; finished tasks are reported and then dropped.
- `GET /api/diag` returns the request ledger: a ring of recent requests with a
  `done` flag each. If the process dies mid-request on the console there is no
  core dump to inspect — the last entry **without** a `done` record is the
  culprit. This is how the "page opens, then one click kills it" class of bug
  gets localised.
- `GET /api/status` reports the listen port, the console's LAN address, and the
  version that is currently running.
- `POST /api/shutdown` stops the payload. There is no service manager on the
  PS5, so nothing brings it back and the web UI dies with it — hence the double
  confirmation.

## Build

Requires the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk#quick-start):

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make                 # -> PS5-Nexus-v1.0.0.elf
```

`libmicrohttpd` is linked from the SDK. `make` checks for it and runs
`install-libmicrohttpd.sh` if it is missing; on a host without network access,
point it at a tarball you downloaded yourself
(`LIBMICROHTTPD_TARBALL=/path/to/libmicrohttpd-1.0.1.tar.gz ./install-libmicrohttpd.sh`).

For local UI work on Linux — same code, no PS5 kernel calls, includes the
`/icon0.png` favicon:

```sh
make linux           # -> PS5-Nexus-linux-v1.0.0
./PS5-Nexus-linux-v1.0.0
```

`make NAS=0` drops the vendored SMB/NFS backends and their libraries.

> ⚠️ Bump `VERSION_TAG` in the Makefile **and** keep the version inside `$(BIN)`.
> A version that is not in the artifact name is invisible on the console, and a
> `VERSION_TAG`-only change would additionally be missed by `make` (the compiler
> flags are not a prerequisite of anything) — so the version is part of the
> target name on purpose.

## Deploy

Send the built ELF to an ELF loader on the console (the usual listener is
`9021`) — or drop it into a payload manager's directory:

```sh
export PS5_HOST=ps5_ip_address
nc -q0 "$PS5_HOST" 9021 < PS5-Nexus-v1.0.0.elf
```

The console shows a notification with the version and the actual listen port.
Then open, from any device on the LAN:

```text
http://<ps5-ip>:2026/
```

The payload listens on `2026` and falls back to the next free port if it is
taken — the notification and the top bar both report the real one. On startup it
also registers a home-screen launcher (title id `NEXS88888`, name *PS5 Nexus*)
when it is missing; the UI exposes the same action as a button, because
registering it touches the kernel and must never happen by itself while a game
is running.

Loading the payload again replaces the running instance: the new process reaps
older ones by process name before binding, so you always end up with a single
instance on a predictable port.

## Host testing

`hosttest.py` runs the real binary against a real HTTP client and a real SMB
server, exercising the wire formats the browser actually sends:

```sh
python3 hosttest.py                 # 79 assertions, needs a smbd fixture on :1500
```

Helper scripts live in `.build/` (see `.build/build-verify-wsl.sh`).

The host build honours a few environment variables so paths and fixtures can be
redirected away from the console's:

| Variable | Default | Purpose |
| --- | --- | --- |
| `WFM_SAVE_ROOT` | `/user/data`, `/data`, `/mnt/ext1/user/data` | save-directory probe |
| `WFM_SNAPSHOT_DIR` | `/data/savesnap` | save snapshots |
| `WFM_HOMEBREW_DIR` | `/data/homebrew` | fPKG deployment target |
| `WFM_DEVICE_ROOT_PREFIX` | `/mnt` | device-root mount gate |

## Notes and limits

- One task at a time. File operations that would start a second long task are
  rejected while one is running.
- Delete is recursive and permanent; there is no recycle bin.
- Upload and download are hidden in the PS5's own browser — they are meant for
  another device on the network.
- Copied and moved files are created `0777` on filesystems that support Unix
  modes; FAT/exFAT ignore this. Modes are set after creation, since the umask
  would otherwise trim them.
- The interface is Simplified Chinese. `assets/lang-*.js` are placeholders for
  future localisation; the UI text currently lives in `assets/index.html`.
- Archive extraction runs in-process (UnRAR / LZMA SDK / minizip-ng, vendored
  under `third_party/`); no external helper payload is involved.

## Credits

Built on the work of others:

- **[owendswang/ps5-web-file-manager](https://github.com/owendswang/ps5-web-file-manager)** — the back end this project is derived from. GPL-3.0.
- **[ps5-payload-dev/websrv](https://github.com/ps5-payload-dev/websrv)** — HTTP server structure, static asset embedding, PS5 PKG install function. GPL-3.0+.
- **[ps5-payload-dev/ftpsrv](https://github.com/ps5-payload-dev/ftpsrv)** — payload conventions, home-screen launcher/install flow. GPL-3.0+.
- **[ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk)** — build foundation. GPL-3.0+.
- **[libsmb2](https://github.com/sahlberg/libsmb2)** — SMB2/3 client, vendored. LGPL-2.1+.
- **[libnfs](https://github.com/sahlberg/libnfs)** — NFSv3/v4 client, vendored. LGPL-2.1+ (BSD for the rpcgen-derived protocol files).
- **[libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/)** — embedded HTTP server, linked from the SDK. LGPL-2.1+.
- **[UnRAR](https://www.rarlab.com/rar_add.htm)** — RAR extraction (vendored, `third_party/unrar7`). Non-free but redistributable licence, extraction only.
- **[7-Zip / LZMA SDK](https://www.7-zip.org/)** — 7z extraction (vendored, `third_party/7z`). LGPL-2.1+ / unRAR restriction.
- **[minizip-ng](https://github.com/zlib-ng/minizip-ng)** — zip/zipx extraction incl. crypto backends (vendored, `third_party/minizip-ng`). Zlib licence.
- **[zlib](https://zlib.net/)** — DEFLATE (vendored, `third_party/zlib`). Zlib licence.
- **[owendswang/wfm-7zip-helper](https://github.com/owendswang/wfm-7zip-helper)** — the helper-payload approach this project originally used; it has since been replaced by the in-process engine below, thanks for showing the way.
- **[seregonwar/zftpd](https://github.com/seregonwar/zftpd)** — socket buffer tuning reference. MIT.
- **[itsPLK/ps5-payload-manager](https://github.com/itsPLK/ps5-payload-manager)** — payload packaging reference. GPL-3.0.
- **[etaHEN](https://github.com/etaHEN/etaHEN)** — ShellUI URI navigation used before exit. GPL-3.0.

## License

GPL-3.0-or-later. See `LICENSE`.

Bundled third-party libraries keep their own licences — see
`THIRD_PARTY_NOTICES`. If you distribute binaries, honour the LGPL terms for
libsmb2, libnfs and libmicrohttpd in addition to this project's GPL.

PS5, PlayStation and related marks are trademarks of Sony Interactive
Entertainment. This is unofficial homebrew, not affiliated with or endorsed by
Sony.
