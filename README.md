# Atrim for Linux

Qt 6 launcher for [Atrim](https://github.com/roozerxc/atrim) — a custom client for Amnesia: The Dark Descent.

One app: point it at your game folder, pick a build, install. It downloads from the official Atrim GitHub releases, checks what it can, backs up what it will overwrite, then drops the files in place.

**You need a legal copy of Amnesia: The Dark Descent.** Steam, GOG, Epic, Humble, DVD, whatever — as long as it’s yours. Cracked copies may work; they are neither tested nor supported.

Amnesia, HPL, and related marks are © / ™ Frictional Games. This is unofficial and not affiliated with them.

## What it does, in plain words

- **Scan** — looks through Steam libraries, Heroic, common game folders, and mounted drives for an Amnesia install. You can also pick the folder by hand.
- **Install** — downloads the selected Atrim zip, checks size (and SHA-256 when we know it), extracts, backs up overlapping game files + known save dirs, then copies into the game folder.
- **Restore** — picks a previous backup under `~/.local/share/atrim-backups/` (or `$XDG_DATA_HOME/atrim-backups/`) and puts those files back.
- **Cancel download** — aborts a download in progress and throws away the partial file.
- **Free space check** — refuses to start if the volume holding the game folder looks too tight for zip + extract + backup.
- **OpenGL check** — Amnesia wants OpenGL 2.1+. Shows vendor / renderer / version so you know if you’re on a real GPU or something like llvmpipe.
- **Remembers** last game folder and last selected version.

Release list: tries the GitHub API first so new assets show up without a launcher update. If the network or API is dead, it falls back to a built-in list with pinned sizes and checksums.

## What you need

| Need | Notes |
|------|--------|
| Amnesia install | Folder must be **writable** (Proton/Steam ownership can block this). |
| Qt 6 | Widgets, Network, OpenGL — if you build from source. |
| Extractor | One of: `unzip`, `7z` / `7zz` / `7za`, `bsdtar`, or `python3` (zipfile). |
| Runtime to play | Atrim ships Windows binaries — run them through **Proton** (or similar). |

No `curl` / `wget` required for downloads; Qt Network handles that. No systemd, no special init.

Works with any legit copy of the game (Steam, GOG, Epic, Humble, DVD, …). Tested mainly on Arch / CachyOS; should be fine on anything that can run a Qt 6 app or AppImage.

## Usage

1. Start the launcher.
2. **Scan Steam and Heroic locations**, or **Choose game folder…**
3. Pick an Atrim version.
4. **Install selected Atrim version** — confirm when asked.
5. Launch the game the way you always do (Steam/Heroic + Proton, etc.).

Optional:

- **Restore from backup…** — after a bad install or if you want the previous files back.
- **Check OpenGL** — if the game won’t start or looks software-rendered.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
