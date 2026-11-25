# YouTubeOnVita-NoAds v0.0.3 — Tweaks and build regression fix 🇧🇷

Independent fork of **[YouTubeOnVita by Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita)**. Most features of the YouTube TV interface, native playback and downloads are from the original project. **SponsorBlock is the primary added user-facing capability in this fork.**

## Changes since v0.0.2

- **Fixed GitHub Actions regression:** after the settings page was renamed, the source-only test still expected `kPageModifications`, causing the build to fail with `AssertionError [ERR_ASSERTION]: kPageModifications`. Updated the test assertions to the current `kPageTweaks` identifier. **This was a CI/test issue, not evidence of a PS Vita runtime crash.**
- **Settings → Tweaks** (Português: *Modificações*): the AdBlock and SponsorBlock controls are grouped together. Both choices are saved under the fork's own data directory. SponsorBlock remains off by default.
- **Internal application version:** `00.03` in `sce_sys/param.sfo`, corresponding to fork v0.0.3.
- **Existing fork identity maintained:** `YTVNX0001` (Full) and `YTVNX0002` (Lite), so updating the existing fork should not replace original YouTubeOnVita (`YTVCB0001` / `YTVCB0002`).
- **Independent data directory:** `ux0:data/youtubevita-noads/` for sessions, settings, cache, downloads and logs; no automatic migration from old `ux0:data/youtubevita/`.

## Possible V8 crash mitigation — not confirmed

The experimental V8/Starboard data-mapping change carried forward from the fork's earlier builds **may improve stability** in some circumstances. Previous real-device crash dumps showed a **read-side translation fault (`DFSR=7`)**; those observations alone do not establish the cause or prove the mapping change fixed it. This release **does not introduce a newly demonstrated fix for that crash**. Longer comparable PS Vita hardware sessions and redacted crash evidence are still needed. Occasional video/UI distortions remain under investigation.

## Install / upgrade

Install **Full** or **Lite** through VitaShell. Both can coexist. Upgrading from v0.0.2 keeps the same fork title IDs and data directory. Users upgrading directly from v0.0.1 may need to sign in again because the newer fork no longer reads the legacy app data folder.

Requires **kubridge 0.3.1+** and **libshacccg** at `ur0:data/libshacccg.suprx`.

## Verification and limits

CI must pass source-level tests, compile Full and Lite VPKs with VitaSDK, verify each archive and its internal version/title IDs, and validate the embedded custom Cobalt runtime checksum. The integrated fork has previously been exercised on real PS Vita hardware, but **this exact v0.0.3 package has not yet been independently hardware-tested**. This release is marked as a **prerelease** until that validation is available.

Proudly made in 🇧🇷. Unofficial and unaffiliated with YouTube, Google or Sony.

## Credits

**Original project:** [YouTubeOnVita by Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita).

**Special thanks to Wolff's Room**, original creator and principal developer. The real YouTube TV experience on PS Vita and most existing features are his work. This fork builds on that foundation.

Additional thanks to Cobalt, VitaSDK, vitaGL contributors and the PS Vita homebrew community.
