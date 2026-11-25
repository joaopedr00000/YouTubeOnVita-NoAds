# YouTubeOnVita-NoAds v0.0.2 — Independent fork update 🇧🇷

This is a separate fork of **[YouTubeOnVita by Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita)**. The original author deserves credit for the real YouTube TV frontend and the vast majority of its PS Vita features. **SponsorBlock is the main additional user-facing feature of this fork.**

## Changes in v0.0.2

- **Settings → Modifications:** a dedicated page with independent AdBlock and SponsorBlock controls. SponsorBlock is **disabled by default**, and the preferences are saved locally. The AdBlock setting controls the native layer; the custom Cobalt build also has a separate page-filtering layer which this toggle does **not** necessarily disable.
- **Independent app data:** new `ux0:data/youtubevita-noads/` directory for settings, session storage, downloads, cache, temporary files and logs. Screenshots use `ux0:picture/YouTubeVita-NoAds/`.
- **Clean app names:** `YouTube` (Full) and `YouTube Lite` instead of the temporary `Candidate` label.
- **Internal version:** `00.02` in `sce_sys/param.sfo`.
- **Fork-specific Vita IDs:** `YTVNX0001` and `YTVNX0002`, distinct from original `YTVCB0001/0002`. These IDs are unchanged from fork v0.0.1 to support updating its existing LiveArea bubbles.
- Experimental logging and V8/Starboard work remain in the fork, **without a claim that the observed `DFSR=7` crash has been fixed**.

## Existing original-app features (not introduced by this fork)

YouTube TV browsing and account sign-in, native Vita controls, hardware video decoding, 720p/compatible 60fps playback, Shorts, MP4/MP3 downloads, picture adjustments, screenshots, sleep timer and Full/Lite memory modes are principally **upstream features** created by Wolff's Room.

## Upgrade notes

**No automatic session migration.** v0.0.1 stored settings, downloads and credentials under `ux0:data/youtubevita/`. v0.0.2 does not read or delete that directory; it starts with a separate app configuration. **Sign in again** and configure your settings in the new version. Previous downloads remain in the old folder.

Requires kubridge 0.3.1+ and `ur0:data/libshacccg.suprx`. Install VPKs via VitaShell.

## Validation and known limitations

Packages are intended to be built and verified with VitaSDK and the exact known Cobalt runtime (SHA-256 checks). **Hardware regression testing of v0.0.2 is still pending.** Some image distortions remain under investigation; the V8 memory translation fault is not conclusively resolved. This is a **prerelease** until tested on an actual PS Vita.

Proudly made in 🇧🇷. This independent fork is unofficial and unaffiliated with YouTube, Google or Sony.

## Credits

**Original project:** [YouTubeOnVita by Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita).

**Special thanks to Wolff's Room**, the original creator and main developer of YouTubeOnVita. The YouTube TV experience on PS Vita and the great majority of the app's existing features come from his work. This fork builds on that foundation, with SponsorBlock as its principal added user-facing feature.

Additional thanks to the contributors behind Cobalt, VitaSDK, vitaGL and the wider PS Vita homebrew community.
