# Fork data isolation for v0.0.2 (development branch)

This document describes **planned v0.0.2 behavior**, not the currently downloadable v0.0.1 VPKs.

## Directory isolation

The PS Vita fork uses `ux0:data/youtubevita-noads/` rather than `ux0:data/youtubevita/`.

The v0.0.2 code routes **Cobalt cookies/session storage, addon settings, cache and temporary files, downloads, download settings, diagnostic switches, player dumps, and crash/startup logs** to the new directory. The Starboard file access checks accept the new root and refuse the original application's root. The source-built Cobalt Starboard overlay uses the same new directory. Screenshots use `ux0:picture/YouTubeVita-NoAds/`.

For encrypted session data, the fork also uses a different key-derivation context (`YouTubeVita-NoAds/storage/v1`) so encrypted session blobs from the original application cannot simply be copied over to the fork and used.

## Upgrade and privacy behavior

- The published v0.0.1 VPKs **still use** `ux0:data/youtubevita/`.
- The future v0.0.2 will **not** automatically import cookies, encrypted credentials, settings, downloads, or other files from that directory.
- Existing files under `ux0:data/youtubevita/` are **left untouched**. The fork creates its own directory; users sign in again and set their preferences when testing a new build.
- Preserve both Title IDs `YTVNX0001` (Full) and `YTVNX0002` (Lite). No new LiveArea bubble is intended.
- Never attach session or encrypted cookie files to bug reports.

## Verification gate before a release

1. Run `node tools/test_fork_data_isolation.cjs` and review the path changes.
2. Compile a test VPK later, then check the `param.sfo` name and version for Full/Lite.
3. On PS Vita, install with the original YouTubeOnVita installed and confirm that both apps have independent sign-in sessions, addons settings, downloads and logs.
4. Confirm that the old directory is not created, read, written, renamed or deleted by v0.0.2, and that the original app still works unchanged.
5. Confirm successful startup, playback, an MP4/MP3 download, and crash log capture from the new directory.

**Status:** source code has been pushed to a development branch. **Not compiled, tested on hardware or released.**
