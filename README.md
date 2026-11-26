
<div align="center">
  <picture>
    <source
      media="(prefers-color-scheme: dark)"
      srcset="https://github.com/user-attachments/assets/71d14b35-5d7d-43ef-a3bd-1371da640aae"
    />
    <source
      media="(prefers-color-scheme: light)"
      srcset="https://github.com/user-attachments/assets/e60897d8-5614-4b38-9751-020c74b78062"
    />
    <img
      src="https://github.com/user-attachments/assets/e60897d8-5614-4b38-9751-020c74b78062"
      alt="YouTube on PS Vita"
      width="60%"
    />
  </picture>
</div>

<div align="center">

_Unofficial client for PS Vita_

**The real YouTube on TV experience, brought to PlayStation Vita through Cobalt.**

**This fork is proudly made in 🇧🇷**

</div>
<br>

**Most playback features and built-in tools listed below are inherited from the original YouTubeOnVita project by Wolff's Room. The key functional addition in this fork is SponsorBlock integration; the fork also carries separate experimental filtering, memory diagnostics, and compatibility work.**

**YouTubeOnVita** runs the genuine **YouTube on TV** interface on **PlayStation Vita**, powered by Cobalt and Starboard 15. It is **not** a custom recreation. Browse, search, sign in and watch videos using native buttons and touchscreen input, without a companion PC or streaming server. The early handmade prototype is archived in `legacy/native-mock-v001/` and is not part of this release.

> [!IMPORTANT]
> **This is the genuine YouTube on TV frontend running through Cobalt**, not a recreated or simulated interface.

## Project Status

<p align="center">
  <a href="#overview"><img src="https://img.shields.io/badge/PLATFORM-PS%20VITA-006fcd?style=for-the-badge&logo=playstation&logoColor=white" alt="Platform"></a>&nbsp;&nbsp;
  <a href="#how-it-works"><img src="https://img.shields.io/badge/ENGINE-COBALT%20%2B%20STARBOARD-455a64?style=for-the-badge" alt="Engine"></a>&nbsp;&nbsp;
  <a href="#download"><img src="https://img.shields.io/badge/BETA-v0.0.4-beta.1%20FULL%20%2B%20LITE-2ea44f?style=for-the-badge" alt="Latest Release"></a>&nbsp;&nbsp;
  <a href="https://github.com/joaopedr00000/YouTubeOnVita-NoAds/releases"><img src="https://img.shields.io/github/downloads/joaopedr00000/YouTubeOnVita-NoAds/total?style=for-the-badge&color=blue&logo=github" alt="Total Downloads"></a>
</p>


<div align="center">
  <p><em>Independent fork maintained by <a href="https://github.com/joaopedr00000">joaopedr00000</a>, proudly made in 🇧🇷. Based on the original homebrew port by <a href="https://github.com/WolffsRoom">Wolff's Room</a>.</em></p>
  <a id="support-this-and-other-projects"></a>

  <em>If you enjoy my work, consider supporting the development!</em>
  <br>
