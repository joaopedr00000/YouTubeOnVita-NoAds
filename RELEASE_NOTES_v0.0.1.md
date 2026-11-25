# YouTubeOnVita-NoAds v0.0.1 — First fork release

This is an **independent fork of [YouTubeOnVita by Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita)**. Thanks to the original author for the project and the extensive native PS Vita work.

## What is different in this fork?

- **SponsorBlock integration** is the main user-facing functionality added by this fork. It can use publicly submitted segment data to skip supported sponsor segments while a video plays.
- This build also includes experimental, fork-specific filtering and runtime/memory diagnostics. These are not upstream features; the V8 memory-mapping change is an **unproven crash mitigation**, not a confirmed fix.
- The Full and Lite VPKs package the same previously verified custom Cobalt binary.

## Features already available in the original YouTubeOnVita

The real **YouTube on TV** interface, search and account sign-in, native Vita controls, H.264 playback up to supported 720p/60fps modes, Shorts, subtitles, native MP4/MP3 downloads, picture settings, UI tools, screenshots, sleep timer and Full/Lite memory editions come principally from the original project. **These should not be presented as new features introduced by this fork.**

## VPKs and installation

- **Full:** `YouTube-Full-v0.0.1.vpk`
- **Lite:** `YouTube-Lite-v0.0.1.vpk`

Requirements: **kubridge 0.3.1+** and **libshacccg** at `ur0:data/libshacccg.suprx`. Install with VitaShell. The fork uses its own title IDs: `YTVNX0001` / `YTVNX0002`, instead of upstream `YTVCB0001` / `YTVCB0002`.

**v0.0.1 shares the legacy data path `ux0:data/youtubevita/`**. Keep it backed up. The later v0.0.2 release will separate the fork's data and will not automatically migrate login sessions.

## Known limitations

The V8 `DFSR=7` translation fault has **not** been conclusively fixed. Full HD and some screen modes are experimental. Report reproducible problems using safe logs; never post credentials, stored sessions or raw memory dumps.

## Credits

Original application and most features: **[Wolff's Room / YouTubeOnVita](https://github.com/WolffsRoom/YouTubeOnVita)**. This fork is not affiliated with YouTube, Google or Sony.
