# Starboard Vita Platform 15 — API 15 probe

Not the official YouTube app; no user interface and no video.
Source uses original Cobalt 25 LTS Starboard API 15 headers from `upstream-25/`.
A partial Vita-specific implementation of time, logging, heap and system paths is compiled and linked into a real VPK.

Build on PC-Wolff from `YouTubeVita/`:

```powershell
docker run --rm -v "${PWD}:/src" -w /src -e VITASDK=/usr/local/vitasdk vitasdk/vitasdk:latest sh -lc "cmake -S . -B build-starboard15 -DCMAKE_BUILD_TYPE=Release && cmake --build build-starboard15 -j4"
```

Vita3K app title: `YTVSB0001`.
Log in emulated guest `ux0:data/youtubevita-noads/starboard15.log`.
Do **not** represent this as Cobalt browser working, because it is a platform ABI/API test only.