[<img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="48">](https://www.buymeacoffee.com/5rsrt7j4z8f)
</div>

---

<h2 align="center">Table of Contents</h2>

<details open>
<summary><b>Project and Features</b></summary>

- [Overview](#overview)
- [Download — Full or Lite](#download)
- [Features](#features)
- [Controls](#controls)
- [Cobalt and  FPS](#cobalt-and-fps)
- [Native Add-ons](#native-add-ons)
- [Screenshots](#screenshots)

</details>

<details>
<summary><b>Technical and Development</b></summary>

- [How It Works](#how-it-works)
- [Video and Audio](#video-and-audio)
- [Google Sign-in and  Security](#google-sign-in-and-security)
- [Known Limitations](#known-limitations)
- [Building and  Testing](#building-and-testing)
- [Development History](#development-history)
- [Credits and Disclaimer](#credits-and-disclaimer)

</details>

---

## Download

| Version | Download | Memory | Recommended use |
|:--|:--|:--|:--|
> **Public beta v0.0.4-beta.1:** serial V8 garbage collection; the maintainer observed no crash during roughly 15 minutes of playback and video switching, but a definitive fix is **not confirmed**. [Release notes](RELEASE_NOTES_v0.0.4-beta.1.md). The previous [v0.0.3 prerelease](https://github.com/joaopedr00000/YouTubeOnVita-NoAds/releases/tag/v0.0.3) remains available.

| **YouTube Full** | **[YouTube-Full-v0.0.4-beta.1.vpk](https://github.com/joaopedr00000/YouTubeOnVita-NoAds/releases/download/v0.0.4-beta.1/YouTube-Full-v0.0.4-beta.1.vpk)** | Extended RAM | **Recommended:** more memory available to the player |
| **YouTube Lite** | **[YouTube-Lite-v0.0.4-beta.1.vpk](https://github.com/joaopedr00000/YouTubeOnVita-NoAds/releases/download/v0.0.4-beta.1/YouTube-Lite-v0.0.4-beta.1.vpk)** | Normal RAM | Lower memory footprint, better coexistence with suspended applications |

### Requirements

- **kubridge v0.3.1 or newer** ([bythos14/kubridge](https://github.com/bythos14/kubridge/releases/latest)) in the `*KERNEL` section of `ur0:tai/config.txt`, then reboot. The older kubridge v0.1 (TheOfficialFloW) lacks a memory function the app needs.
- **libshacccg: `ur0:data/libshacccg.suprx`** (the system shader compiler, extracted with [ShaRKF00D](https://github.com/Rinnegatamante/ShaRKF00D)). The app ships its shaders precompiled, but any shader missing from that set needs the compiler.

### Install and update

Install using **FMVita/VitaShell** or the [**Wolff's Store**](https://wolffsroom.github.io/WolffsRoomSite/).
Both editions can be installed side by side, with separate LiveArea bubbles (**YTVNX0001** / **YTVNX0002**). To update, install the new VPK over the old one. **Fork v0.0.3** stores sessions, settings, downloads, cache and logs under `ux0:data/youtubevita-noads/`, separate from the original application's directory. v0.0.1 used `ux0:data/youtubevita/`; **v0.0.2 does not automatically migrate credentials or other files**. Sign in again after updating; do not delete the old data directory. See [fork data isolation notes](docs/FORK_DATA_ISOLATION_V0_0_2.md).

> [!TIP]
> **Crash right after the startup animation?**
> Check both requirements above. To report a crash, attach only `cobalt_boot.log`, `starboard15.log` and the `*_prev.log` files from `ux0:data/youtubevita/`, plus the newest `psp2core-*.psp2dmp` crash dump (usually in `ux0:data/`).

Also available through **[Wolff's Room](https://wolffsroom.github.io/WolffsRoomSite/#youtube-on-vita)** and the Wolff's Store app.

---

## Features

**Credit:** The following YouTube TV, video playback, downloads and native PS Vita tools are mainly features of the original project. They are listed for compatibility, **not as new developments introduced by this fork**.

- The **real YouTube on TV** interface: browsing, search, sign-in and player controls.
- Native **buttons, touchscreen and microphone integration**.
- **H.264/AAC playback up to 720p**, including compatible 60 FPS streams and non-standard sizes (e.g. 1190×720); optional Full HD (beta).
- **Shorts** playback, with a **Vertical mode** that turns the Vita sideways like a phone and swipes between Shorts.
- **Aspect Ratio** (Original / Zoom / Stretch) and **Video filtering** (Bilinear / Nearest).
- **Analog sticks:** left stick navigates; right stick sets volume and brightness.
- Native **Add-ons** for screenshots, sleep timer, display, sound and screen-orientation settings.
- Native **UI sounds**, notifications and an offline / reconnect screen.
- **MP4 video and MP3 audio downloads** with square artwork on new MP3s.
- **Encrypted local session data**, custom LiveArea and animated startup.

The Vita display is **960 × 544**, so 720p video is scaled down to fit. Some formats and protected content are unsupported.

---

## Controls

<table align="center">
<thead><tr><th align="center">Control</th><th align="center">Function</th></tr></thead>
<tbody>
<tr><td align="center"><img src="assets/sony-buttons/up.png" alt="up" width="22"> <img src="assets/sony-buttons/left.png" alt="left" width="22"> <img src="assets/sony-buttons/down.png" alt="down" width="22"><br>D-pad</td><td align="center">Navigate menus and video controls</td></tr>
<tr><td align="center"><img src="assets/sony-buttons/cross.png" alt="cross" width="22"></td><td align="center">Select / confirm</td></tr>
<tr><td align="center"><img src="assets/sony-buttons/circle.png" alt="circle" width="22"></td><td align="center">Back / close</td></tr>
<tr><td align="center"><img src="assets/sony-buttons/square.png" alt="square" width="22"> <img src="assets/sony-buttons/triangle.png" alt="triangle" width="22"></td><td align="center">Context-dependent YouTube controls</td></tr>
<tr><td align="center"><img src="assets/sony-buttons/Select.png" alt="Select" width="52"></td><td align="center">Open / close native Add-ons</td></tr>
<tr><td align="center"><img src="assets/sony-buttons/touchpad.png" alt="touchpad" width="38"><br>Front touchscreen</td><td align="center">Tap: Select · Swipe: Navigate<br>Two-finger tap: Back · Three-finger tap: Add-ons<br>Vertical mode: swipe = next / previous Short · tap = play / pause</td></tr>
<tr><td align="center">Left stick</td><td align="center">Navigate (same as D-pad)</td></tr>
<tr><td align="center">Right stick</td><td align="center">Right / Left: volume up / down<br>Up / Down: brightness up / down</td></tr>
<tr><td align="center">L / R</td><td align="center">Player navigation (when enabled)</td></tr>
<tr><td align="center"><img src="assets/sony-buttons/Start.png" alt="Start" width="52"></td><td align="center">During playback: Subtitles key (availability depends on the video)</td></tr>
</tbody>
</table>

<p align="center"><sub>Controls depend on the current YouTube screen.</sub></p>

---

## Cobalt and FPS

Cobalt is designed for television applications. **It does not continuously redraw a static interface when nothing has changed**, so an FPS counter may show very low values while the menu is idle. That alone is **not a performance problem**; avoiding redundant work can also conserve CPU/GPU time and battery life.

**Playback supports up to 60 FPS**, subject to the video's frame rate, chosen resolution, decoding limits and device conditions. This does not mean that every stream or menu is guaranteed to run at 60 FPS.

**Low-clock test:** video playback was also tested at **222 MHz CPU / 111 MHz GPU**, with satisfactory playback for the tested content.

---

## Native Add-ons

Open **Add-ons** with **SELECT** or a three-finger tap. These tools extend the original YouTube interface without replacing it.

| Section | Options |
|:--|:--|
| **Player settings** | Download, screenshots, sleep timer (from 5 min), Black Screen, Darken Screen, Video filtering, Aspect Ratio and Vertical (Shorts) |
| **Downloads** | MP4 video or locally encoded MP3 audio, saved list with 5 items per page |
| **Settings** | Download quality and format, L/R controls, input lock, language, Inverted screen, and a dedicated **Modifications** submenu for the independent playback filters |
| **Screen and info** | Full HD (beta), performance, clock, battery, UI sounds and UI sounds volume |
| **Vita Settings** | System Settings shortcut (subject to available memory) |

<table align="center">
  <tbody>
    <tr>
      <td align="center"><a href="assets/Add-ons/en/1-home.png"><img src="assets/Add-ons/en/1-home.png" alt="Add-ons" width="150"></a><br><sub>Add-ons</sub></td>
      <td align="center"><a href="assets/Add-ons/en/2-player-settings.png"><img src="assets/Add-ons/en/2-player-settings.png" alt="Player settings" width="150"></a><br><sub>Player settings</sub></td>
      <td align="center"><a href="assets/Add-ons/en/3-download.png"><img src="assets/Add-ons/en/3-download.png" alt="Download options" width="150"></a><br><sub>Download options</sub></td>
      <td align="center"><a href="assets/Add-ons/en/4-downloads.png"><img src="assets/Add-ons/en/4-downloads.png" alt="Downloads" width="150"></a><br><sub>Downloads</sub></td>
      <td align="center"><a href="assets/Add-ons/en/5-settings.png"><img src="assets/Add-ons/en/5-settings.png" alt="Settings" width="150"></a><br><sub>Settings</sub></td>
      <td align="center"><a href="assets/Add-ons/en/6-screen-and-info.png"><img src="assets/Add-ons/en/6-screen-and-info.png" alt="Screen and Info" width="150"></a><br><sub>Screen &amp; Info</sub></td>
      <td align="center"><a href="assets/Add-ons/en/7-no-internet.png"><img src="assets/Add-ons/en/7-no-internet.png" alt="No internet" width="150"></a><br><sub>No internet</sub></td>
    </tr>
  </tbody>
</table>

### MP4 and MP3 downloads

Save available videos as **MP4** or encode audio locally to **MP3 (160 kbps)**. New MP3s may include **1:1 square cover art**. With fork v0.0.3, files are saved to `ux0:data/youtubevita-noads/downloads/`.

Available qualities depend on YouTube's provided streams. Only download content you have permission to save.

### Partial AdBlock

The optional AdBlock filters a small list of advertising domains. It **cannot reliably block YouTube ads delivered through first-party video endpoints**.

---

## Screenshots

Screenshots of the actual YouTube interface captured in **Vita3K**.

| Home / Browse | YouTube Music |
|:--:|:--:|
| <img src="assets/prints/new/Home-Browse.png" width="420" alt="Home / Browse"> | <img src="assets/prints/new/YouTube%20Music.png" width="420" alt="YouTube Music"> |

| Search | Video playback |
|:--:|:--:|
| <img src="assets/prints/new/Search.png" width="420" alt="Search"> | <img src="assets/prints/new/Video.png" width="420" alt="Video playback"> |

| Captions | Shorts |
|:--:|:--:|
| <img src="assets/prints/new/Captions.png" width="420" alt="Captions"> | <img src="assets/prints/new/Shorts.png" width="420" alt="Shorts"> |

| Play Music | Lyrics |
|:--:|:--:|
| <img src="assets/prints/new/Play%20Music.png" width="420" alt="Play Music"> | <img src="assets/prints/new/Lyrics.png" width="420" alt="Lyrics"> |

| Movies Player | Movies Trailer |
|:--:|:--:|
| <img src="assets/prints/new/Movies%201.png" width="420" alt="Movies 1"> | <img src="assets/prints/new/Movies%202.png" width="420" alt="Movies 2"> |

See [all screenshots](assets/prints/new/) or the [Wolff's Room release post](https://wolffsroom.github.io/WolffsRoomSite/posts/youtube-on-vita/).


---

## How It Works

Cobalt is **not Chromium**. It is a Google-developed runtime for HTML5-based applications on televisions, streaming devices and game consoles. It uses technologies such as the V8 JavaScript engine and Skia, but it is not a full desktop browser with tabs and a general-purpose Chromium feature set.

The Vita port provides the **Starboard** implementation required to connect Cobalt to the console's operating system and hardware.

```text
  YouTube on TV (HTML / CSS / JavaScript)
                     |
                     v
  Cobalt Evergreen (V8 / Skia / Web Runtime)
                     |
                     v
          Starboard 15 (PS Vita Port)
                     |
     +---------------+---------------+---------------+---------------+
     |               |               |               |               |
     v               v               v               v               v
  Graphics        Network         Input / I/O     Native Media    System
  VitaGL / GXM    HTTP / TLS       SceCtrl         SbPlayer        VitaSDK
     |               |            SceTouch            |               |
     v               v               v               v               v
   SceGxm       Vita Network      SceIo           SceAvcdec     Vita Kernel
                                                   SceAudiodec
     |               |               |               |               |
     +---------------+---------------+---------------+---------------+
                                     |
                                     v
                              PlayStation Vita
```

### Native runtime and loader

The project uses a native ARM32 loader for the matching **Cobalt Evergreen 5.50.2 / Starboard 15** binary. It performs segment loading, relocation, external symbol resolution, executable-memory protection, C++ static initialization and dispatch of Cobalt's native event entry point.

The runtime connects YouTube's application logic to Vita system services through Starboard.

### Graphics

The graphics implementation connects Cobalt's EGL/GLES expectations to **VitaGL** and the Vita graphics stack. It supplies the EGL interfaces required by the browser, translates missing GLES behavior where possible, and manages shader compilation and caching. The port has been tested rendering YouTube interface elements, thumbnails and video frames in Vita3K.

---

## Video and Audio

### Video

Cobalt supplies encoded media samples to the Vita's **SbPlayer** implementation, which uses the native H.264 decoder and submits YUV video surfaces to the GPU.

The Vita's hardware decoder accepts compatible **1280 × 720** streams. Non-standard sizes (such as 1190 × 720, 240 × 426 or 720 × 720) are decoded into an aligned layout and cropped by the GPU, and portrait Shorts up to **480 × 854** use a dedicated portrait decoder. Full HD is an optional beta mode and is not guaranteed on every device.

### Audio

AAC-LC media samples are decoded through `SceAudiodec` and sent to the Vita audio output. The port also provides optional native UI sounds. The earlier AAC/ADTS framing fixes and buffering changes remain part of this release.

### Microphone

Microphone input is integrated through `sceAudioIn` for supported voice features. YouTube controls whether its own voice-search interface is available.

---

## Google Sign-in and Security

The app uses **YouTube's own sign-in flow inside Cobalt**. Wolff's Room does not operate a separate Google sign-in server or request your password.

- Supported network requests use **HTTPS/TLS**, including certificate verification.
- Saved session records are **encrypted and authenticated at rest using XChaCha20-Poly1305**.
- Local storage uses a key derived in part from the Vita's device identity.

These measures help protect local data but do **not** make the client an official Google-certified device or a hardware-secure vault. Use normal precautions with accounts and modified consoles.

---

## Known Limitations

> [!WARNING]
> **v0.65:** A crash when switching videos (inside Cobalt's JavaScript engine, seen since v0.53) is still under investigation. Crash logs are welcome: since v0.65 the previous session's logs are kept as `cobalt_boot_prev.log` / `starboard15_prev.log`.

- **YouTube Full** reserves more RAM; **Lite** leaves more available for suspended applications.
- **Full HD (beta):** if no video plays with it enabled, turn it off in Add-ons.
- **Vertical mode** covers the YouTube player controls (they still work, just hidden). **Inverted screen** is experimental.
- Opening comments with Square is not available yet.
- Certain video formats, DRM-protected content and Full HD streams may not work.
- The YouTube service may change on uncertified devices.

For crash reports, share reproduction steps and the applicable logs (`ux0:data/youtubevita/` for v0.0.1, or `ux0:data/youtubevita-noads/` for v0.0.2 and later), **not stored account or session data**.

---

## Building and Testing

**For developers.** The public VPKs are ready to install; compiling them requires a **VitaSDK / Docker** environment and runtime dependencies.

### Prerequisites

- Docker and the `vitasdk/vitasdk:latest` image.
- Git with submodule support, CMake and Python 3.
- Matching, legally obtained **Cobalt Evergreen / Starboard 15** runtime and required assets (not all are redistributable in this repository).
- Required VitaGL, VitaSDK, TLS and media dependencies.

### Local build

Clone the repository with submodules:

```powershell
git clone --recursive https://github.com/WolffsRoom/YouTubeOnVita.git
cd YouTubeOnVita

# Build both versions (recommended)
powershell -ExecutionPolicy Bypass -File tools/build_youtube.ps1 -Variant Both

# Or build one version
powershell -ExecutionPolicy Bypass -File tools/build_youtube.ps1 -Variant Normal
powershell -ExecutionPolicy Bypass -File tools/build_youtube.ps1 -Variant Lite
```

The script can also use `-Configuration Debug`. It puts the resulting packages in `current/`.

| Build | Output | Title ID | Vita memory mode |
|:--|:--|:--|:--|
| Full | `build-youtube/YouTube.vpk` | `YTVCB0001` | Extended (`ATTRIBUTE2=12`) |
| Lite | `build-youtube-lite/YouTube-Lite.vpk` | `YTVCB0002` | Normal (`ATTRIBUTE2=0`) |

**Note:** The proprietary Cobalt runtime and certain supporting assets are intentionally not included in the public source. A fresh clone alone may not produce a VPK until these prerequisites are supplied through authorized means.

### Vita3K testing

After building, run `./tools/test_cobalt_boot_vita3k.ps1 -Seconds 90`. Diagnostic logs and captures are stored under `tools/capture/cobalt-boot-*/`. Results on real hardware may differ.

---

## Development History

| Version / milestone | Highlights |
|:--|:--|
| **v0.01 — Prototype** | Original handmade interface, later archived in favor of real YouTube on TV |
| **Starboard foundation** | ARM ELF loader, native filesystem, networking and event loop |
| **Cobalt bootstrap** | Cobalt event entry point working in Vita3K |
| **v0.18 – v0.20** | Navigation, early hardware feedback, performance and runtime improvements |
| **v0.21 – v0.22** | Native media player, microphone capture, AAC/ADTS fix |
| **v0.23 – v0.26** | Touch gestures, hardware decode tiers, synchronization and session persistence |
| **v0.27 – v0.28** | LiveArea graphics and packaging improvements |
| **v0.34 – v0.55** | YouTube frontend integration, Add-ons, downloads and additional testing |
| **v0.56 – v0.57** | Unified build experiments, HD playback diagnostics, animated startup and MP3 cover art |
| **v0.58 - First Release** | **Public Full and Lite VPKs, new README, screenshots and GitHub release** |
| **v0.59 – v0.61** | Video artifact fixes, Video filtering, Downloads pages, offline recovery, startup diagnostics |
| **v0.62** | New startup animation, UI sounds and notifications, sleep timer HUD, Aspect Ratio, 1190×720 support |
| **v0.63** | Analog stick controls, media volume and brightness, START = Subtitles |
| **v0.65** | **Shorts + Vertical mode, Inverted screen, non-standard resolutions, Full HD rework, lag, UI-artifact, quality-switch crash and text-background fixes** |

Older detailed technical notes are preserved in [Development Notes Archive](docs/DEVELOPMENT_NOTES_ARCHIVE.md).

---

## Credits and Disclaimer

**This fork:** [joaopedr00000](https://github.com/joaopedr00000) — proudly made in 🇧🇷.

**Original port and development:** [Wolff's Room](https://github.com/WolffsRoom)

This project builds upon the work of:

- [Cobalt](https://github.com/youtube/cobalt) - original HTML5 runtime and Starboard architecture.
- [VitaSDK](https://vitasdk.org/) - homebrew toolchain and Vita interfaces.
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) - Vita graphics integration.
- [Vita3K](https://vita3k.org/) - emulator for development and captures.
- [kubridge](https://github.com/TheOfficialFloW/kubridge) and the [bythos14 fork](https://github.com/bythos14/kubridge) - kernel bridge used for executable-memory integration.

### AI NOTICE

This project was created using AI (Chat GPT Plus and Claude Code Opus 5.5). 

### Visual and audio references

- [YouTube Brand Resources](https://brand.youtube/) - branding and visual reference.
- [YouTube Splash Screen Animation on LottieFiles](https://lottiefiles.com/free-animation/youtube-splash-screen-animation-8wjVJQIQHQ) - inspiration/reference for the adapted startup sequence.
- [iOS System Sounds](https://github.com/extratone/iOSSystemSounds/) - reference/source for the optional UI sound set.

All trademarks, branded assets, music, and third-party libraries remain the property of their respective owners. Their respective licenses and redistribution conditions apply; see `LICENSES.md` and dependency notices.

**YouTube**, the YouTube logo, **Google**, **PlayStation**, **PS Vita** and **Apple** are trademarks of their respective owners. This is an **unofficial community project** and is not affiliated with, sponsored by or certified by Google, YouTube, Sony or Apple.

Use the software responsibly, in accordance with applicable service terms and laws.

---

<div align="center">

<div align="center">
  <picture>
    <source
      media="(prefers-color-scheme: dark)"
      srcset="https://github.com/user-attachments/assets/71d14b35-5d7d-43ef-a3bd-1371da640aae"
    />
    <source
      media="(prefers-color-scheme: light)"
      srcset="https://github.com/user-attachments/assets/e60897d8-5614-4b38-9751-020c74b78062"
    />
    <img
      src="https://github.com/user-attachments/assets/e60897d8-5614-4b38-9751-020c74b78062"
      alt="YouTube on PS Vita"
      width="30%"
    />
  </picture>
</div>

*Unofficial client for PS Vita*

*Real TV frontend by Wolff's Room. Independent fork v0.0.4-beta.1 (experimental beta; crash fix unconfirmed).*

**Vita means life!**

</div>
