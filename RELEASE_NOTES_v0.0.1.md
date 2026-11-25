# YouTubeOnVita — v0.0.1

**First public release of this PS Vita fork.** The genuine YouTube on TV experience, powered by Cobalt and Starboard 15 and adapted for native PlayStation Vita controls.

This release focuses on smoother viewing, more consistent playback, practical on-device tools, and a lightweight interface without requiring a PC or streaming server.

## Features

### YouTube experience
- The original YouTube on TV interface: Home, browsing, search, compatible account sign-in, and video playback.
- Native D-pad, face-button, analog-stick, touchscreen and supported microphone controls.
- On-screen player controls and subtitles where available from the video.
- **YouTube Shorts**, including portrait/Vertical mode with touch swipes between videos.

### Video and audio
- Native hardware-assisted **H.264 video playback up to 720p**, including compatible **60 FPS** streams.
- AAC-LC audio decoding through the Vita's native media services.
- Experimental **Full HD** option for compatible streams (not guaranteed on every video).
- **Aspect ratio** settings: Original, Zoom and Stretch.
- **Video filters**: Bilinear and Nearest.
- Volume and brightness adjustment with the right analog stick.
- Refined playback transitions, background media coordination and memory handling to improve continuity during longer sessions.

### Native tools and extras
- Save compatible videos as **MP4** or extract audio as **MP3**, with cover art for supported MP3 downloads.
- Download manager, available quality/format choices and local file browsing.
- Screenshot capture.
- Sleep timer, Black Screen and Darken Screen modes.
- Touch and button navigation options, optional L/R controls and input lock.
- Portrait orientation for Shorts and experimental inverted-screen option.
- Configurable UI sounds and sound volume.
- Device information including battery, clock and performance details.
- Native notifications, connectivity/reconnect handling and a shortcut to Vita Settings.
- Language options and a custom LiveArea/startup experience.
- Local session data protected with authenticated encryption; supported network connections use HTTPS/TLS.

### Reliability improvements in this fork
- Updated V8/Starboard virtual-memory mapping behavior on PS Vita.
- Safer scheduling of playback-related actions on the main event loop.
- Additional periodic playback-memory diagnostics.
- Full and Lite packages created from the same verified Cobalt runtime.

## Which edition should I install?

| Edition | File | Memory profile |
| --- | --- | --- |
| **Full** | `YouTube-Full-v0.0.1.vpk` | Extended memory profile; recommended when available |
| **Lite** | `YouTube-Lite-v0.0.1.vpk` | Standard memory profile; for lower memory usage |

Both editions can coexist; they have separate application IDs. Both contain the same Cobalt runtime. The release tag is **v0.0.1**, the first version of this fork.

## Requirements and installation

1. A homebrew-enabled PlayStation Vita with **kubridge v0.3.1 or newer** properly configured under `*KERNEL`; reboot after installing/updating kubridge.
2. **libshacccg** present at `ur0:data/libshacccg.suprx` (typically installed using ShaRKF00D).
3. Install the preferred VPK with VitaShell or another trusted VPK installer.
4. Preserve and back up `ux0:data/youtubevita/` during updates; it contains settings and local session information.

## Notes

- Playback availability depends on the video, codec, resolution and changes to the YouTube service.
- Some DRM-protected media and formats are unsupported. Full HD and certain screen modes remain experimental.
- For technical diagnostics, preserve `cobalt_boot.log`, `starboard15.log`, and their `*_prev.log` counterparts. Do **not** post raw crash dumps or account/session data publicly.
- Download only content you have permission to save.

## Credits

Based on the independent, unofficial **YouTubeOnVita** project by [Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita), with contributions from the PS Vita homebrew ecosystem and Cobalt/Starboard technologies. This community fork is not affiliated with or endorsed by YouTube, Google, Sony or PlayStation.
