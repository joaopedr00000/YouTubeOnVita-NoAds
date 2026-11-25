cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
  else()
    message(FATAL_ERROR "VITASDK required")
  endif()
endif()
project(YouTubeVita_OneToOne LANGUAGES C CXX)
# vita-elf-create appends SCE data (~4.4 KiB) after the R-X segment; with
# 4 KiB pages the RW segment can start too close ("segment 1 overlaps").
# 64 KiB alignment always leaves room (every executable here).
add_link_options(-Wl,-z,max-page-size=0x10000)
# VitaGL built without the boot splash (same commit as the VitaSDK image).
option(YTV_VITAGL_NOSPLASH "Link VitaGL built with NO_SPLASHSCREEN=1" ON)
if(YTV_VITAGL_NOSPLASH)
  link_directories(BEFORE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitagl-nosplash/lib")
endif()
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
option(YTV_BUILD_STARBOARD15_PROBE "Build minimal real Starboard 15 Vita API test" ON)
option(YTV_V8_JITLESS_DIAGNOSTIC "Crash diagnosis: disable V8 JIT (slower, not a proven fix)" OFF)
if(YTV_BUILD_STARBOARD15_PROBE)
  set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt")
  set(SB "${ROOT}/upstream-25")
  set(VITA "${ROOT}/vita")
  set(VITA_APP_NAME "Starboard Vita API Test")
  set(VITA_TITLEID "YTVSB0001")
  set(VITA_VERSION "00.02")
  add_library(starboard_vita15 STATIC "${VITA}/platform15.cpp" "${VITA}/port_window.cpp" "${VITA}/pad_adapter.cpp" "${VITA}/import_lookup.cpp" "${VITA}/port_files.cpp" "${VITA}/port_sync.cpp" "${VITA}/port_network.cpp" "${VITA}/port_socket_waiter.cpp" "${VITA}/port_runtime.cpp" "${VITA}/port_threads.cpp" "${VITA}/port_graphics.cpp" "${VITA}/port_virtual_memory.cpp" "${VITA}/port_storage.cpp" "${VITA}/port_system.cpp" "${VITA}/port_events.cpp" "${VITA}/port_constants.cpp" "${VITA}/port_optional_unsupported.cpp" "${VITA}/port_media_policy.cpp" "${VITA}/port_misc_utility.cpp" "${VITA}/port_audio_sink.cpp" "${VITA}/port_lifecycle.cpp" "${VITA}/port_player.cpp" "${VITA}/port_input.cpp" "${VITA}/port_microphone.cpp" "${VITA}/port_addons.cpp" "${VITA}/port_downloads.cpp" "${VITA}/port_native_download.cpp" "${VITA}/port_native_mp3_stream.cpp" "${VITA}/port_native_mp4_stream.cpp" "${VITA}/port_mp4_aac.cpp" "${VITA}/port_download_metadata.cpp" "${VITA}/port_cover_4x3.cpp" "${VITA}/port_media_session.cpp" "${VITA}/port_osk.cpp" "${VITA}/port_ui_sound.cpp")
  if(YTV_V8_JITLESS_DIAGNOSTIC)
    target_compile_definitions(starboard_vita15 PRIVATE YTV_V8_JITLESS_DIAGNOSTIC=1)
  endif()
  option(YTV_V061_RX_CHUNKS "v0.61 diagnostic: chunk Cobalt executable mapping" OFF)
  option(YTV_V061_THREE_CORES "v0.61 diagnostic: force 3 user CPU cores" OFF)
  if(YTV_V061_THREE_CORES)
    target_compile_definitions(starboard_vita15 PRIVATE YTV_V061_THREE_CORES=1)
  endif()
  add_executable(ytv_sb15_probe "${VITA}/main_test.cpp")
  option(YTV_GL_TRACE "Diagnostic: log GL calls made through the Starboard GLES table" OFF)
  if(YTV_GL_TRACE)
    target_compile_definitions(starboard_vita15 PRIVATE YTV_GL_TRACE=1)
  endif()
  # Experimental, audited libcurl 8.21.0 + Mbed TLS transport for native downloads.
  # Unlike legacy VitaSDK curl/OpenSSL, supports modern verified TLS.
  option(YTV_NATIVE_HARDENED_CURL "Use pinned Mbed TLS curl instead of VitaSDK OpenSSL" OFF)
  if(YTV_NATIVE_HARDENED_CURL)
    set(YTV_HARDENED_CURL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitube/external/vita-https/build/deps/curl-mbedtls")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitube/external/vita-https/build/deps/curl-mbedtls/lib/libcurl.a")
      message(FATAL_ERROR "Run vita-https/tools/build-curl-mbedtls.sh first")
    endif()
    set(YTV_CURL_LIB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitube/external/vita-https/build/deps/curl-mbedtls/lib/libcurl.a")
    set(YTV_CURL_TLS_LIBS mbedtls mbedx509 mbedcrypto)
    target_include_directories(starboard_vita15 BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitube/external/vita-https/build/deps/curl-mbedtls/include")
    message(STATUS "Native downloads using hardened curl/Mbed TLS")
  else()
    set(YTV_CURL_LIB curl)
    set(YTV_CURL_TLS_LIBS ssl crypto)
  endif()
  # Hardware video/audio decoders (SbPlayer) and microphone input.
  if(YTV_NATIVE_DOWNLOAD_TEST)
    target_compile_definitions(starboard_vita15 PRIVATE YTV_NATIVE_DOWNLOAD_TEST=1)
  endif()
  target_link_libraries(starboard_vita15 PUBLIC SceVideodec_stub SceAudiodec_stub SceAudioIn_stub SceRtc_stub
    ${YTV_CURL_LIB} ${YTV_CURL_TLS_LIBS} sodium vitaGL vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceGxm_stub SceDisplay_stub SceAppUtil_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub
    ScePower_stub SceCtrl_stub SceTouch_stub SceAudio_stub SceCommonDialog_stub
    SceAppMgr_stub SceKernelDmacMgr_stub zstd mathneon pthread z m)
  foreach(tgt starboard_vita15 ytv_sb15_probe)
    target_include_directories(${tgt} PRIVATE "${VITA}/include" "${SB}")
    target_compile_options(${tgt} PRIVATE -O2 -Wall -Wextra -Wno-expansion-to-defined -std=gnu++17
      -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
      "-include" "${VITA}/include/port_config.h")
  endforeach()
  target_link_libraries(ytv_sb15_probe PRIVATE starboard_vita15
      SceIofilemgr_stub SceLibKernel_stub SceKernelThreadMgr_stub
      SceNet_stub SceNetCtl_stub SceAudio_stub SceSysmodule_stub SceAppUtil_stub SceSysmem_stub kubridge_stub
      vitaGL vitashark SceShaccCgExt SceShaccCg_stub taihen_stub SceDisplay_stub SceGxm_stub
      SceKernelDmacMgr_stub ScePower_stub SceAppMgr_stub SceCtrl_stub SceTouch_stub
      SceCommonDialog_stub mathneon z pthread m)
  vita_create_self(ytv_sb15_probe.self ytv_sb15_probe UNSAFE)
  vita_create_vpk(StarboardVitaTest.vpk ${VITA_TITLEID} ytv_sb15_probe.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
    FILE experiments/cobalt/vita/README.md README.md
    FILE experiments/cobalt/vita/assets/player_test_854x480.h264 player_test_854x480.h264)
endif()


# Optional readonly inspection of the official Google Cobalt 25 Evergreen ELF.
# This is not a loader and does not run libcobalt.so.
option(YTV_BUILD_EVERGREEN_INSPECTOR "Build standalone ELF inspection Vita3K VPK" ON)
if(YTV_BUILD_EVERGREEN_INSPECTOR)
  set(COBALT_CORE "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/evergreen/core25/lib/libcobalt.so")
  if(NOT EXISTS "${COBALT_CORE}")
    message(FATAL_ERROR "Official Cobalt ELF missing: ${COBALT_CORE}")
  endif()
  set(VITA_APP_NAME "Cobalt ELF Inspector")
  set(VITA_TITLEID "YTVEL0001")
  set(VITA_VERSION "00.03")
  add_executable(ytv_elf_inspector experiments/cobalt/vita/evergreen_inspect.c)
  target_compile_options(ytv_elf_inspector PRIVATE -O2 -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  target_link_libraries(ytv_elf_inspector PRIVATE SceIofilemgr_stub SceLibKernel_stub SceSysmem_stub)
  vita_create_self(ytv_elf_inspector.self ytv_elf_inspector UNSAFE)
  vita_create_vpk(CobaltEvergreenInspect.vpk ${VITA_TITLEID} ytv_elf_inspector.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${COBALT_CORE} libcobalt.so
    FILE experiments/cobalt/evergreen/core25/manifest.json cobalt-manifest.json)
endif()

# Stage 3: evaluate ARM relocation machinery in non-executable Vita RW memory.
option(YTV_BUILD_EVERGREEN_RELOCATOR "Build official ELF RAM-only relocation validation" ON)
if(YTV_BUILD_EVERGREEN_RELOCATOR)
  set(COBALT_CORE "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/evergreen/core25/lib/libcobalt.so")
  if(NOT EXISTS "${COBALT_CORE}")
    message(FATAL_ERROR "Official Cobalt ELF missing: ${COBALT_CORE}")
  endif()
  set(VITA_APP_NAME "Cobalt Relocation Test")
  set(VITA_TITLEID "YTVRL0001")
  set(VITA_VERSION "00.04")
  add_executable(ytv_relocator experiments/cobalt/vita/relocate_elf.c)
  target_compile_options(ytv_relocator PRIVATE -O2 -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums)
  target_link_libraries(ytv_relocator PRIVATE starboard_vita15 kubridge_stub
     vitaGL vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
     SceDisplay_stub SceGxm_stub SceAudio_stub SceKernelDmacMgr_stub
     ScePower_stub SceAppMgr_stub SceCtrl_stub SceTouch_stub
     SceCommonDialog_stub SceIofilemgr_stub SceLibKernel_stub
     SceSysmem_stub SceNet_stub SceSysmodule_stub SceAppUtil_stub
     SceKernelThreadMgr_stub mathneon pthread z m)
  vita_create_self(ytv_relocator.self ytv_relocator UNSAFE)
  vita_create_vpk(CobaltEvergreenRelocations.vpk ${VITA_TITLEID} ytv_relocator.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${COBALT_CORE} libcobalt.so
    FILE experiments/cobalt/evergreen/core25/manifest.json cobalt-manifest.json)
endif()
# Stage 4: kubridge RX executable memory smoke test, no Cobalt code invoked.
option(YTV_BUILD_EXEC_MEMORY_PROBE "Probe RX W^X permissions via kubridge in Vita3K" ON)
if(YTV_BUILD_EXEC_MEMORY_PROBE)
  set(VITA_APP_NAME "Vita RX Memory Probe")
  set(VITA_TITLEID "YTVMX0001")
  set(VITA_VERSION "00.05")
  add_executable(ytv_exec_mem_probe experiments/cobalt/vita/executable_memory.c)
  target_compile_options(ytv_exec_mem_probe PRIVATE -O2 -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  target_link_libraries(ytv_exec_mem_probe PRIVATE kubridge_stub SceSysmem_stub SceIofilemgr_stub SceLibKernel_stub)
  vita_create_self(ytv_exec_mem_probe.self ytv_exec_mem_probe UNSAFE)
  vita_create_vpk(CobaltExecutableMemoryProbe.vpk ${VITA_TITLEID} ytv_exec_mem_probe.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 5: real SceNet init/socket test; intentionally no network request.
option(YTV_BUILD_OFFLINE_NET_PROBE "Build Vita native networking offline probe" ON)
if(YTV_BUILD_OFFLINE_NET_PROBE)
  set(VITA_APP_NAME "Starboard Vita Network Test")
  set(VITA_TITLEID "YTVNT0001")
  set(VITA_VERSION "00.06")
  add_executable(ytv_net_probe experiments/cobalt/vita/network_probe.c)
  target_compile_options(ytv_net_probe PRIVATE -O2 -Wall -Wextra -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  target_link_libraries(ytv_net_probe PRIVATE SceNet_stub SceSysmodule_stub
   SceIofilemgr_stub SceLibKernel_stub)
  vita_create_self(ytv_net_probe.self ytv_net_probe UNSAFE)
  vita_create_vpk(StarboardVitaNetworkTest.vpk ${VITA_TITLEID} ytv_net_probe.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 6: real nonblocking SceNet / Starboard socket integration test.
option(YTV_BUILD_SOCKET_INTEGRATION_TEST "Build Starboard sockets end-to-end proof" ON)
if(YTV_BUILD_SOCKET_INTEGRATION_TEST)
  set(VITA_APP_NAME "Starboard Vita Socket Test")
  set(VITA_TITLEID "YTVSK0001")
  set(VITA_VERSION "00.07")
  add_executable(ytv_socket_test experiments/cobalt/vita/socket_test.cpp)
  target_include_directories(ytv_socket_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/upstream-25")
  target_compile_options(ytv_socket_test PRIVATE -O2 -Wall -Wextra -std=gnu++17
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include/port_config.h")
  target_link_libraries(ytv_socket_test PRIVATE starboard_vita15 SceNet_stub SceSysmodule_stub
    SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_socket_test.self ytv_socket_test UNSAFE)
  vita_create_vpk(StarboardVitaSocketTest.vpk ${VITA_TITLEID} ytv_socket_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 7: actual TCP stream loopback proof.
option(YTV_BUILD_TCP_INTEGRATION_TEST "Build PS Vita Starboard TCP proof" ON)
if(YTV_BUILD_TCP_INTEGRATION_TEST)
  set(VITA_APP_NAME "Starboard Vita TCP Test")
  set(VITA_TITLEID "YTVTC0001")
  set(VITA_VERSION "00.08")
  add_executable(ytv_tcp_test experiments/cobalt/vita/socket_tcp_test.cpp)
  target_include_directories(ytv_tcp_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/upstream-25")
  target_compile_options(ytv_tcp_test PRIVATE -O2 -Wall -Wextra -Wno-expansion-to-defined -std=gnu++17
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include/port_config.h")
  target_link_libraries(ytv_tcp_test PRIVATE starboard_vita15 SceNet_stub SceSysmodule_stub
    SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_tcp_test.self ytv_tcp_test UNSAFE)
  vita_create_vpk(StarboardVitaTcpTest.vpk ${VITA_TITLEID} ytv_tcp_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
option(YTV_BUILD_EXTERNAL_DNS_TEST "Build Vita3K external DNS test" ON)
if(YTV_BUILD_EXTERNAL_DNS_TEST)
  set(VITA_APP_NAME "Starboard Vita DNS Test")
  set(VITA_TITLEID "YTVDN0001")
  set(VITA_VERSION "00.09")
  add_executable(ytv_dns_test experiments/cobalt/vita/socket_dns_test.cpp)
  target_include_directories(ytv_dns_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/upstream-25")
  target_compile_options(ytv_dns_test PRIVATE -O2 -Wall -Wextra -Wno-expansion-to-defined -std=gnu++17
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include/port_config.h")
  target_link_libraries(ytv_dns_test PRIVATE starboard_vita15 SceNet_stub SceSysmodule_stub
    SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_dns_test.self ytv_dns_test UNSAFE)
  vita_create_vpk(StarboardVitaDnsTest.vpk ${VITA_TITLEID} ytv_dns_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
option(YTV_BUILD_EPOLL_TEST "Build native Vita epoll socket event test" ON)
if(YTV_BUILD_EPOLL_TEST)
  set(VITA_APP_NAME "Starboard Vita Epoll Test")
  set(VITA_TITLEID "YTVWT0001")
  set(VITA_VERSION "00.10")
  add_executable(ytv_epoll_test experiments/cobalt/vita/socket_waiter_test.cpp)
  target_include_directories(ytv_epoll_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/upstream-25")
  target_compile_options(ytv_epoll_test PRIVATE -O2 -Wall -Wextra -Wno-expansion-to-defined -std=gnu++17
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
    "-include" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include/port_config.h")
  target_link_libraries(ytv_epoll_test PRIVATE starboard_vita15 SceNet_stub SceSysmodule_stub
    SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_epoll_test.self ytv_epoll_test UNSAFE)
  vita_create_vpk(StarboardVitaEpollTest.vpk ${VITA_TITLEID} ytv_epoll_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 8: genuine hardware-accelerated vitaGL EGL/GXM frame probe.
option(YTV_BUILD_GRAPHICS_PROBE "Build Vita GXM EGL hardware smoke test" ON)
if(YTV_BUILD_GRAPHICS_PROBE)
  set(VITA_APP_NAME "YouTube Vita Graphics Test")
  set(VITA_TITLEID "YTVGX0001")
  set(VITA_VERSION "00.11")
  # Links the same Starboard platform library the Cobalt bootstrap uses, so
  # the EGL/GLES bridge under test is exactly the one Cobalt calls.
  add_executable(ytv_graphics_test experiments/cobalt/vita/graphics_test.cpp)
  target_include_directories(ytv_graphics_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/upstream-25")
  target_compile_options(ytv_graphics_test PRIVATE -O2 -Wall -Wextra -std=gnu++17
       -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums
       "-include" "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/include/port_config.h")
  target_link_libraries(ytv_graphics_test PRIVATE starboard_vita15 kubridge_stub
    SceNet_stub SceNetCtl_stub SceAppUtil_stub
    vitaGL vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceSysmodule_stub SceDisplay_stub SceGxm_stub SceAudio_stub
    SceKernelDmacMgr_stub ScePower_stub SceSysmem_stub SceAppMgr_stub
    SceCtrl_stub SceTouch_stub SceCommonDialog_stub SceIofilemgr_stub
    SceKernelThreadMgr_stub SceLibKernel_stub mathneon pthread z m)
  vita_create_self(ytv_graphics_test.self ytv_graphics_test UNSAFE)
  vita_create_vpk(YouTubeVitaGraphicsTest.vpk ${VITA_TITLEID} ytv_graphics_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 9: public HTTPS/TLS endpoint probe; credentials are never used.
option(YTV_BUILD_HTTPS_PROBE "Build public SceHttp HTTPS capability probe" ON)
if(YTV_BUILD_HTTPS_PROBE)
  set(VITA_APP_NAME "YouTube Vita HTTPS Probe")
  set(VITA_TITLEID "YTVHT0001")
  set(VITA_VERSION "00.12")
  add_executable(ytv_https_probe experiments/cobalt/vita/https_probe.cpp)
  target_compile_options(ytv_https_probe PRIVATE -O2 -Wall -Wextra -std=gnu++17
      -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard)
  target_link_libraries(ytv_https_probe PRIVATE SceNet_stub SceHttp_stub SceSsl_stub
      SceSysmodule_stub SceIofilemgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_https_probe.self ytv_https_probe UNSAFE)
  vita_create_vpk(YouTubeVitaHttpsProbe.vpk ${VITA_TITLEID} ytv_https_probe.self
     VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
     FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 12: genuine HTTPS certificate-validation test via VitaSDK libcurl.
option(YTV_BUILD_TLS_TEST "Build real external HTTPS/TLS diagnostic" ON)
if(YTV_BUILD_TLS_TEST)
  set(VITA_APP_NAME "YouTube Vita TLS Test")
  set(VITA_TITLEID "YTVTL0001")
  set(VITA_VERSION "00.12")
  add_executable(ytv_tls_test experiments/cobalt/vita/tls_test.c)
  target_compile_options(ytv_tls_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(ytv_tls_test PRIVATE curl ssl crypto zstd z
    SceNet_stub SceSysmodule_stub SceIofilemgr_stub
    SceKernelThreadMgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_tls_test.self ytv_tls_test UNSAFE)
  vita_create_vpk(YouTubeVitaTLSTest.vpk ${VITA_TITLEID} ytv_tls_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
    FILE experiments/cobalt/vita/ca-bundle.pem cacert.pem)
endif()
# Stage 13: real SceVideodec H.264 hardware decoder memory query.
option(YTV_BUILD_H264_PROBE "Build native Vita H264 decoder query" ON)
if(YTV_BUILD_H264_PROBE)
  set(VITA_APP_NAME "YouTube Vita H264 Test")
  set(VITA_TITLEID "YTVVD0001")
  set(VITA_VERSION "00.13")
  add_executable(ytv_h264_probe experiments/cobalt/vita/video_decoder_probe.c)
  target_compile_options(ytv_h264_probe PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(ytv_h264_probe PRIVATE
      SceVideodec_stub SceSysmodule_stub SceIofilemgr_stub SceLibKernel_stub)
  vita_create_self(ytv_h264_probe.self ytv_h264_probe UNSAFE)
  vita_create_vpk(YouTubeVitaH264Probe.vpk ${VITA_TITLEID} ytv_h264_probe.self
     VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
     FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png)
endif()
# Stage 14: H.264 decoder creation using physical contiguous Vita buffer.
option(YTV_BUILD_H264_FRAME_TEST "Build native H264 decoder creation and synthetic frame test" ON)
if(YTV_BUILD_H264_FRAME_TEST)
  set(VITA_APP_NAME "YouTube Vita H264 Frame Test")
  set(VITA_TITLEID "YTVDF0001")
  set(VITA_VERSION "00.14")
  add_executable(ytv_h264_frame_test experiments/cobalt/vita/decode_frame_probe.c)
  target_compile_options(ytv_h264_frame_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(ytv_h264_frame_test PRIVATE
    SceVideodec_stub SceSysmodule_stub SceSysmem_stub
    SceIofilemgr_stub SceLibKernel_stub)
  vita_create_self(ytv_h264_frame_test.self ytv_h264_frame_test UNSAFE)
  vita_create_vpk(YouTubeVitaH264FrameTest.vpk ${VITA_TITLEID} ytv_h264_frame_test.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
    FILE experiments/cobalt/vita/assets/h264_test.h264 h264_test.h264)
endif()
# Stage 15: diagnostic software FFmpeg H.264 decoder for independent pixel verification.
option(YTV_BUILD_SW_H264_PROBE "Build Vita libavcodec software H264 pixel validation" OFF)
if(YTV_BUILD_SW_H264_PROBE)
  set(VITA_APP_NAME "YouTube Vita SW H264 Test")
  set(VITA_TITLEID "YTVSW0001")
  set(VITA_VERSION "00.15")
  add_executable(ytv_software_decoder experiments/cobalt/vita/software_decoder_probe.c)
  target_compile_options(ytv_software_decoder PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(ytv_software_decoder PRIVATE
    avcodec avutil swscale swresample mp3lame z bz2 lzma
    SceIofilemgr_stub SceKernelThreadMgr_stub SceLibKernel_stub pthread m)
  vita_create_self(ytv_software_decoder.self ytv_software_decoder UNSAFE)
  vita_create_vpk(YouTubeVitaSoftwareH264Test.vpk ${VITA_TITLEID} ytv_software_decoder.self
     VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
     FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
     FILE experiments/cobalt/vita/assets/h264_test.h264 h264_test.h264)
endif()
# Stage 16: native SceAvPlayer MP4 decoder with original synthetic video.
option(YTV_BUILD_AVPLAYER_PROBE "Build Vita native synthetic H264 MP4 playback probe" ON)
if(YTV_BUILD_AVPLAYER_PROBE)
  set(VITA_APP_NAME "YouTube Vita AV Player Test")
  set(VITA_TITLEID "YTVAP0001")
  set(VITA_VERSION "00.16")
  add_executable(ytv_avplayer_probe experiments/cobalt/vita/avplayer_probe.c)
  target_compile_options(ytv_avplayer_probe PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(ytv_avplayer_probe PRIVATE
     SceAvPlayer_stub SceGxm_stub SceSysmodule_stub SceSysmem_stub
     SceKernelThreadMgr_stub SceIofilemgr_stub SceLibKernel_stub m)
  vita_create_self(ytv_avplayer_probe.self ytv_avplayer_probe UNSAFE)
  vita_create_vpk(YouTubeVitaAVPlayerTest.vpk ${VITA_TITLEID} ytv_avplayer_probe.self
      VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
      FILE legacy/native-mock-v001/sce_sys/icon0.png sce_sys/icon0.png
      FILE experiments/cobalt/vita/assets/vita_avplayer_test.mp4 vita_avplayer_test.mp4)
endif()

# Distribution packages: normal extended-RAM YouTube and a Lite alternative.
# Both share the same source and version; their title IDs keep them installable
# side by side. Prefer the normal package unless background apps are important.
option(YTV_PACKAGE_LITE "Build normal-memory YouTube Lite (YTVCB0002)" OFF)
option(YTV_BUILD_COBALT_BOOT_PROBE "Build native Cobalt YouTube" ON)
set(YTV_CUSTOM_COBALT_DIR "" CACHE PATH "Optional validated experimental Cobalt runtime directory")
if(YTV_BUILD_COBALT_BOOT_PROBE)
  if(YTV_PACKAGE_LITE)
    set(VITA_APP_NAME "YouTube Lite")
    set(VITA_TITLEID "YTVCB0002")
    set(YTV_DISTRIBUTION_VPK "YouTube-Lite.vpk")
  else()
    set(VITA_APP_NAME "YouTube")
    set(VITA_TITLEID "YTVCB0001")
    set(YTV_DISTRIBUTION_VPK "YouTube.vpk")
  endif()
  if(YTV_CUSTOM_COBALT_DIR)
    if(NOT EXISTS "${YTV_CUSTOM_COBALT_DIR}/libcobalt.so" OR
       NOT EXISTS "${YTV_CUSTOM_COBALT_DIR}/cobalt_candidate_profile.h" OR
       NOT IS_DIRECTORY "${YTV_CUSTOM_COBALT_DIR}/content")
      message(FATAL_ERROR "Incomplete candidate Cobalt profile/runtime/content")
    endif()
    set(COBALT_CORE "${YTV_CUSTOM_COBALT_DIR}/libcobalt.so")
    if(YTV_PACKAGE_LITE)
      set(VITA_APP_NAME "YouTube Lite")
      set(VITA_TITLEID "YTVNX0002")
      set(YTV_DISTRIBUTION_VPK "YouTube-Lite.vpk")
    else()
      set(VITA_APP_NAME "YouTube")
      set(VITA_TITLEID "YTVNX0001")
      set(YTV_DISTRIBUTION_VPK "YouTube-Full.vpk")
    endif()
  endif()
  # PS Vita param.sfo APP_VER uses NN.NN: 00.03 corresponds to fork v0.0.3.
  set(YTV_COBALT_APP_VERSION "00.03" CACHE STRING "YouTube on Vita fork version (v0.0.3)")
  set(VITA_VERSION "${YTV_COBALT_APP_VERSION}")
  add_executable(ytv_cobalt_boot experiments/cobalt/vita/relocate_elf.c
    experiments/cobalt/vita/port_startup_splash.c)
  target_compile_definitions(ytv_cobalt_boot PRIVATE
    YTV_COBALT_BOOT_STAGE=2)
  if(YTV_CUSTOM_COBALT_DIR)
    target_compile_definitions(ytv_cobalt_boot PRIVATE YTV_CUSTOM_COBALT=1)
    target_include_directories(ytv_cobalt_boot PRIVATE "${YTV_CUSTOM_COBALT_DIR}")
  else()
    target_compile_definitions(ytv_cobalt_boot PRIVATE YTV_COBALT_DIAG_EVENT_CUTOFF=1)
  endif()
  if(YTV_V061_RX_CHUNKS)
    target_compile_definitions(ytv_cobalt_boot PRIVATE YTV_V061_RX_CHUNKS=1)
  endif()
  if(YTV_PACKAGE_LITE)
    target_compile_definitions(ytv_cobalt_boot PRIVATE YTV_LOWMEM=1)
  endif()
  target_compile_options(ytv_cobalt_boot PRIVATE -O2 -Wall -Wextra
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -fno-short-enums)
  target_link_libraries(ytv_cobalt_boot PRIVATE starboard_vita15 kubridge_stub jpeg png webpdecoder avformat avcodec swresample avutil mp3lame mpg123 ${YTV_CURL_LIB} ${YTV_CURL_TLS_LIBS} zstd
    vitaGL vitashark SceShaccCgExt SceShaccCg_stub taihen_stub
    SceDisplay_stub SceGxm_stub SceAudio_stub SceKernelDmacMgr_stub
    ScePower_stub SceAppMgr_stub SceCtrl_stub SceTouch_stub
    SceCommonDialog_stub SceIofilemgr_stub SceLibKernel_stub
    SceSysmem_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub SceAppUtil_stub
    SceKernelThreadMgr_stub mathneon pthread z m)
  # vita-elf-create appends SCE data (~4.4 KiB) after the R-X segment; with
  # 4 KiB pages the RW segment can start too close ("segment 1 overlaps").
  # 64 KiB alignment always leaves room.
  target_link_options(ytv_cobalt_boot PRIVATE -Wl,-z,max-page-size=0x10000)
  vita_create_self(ytv_cobalt_boot.self ytv_cobalt_boot UNSAFE)
  # Package only content actually extracted from the matching official CRX.
  set(COBALT_EXTRACTED_CONTENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/evergreen/core25/content")
  if(YTV_CUSTOM_COBALT_DIR)
    set(COBALT_EXTRACTED_CONTENT_DIR "${YTV_CUSTOM_COBALT_DIR}/content")
  endif()
  file(GLOB_RECURSE COBALT_BOOT_ASSETS CONFIGURE_DEPENDS
       "${COBALT_EXTRACTED_CONTENT_DIR}/*")
  set(COBALT_BOOT_ASSET_FILES "")
  foreach(COBALT_BOOT_ASSET IN LISTS COBALT_BOOT_ASSETS)
    file(RELATIVE_PATH COBALT_BOOT_ASSET_DEST
      "${COBALT_EXTRACTED_CONTENT_DIR}" "${COBALT_BOOT_ASSET}")
    list(APPEND COBALT_BOOT_ASSET_FILES
      FILE "${COBALT_BOOT_ASSET}" "${COBALT_BOOT_ASSET_DEST}")
  endforeach()
  # Platform system fonts: Cobalt's official 'limited' font package, staged by
  # tools/stage_vita_system_fonts.py (Evergreen's own fonts.xml is empty).
  set(VITA_SYSTEM_FONTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/experiments/cobalt/vita/system_fonts")
  file(GLOB VITA_SYSTEM_FONT_FILES CONFIGURE_DEPENDS "${VITA_SYSTEM_FONTS_DIR}/*")
  # Pre-compiled VitaGL shader cache (GXP from SceShaccCg), seeded on first run.
  file(GLOB VITA_SHADER_SEED CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/shader_cache_seed/*")
  foreach(SHADER_FILE IN LISTS VITA_SHADER_SEED)
    get_filename_component(SHADER_NAME "${SHADER_FILE}" NAME)
    list(APPEND COBALT_BOOT_ASSET_FILES FILE "${SHADER_FILE}" "shader_cache/${SHADER_NAME}")
  endforeach()
  foreach(FONT_FILE IN LISTS VITA_SYSTEM_FONT_FILES)
    get_filename_component(FONT_NAME "${FONT_FILE}" NAME)
    list(APPEND COBALT_BOOT_ASSET_FILES FILE "${FONT_FILE}" "system_fonts/${FONT_NAME}")
  endforeach()
  # Normal YouTube reserves extended user RAM. Lite uses ordinary mode
  # for better coexistence with suspended applications/system Settings.
  # vita_create_vpk is a macro: VITA_PACK_VPK_FLAGS / resources from the
  # earlier test VPKs in this file leaked into this package (their legacy
  # icon0.png came first and won over the LiveArea icon). Start clean.
  set(VITA_PACK_VPK_FLAGS "")
  set(resources "")
  if(YTV_PACKAGE_LITE)
    unset(VITA_MKSFOEX_FLAGS)
  else()
    set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
  endif()
  # Match Mozilla CA roots to the modern verified-MbedTLS download transport.
  set(YTV_COBALT_CA_BUNDLE "experiments/cobalt/vita/ca-bundle.pem")
  if(YTV_NATIVE_HARDENED_CURL)
    set(YTV_COBALT_CA_BUNDLE "third_party/vitube/external/vita-https/sources/certifi-2026.07.22-cacert.pem")
  endif()
  vita_create_vpk(${YTV_DISTRIBUTION_VPK} ${VITA_TITLEID} ytv_cobalt_boot.self
    VERSION ${VITA_VERSION} NAME ${VITA_APP_NAME}
    FILE assets/livearea/icon0.png sce_sys/icon0.png
    FILE assets/livearea/template.xml sce_sys/livearea/contents/template.xml
    FILE assets/livearea/bg0.png sce_sys/livearea/contents/bg0.png
    FILE assets/livearea/startup.png sce_sys/livearea/contents/startup.png
    FILE experiments/cobalt/vita/assets/splash/youtube_intro_60fps.yspl splash/youtube_intro_60fps.yspl
    FILE experiments/cobalt/vita/assets/splash/youtube_intro_audio.pcm splash/youtube_intro_audio.pcm
    FILE experiments/cobalt/vita/assets/sounds/focus.pcm sfx/ui/focus.pcm
    FILE experiments/cobalt/vita/assets/sounds/confirm.pcm sfx/ui/confirm.pcm
    FILE experiments/cobalt/vita/assets/sounds/back.pcm sfx/ui/back.pcm
    FILE experiments/cobalt/vita/assets/sounds/success.pcm sfx/ui/success.pcm
    FILE experiments/cobalt/vita/assets/sounds/error.pcm sfx/ui/error.pcm
    FILE experiments/cobalt/vita/assets/sounds/open.pcm sfx/ui/open.pcm
    FILE experiments/cobalt/vita/assets/sounds/mic_on.pcm sfx/ui/mic_on.pcm
    FILE experiments/cobalt/vita/assets/sounds/touch.pcm sfx/ui/touch.pcm
    FILE experiments/cobalt/vita/assets/sounds/download_start.pcm sfx/ui/download_start.pcm
    FILE experiments/cobalt/vita/assets/sounds/download_done.pcm sfx/ui/download_done.pcm
    FILE experiments/cobalt/vita/assets/sounds/controls_lock.pcm sfx/ui/controls_lock.pcm
    FILE experiments/cobalt/vita/assets/sounds/sleep_warning.pcm sfx/ui/sleep_warning.pcm
    FILE experiments/cobalt/vita/assets/sounds/timer_enable.pcm sfx/ui/timer_enable.pcm
    FILE experiments/cobalt/vita/assets/sounds/lock.pcm sfx/ui/lock.pcm
    FILE experiments/cobalt/vita/assets/sounds/delete.pcm sfx/ui/delete.pcm
    FILE experiments/cobalt/vita/assets/sounds/battery_low.pcm sfx/ui/battery_low.pcm
    FILE ${COBALT_CORE} libcobalt.so
    FILE ${YTV_COBALT_CA_BUNDLE} cacert.pem
    ${COBALT_BOOT_ASSET_FILES})
  unset(VITA_MKSFOEX_FLAGS)

endif()
