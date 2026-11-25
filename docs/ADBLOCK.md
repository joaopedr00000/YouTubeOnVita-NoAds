# Layered AdBlock — status and test plan

## Native DNS (integrated in the existing loader)
The existing setting in SELECT > Settings is retained, with default ON for fresh
installs. An explicit adblock=0 is respected at startup and after config load.
DNS now matches three dedicated ad-domain families at label boundaries, including
case-insensitive names and an optional terminal dot. It never blocks youtube.com,
googlevideo.com, accounts.google.com or ytimg.com wholesale. No TLS changes.

DNS alone cannot remove first-party in-player ads. Existing menu still says
"AdBlock (partial)" intentionally.

## Experimental in-player filtering (not active in the shipped Evergreen runtime)
The original JS filter removes the explicit adPlacements/playerAds/adSlots fields
from recognized player responses in the YouTube TV document. It preserves
streamingData, playabilityStatus, videoDetails and captions; it has no polling
or external dependencies. Hooks JSON.parse, optional Response.json and JSON XHR
getter; other serialization paths and server-stitched ads are not covered.
No SponsorBlock integration or feed filtering yet.

The stock 5.50.2 Gold runtime is a prebuilt ELF with no public document-start
injection API in this Vita port. Merely packaging JS would not run it.
Do not disable CSP/TLS or add an intercepting proxy as a workaround.

For a separate source-build experiment, checkout youtube/cobalt tag 25.lts.50,
then run from this repo:
    python3 tools/prepare_cobalt_adblock.py /path/to/cobalt
This inserts the opt-in hook after CreateGlobalObject in WebModule::Impl.
It requires the --ytv-adblock argument on the experimental core; it is not
wired to the native menu. The replacement core must be rebuilt for the
correct ARM/Starboard ABI. The loader uses version-specific offsets
(relocate_elf.c); do NOT install a newly linked core with those old offsets.
Revalidate the loader, imports, constructors, V8 cache maintenance and runtime
before packaging Full/Lite. No rebuilt core or VPK is included in this change.

## Test
    node tools/test_adblock.js
    c++ -std=c++11 -Wall -Wextra -pedantic tools/test_adblock_dns.cpp -o /tmp/ytv-adblock-test
    /tmp/ytv-adblock-test

On Vita3K and hardware, validate fresh startup, persisted OFF, persisted ON,
login, browse, search, 360/480/720p, seeking, video switches, music, Shorts,
live streams and Full/Lite memory behavior. For the experimental core compare
pre-/mid-roll playback with the flag off/on and verify no regression.
Fixture tests do not establish live ad-block success.

## References (architecture, not copied code)
- https://github.com/TeamNewPipe/NewPipeExtractor
- https://github.com/polymorphicshade/Tubular
- https://github.com/reisxd/TizenTube
- https://github.com/reisxd/TizenTubeCobalt
- https://github.com/youtube/cobalt/tree/25.lts.50
