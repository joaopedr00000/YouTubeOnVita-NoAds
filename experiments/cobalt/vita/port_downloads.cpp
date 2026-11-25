/*
 * YouTubeVita: optional, native cobalt.tools media download.
 * Not Google Cobalt (the browser engine). No JavaScript or YouTube UI patch.
 *
 * Configuration: ux0:data/youtubevita-noads/download.cfg
 *   api=https://YOUR-AUTHORIZED-COBALT-API/
 *   key=OPTIONAL_API_KEY
 *   quality=480   (360/480/720)
 *   mode=video    (video/audio)
 * Media URL: ux0:data/youtubevita-noads/download_request.txt (one YouTube watch URL).
 * Access only media you own or are authorized to save.
 */
#include <curl/curl.h>
#include "starboard/log.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cctype>

namespace {
constexpr const char* kRoot = "ux0:data/youtubevita-noads";
constexpr const char* kDir = "ux0:data/youtubevita-noads/downloads";
constexpr const char* kConfig = "ux0:data/youtubevita-noads/download.cfg";
constexpr const char* kRequest = "ux0:data/youtubevita-noads/download_request.txt";
constexpr const char* kCaFile = "app0:cacert.pem";
pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
int s_running = 0;
volatile int s_cancel = 0;
char s_status[48] = "Ready";
int s_percent = -1;
struct Settings {
  char api[256], key[256], quality[8], mode[12], video_id[16], url[384];
};
void update(const char* state, int percent = -1) {
  // State-only log (not per-byte progress); helps diagnose hardware errors.
  char line[112];
  snprintf(line, sizeof(line), "YTV_DOWNLOAD_STATE %s percent=%d\n", state, percent);
  SbLogRaw(line);
  pthread_mutex_lock(&s_lock);
  snprintf(s_status, sizeof(s_status), "%s", state);
  s_percent = percent;
  pthread_mutex_unlock(&s_lock);
}
bool read_first_line(const char* path, char* dest, size_t cap) {
  if (!cap) return false;
  dest[0] = 0;
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return false;
  int n = sceIoRead(fd, dest, (unsigned)(cap - 1));
  sceIoClose(fd);
  if (n <= 0) return false;
  dest[n] = 0;
  char* end = strpbrk(dest, "\r\n");
  if (end) *end = 0;
  return dest[0] != 0;
}
bool config(Settings& cfg) {
  snprintf(cfg.quality, sizeof(cfg.quality), "480");
  snprintf(cfg.mode, sizeof(cfg.mode), "video");
  char data[1024] = {};
  SceUID fd = sceIoOpen(kConfig, SCE_O_RDONLY, 0);
  if (fd < 0) return false;
  int n = sceIoRead(fd, data, sizeof(data) - 1);
  sceIoClose(fd);
  if (n <= 0) return false;
  data[n] = 0;
  char* cursor = data;
  while (*cursor) {
    char* end = strpbrk(cursor, "\r\n");
    if (end) *end = 0;
    char* eq = strchr(cursor, '=');
    if (eq) {
      *eq++ = 0;
      if (!strcmp(cursor, "api")) snprintf(cfg.api, sizeof(cfg.api), "%s", eq);
      else if (!strcmp(cursor, "key")) snprintf(cfg.key, sizeof(cfg.key), "%s", eq);
      else if (!strcmp(cursor, "quality")) snprintf(cfg.quality, sizeof(cfg.quality), "%s", eq);
      else if (!strcmp(cursor, "mode")) snprintf(cfg.mode, sizeof(cfg.mode), "%s", eq);
    }
    if (!end) break;
    cursor = end + 1;
    while (*cursor == '\n' || *cursor == '\r') ++cursor;
  }
  // Native Settings panel overrides only presentation preferences.
  // Keep API host/key private in download.cfg and never rewrite them.
  // A missing native setting means the API config's own value still applies.
  char native[256] = {};
  SceUID nfd = sceIoOpen("ux0:data/youtubevita-noads/addons.cfg", SCE_O_RDONLY, 0);
  if (nfd >= 0) {
    int got = sceIoRead(nfd, native, sizeof(native) - 1);
    sceIoClose(nfd);
    if (got > 0) {
      native[got] = 0;
      const char* q = strstr(native, "quality=");
      if (q && (q == native || q[-1] == '\n')) {
        q += 8;
        if (!strncmp(q, "360", 3) || !strncmp(q, "480", 3) || !strncmp(q, "720", 3))
          snprintf(cfg.quality, sizeof(cfg.quality), "%.3s", q);
      }
      q = strstr(native, "audio_only=");
      if (q && (q == native || q[-1] == '\n')) {
        q += 11;
        if (*q == '1') snprintf(cfg.mode, sizeof(cfg.mode), "audio");
        if (*q == '0') snprintf(cfg.mode, sizeof(cfg.mode), "video");
      }
    }
  }
  // HTTPS required; deny URL credentials, fragments, queries and control chars.
  if (strncmp(cfg.api, "https://", 8) != 0 || !cfg.api[8] ||
      strpbrk(cfg.api, "@?# \t\r\n") != nullptr) return false;
  if (strcmp(cfg.quality, "360") && strcmp(cfg.quality, "480") && strcmp(cfg.quality, "720"))
    return false;
  if (strcmp(cfg.mode, "video") && strcmp(cfg.mode, "audio")) return false;
  for (const char* p = cfg.key; *p; ++p)
    if (*p == '\r' || *p == '\n') return false;
  return true;
}
bool parse_id(Settings& cfg) {
  if (!read_first_line(kRequest, cfg.url, sizeof(cfg.url))) return false;
  const char* id = nullptr;
  if (!strncmp(cfg.url, "https://youtu.be/", 17)) id = cfg.url + 17;
  else if (!strncmp(cfg.url, "https://www.youtube.com/watch?", 30) ||
           !strncmp(cfg.url, "https://youtube.com/watch?", 26) ||
           !strncmp(cfg.url, "https://m.youtube.com/watch?", 28)) {
    const char* q = strchr(cfg.url, '?');
    if (!q) return false;
    const char* v = strstr(q + 1, "v=");
    if (v && (v == q + 1 || v[-1] == '&')) id = v + 2;
  }
  if (!id) return false;
  for (int i = 0; i < 11; ++i) {
    const unsigned char ch = (unsigned char)id[i];
    if (!ch || !(isalnum(ch) || ch == '_' || ch == '-')) return false;
    cfg.video_id[i] = ch;
  }
  cfg.video_id[11] = 0;
  const char tail = id[11];
  if (tail && tail != '&' && tail != '?' && tail != '#' && tail != '/') return false;
  snprintf(cfg.url, sizeof(cfg.url), "https://www.youtube.com/watch?v=%s", cfg.video_id);
  return true;
}
// Only decode JSON string values; rejects unsupported escapes and control chars.
bool json_value(const char* json, const char* key, char* out, size_t cap) {
  if (!cap) return false;
  char needle[48] = {};
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(json, needle);
  if (!p) return false;
  p = strchr(p + strlen(needle), ':');
  if (!p) return false;
  ++p;
  while (*p == ' ' || *p == '\t' || *p == '\n') ++p;
  if (*p++ != '"') return false;
  size_t n = 0;
  while (*p && *p != '"') {
    unsigned char c = (unsigned char)*p++;
    if (c == '\\') {
      c = (unsigned char)*p++;
      if (c == '/' || c == '"' || c == '\\') {}
      else if (c == 'n' || c == 'r' || c == 't') return false;
      else return false;
    }
    if (c < 32 || n + 1 >= cap) return false;
    out[n++] = (char)c;
  }
  if (*p != '"') return false;
  out[n] = 0;
  return true;
}
struct Response { char json[32768]; size_t size; };
size_t response_cb(char* ptr, size_t size, size_t nmemb, void* ctx) {
  const size_t n = size * nmemb;
  Response* r = (Response*)ctx;
  if (n > sizeof(r->json) - r->size - 1) return 0;
  memcpy(r->json + r->size, ptr, n);
  r->size += n;
  r->json[r->size] = 0;
  return n;
}
struct Output { SceUID fd; uint64_t bytes; };
size_t file_cb(char* ptr, size_t size, size_t nmemb, void* ctx) {
  const size_t n = size * nmemb;
  Output* o = (Output*)ctx;
  if (s_cancel || o->bytes + n > 2147483647u) return 0;
  const int written = sceIoWrite(o->fd, ptr, (unsigned)n);
  if (written > 0) o->bytes += (unsigned)written;
  return written > 0 ? (size_t)written : 0;
}
int progress_cb(void*, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
  if (s_cancel) return 1;
  if (total > 0 && now >= 0) {
    int pct = (int)(100.0 * now / total);
    if (pct > 100) pct = 100;
    if (pct < 0) pct = 0;
    pthread_mutex_lock(&s_lock);
    s_percent = pct;
    pthread_mutex_unlock(&s_lock);
  }
  return 0;
}
void common(CURL* curl, const char* url) {
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_CAINFO, kCaFile);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 12L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 35L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "YouTubeVita/0.41 downloader");
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
}
void run_download() {
  Settings cfg = {};
  if (!config(cfg)) {
    SbLogRaw("YTV_DOWNLOAD_CONFIG_MISSING_OR_INVALID\n");
    update("Set Cobalt server"); return;
  }
  if (!parse_id(cfg)) {
    SbLogRaw("YTV_DOWNLOAD_URL_MISSING_OR_INVALID\n");
    update("Video ID unavailable"); return;
  }
  if (s_cancel) { update("Cancelled"); return; }
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    update("TLS unavailable"); return;
  }
  CURL* curl = curl_easy_init();
  if (!curl) { update("Network error"); curl_global_cleanup(); return; }
  char body[720];
  snprintf(body, sizeof(body),
    "{\"url\":\"%s\",\"videoQuality\":\"%s\","
    "\"youtubeVideoCodec\":\"h264\",\"youtubeVideoContainer\":\"mp4\","
    "\"downloadMode\":\"%s\",\"audioFormat\":\"mp3\","
    "\"filenameStyle\":\"basic\",\"localProcessing\":\"disabled\"}",
    cfg.url, cfg.quality, strcmp(cfg.mode, "audio") == 0 ? "audio" : "auto");
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Content-Type: application/json");
  if (cfg.key[0]) {
    char auth[300];
    snprintf(auth, sizeof(auth), "Authorization: Api-Key %s", cfg.key);
    headers = curl_slist_append(headers, auth);
    memset(auth, 0, sizeof(auth));
  }
  // Avoid the 32-KiB JSON buffer on the native worker's stack.
  // Previously run_download() reserved >37 KiB, exceeding Vita pthread's
  // default 32-KiB stack and crashing at the first memset on hardware.
  Response* resp = (Response*)calloc(1, sizeof(Response));
  if (!resp) {
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    update("No download memory");
    return;
  }
  common(curl, cfg.api);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, response_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  update("Cobalt API...");
  CURLcode code = curl_easy_perform(curl);
  long status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (code != CURLE_OK || status_code != 200) {
    free(resp);
    update(s_cancel ? "Cancelled" : "Cobalt API error");
    curl_global_cleanup();
    return;
  }
  char type[32] = {}, url[2048] = {};
  if (!json_value(resp->json, "status", type, sizeof(type)) ||
      (strcmp(type, "tunnel") && strcmp(type, "redirect")) ||
      !json_value(resp->json, "url", url, sizeof(url)) ||
      strncmp(url, "https://", 8)) {
    free(resp);
    update("Unsupported result");
    curl_global_cleanup();
    return;
  }
  free(resp);
  sceIoMkdir(kRoot, 0777);
  sceIoMkdir(kDir, 0777);
  char final_path[256], temp_path[264];
  snprintf(final_path, sizeof(final_path), "%s/%s_%s.%s", kDir, cfg.video_id,
           !strcmp(cfg.mode, "audio") ? "audio" : cfg.quality,
           !strcmp(cfg.mode, "audio") ? "mp3" : "mp4");
  snprintf(temp_path, sizeof(temp_path), "%s.part", final_path);
  SceIoStat stat = {};
  if (sceIoGetstat(final_path, &stat) >= 0) {
    update("Already downloaded"); curl_global_cleanup(); return;
  }
  SceUID fd = sceIoOpen(temp_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) { update("Storage error"); curl_global_cleanup(); return; }
  Output output = {fd, 0};
  curl = curl_easy_init();
  if (!curl) {
    sceIoClose(fd); sceIoRemove(temp_path);
    update("Network error"); curl_global_cleanup(); return;
  }
  common(curl, url);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_cb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &output);
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)2147483647);
  update("Downloading", 0);
  code = curl_easy_perform(curl);
  status_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
  curl_easy_cleanup(curl);
  sceIoClose(fd);
  if (code == CURLE_OK && status_code >= 200 && status_code < 300 &&
      output.bytes > 0 && !s_cancel && sceIoRename(temp_path, final_path) >= 0) {
    update("Saved to downloads", 100);
  } else {
    sceIoRemove(temp_path);
    update(s_cancel ? "Cancelled" : "Download failed");
  }
  curl_global_cleanup();
}
void* worker(void*) {
  // Optional server-backed downloads are always background jobs.
  sceKernelChangeThreadPriority(0,180);
  SbLogRaw("YTV_LIFECYCLE_DOWNLOAD_WORKER_ENTER\n");
  run_download();
  SbLogRaw("YTV_LIFECYCLE_DOWNLOAD_WORKER_EXIT\n");
  pthread_mutex_lock(&s_lock);
  s_running = 0;
  pthread_mutex_unlock(&s_lock);
  return nullptr;
}
} // namespace

// Native Add-ons server configuration. Does not log URL or authentication key.
// Field 0 = server endpoint, 1 = optional API key.
extern "C" bool ytv_download_config_read(int field, char* result, size_t cap) {
  if (!result || cap < 2 || (field != 0 && field != 1)) return false;
  result[0] = 0;
  char contents[1024] = {};
  const SceUID fd = sceIoOpen(kConfig, SCE_O_RDONLY, 0);
  if (fd < 0) return false;
  const int n = sceIoRead(fd, contents, sizeof(contents) - 1);
  sceIoClose(fd);
  if (n <= 0) return false;
  contents[n] = 0;
  const char* name = field == 0 ? "api=" : "key=";
  const size_t klen = strlen(name);
  const char* current = contents;
  while (*current) {
    if ((current == contents || current[-1] == '\n') &&
        strncmp(current, name, klen) == 0) {
      current += klen;
      const char* end = strpbrk(current, "\r\n");
      size_t count = end ? (size_t)(end - current) : strlen(current);
      if (count >= cap) return false;
      memcpy(result, current, count);
      result[count] = 0;
      return count > 0;
    }
    const char* e = strchr(current, '\n');
    if (!e) break;
    current = e + 1;
  }
  return false;
}

extern "C" bool ytv_download_config_write(int field, const char* entered) {
  if ((field != 0 && field != 1) || !entered || strlen(entered) > 200) {
    update("Invalid setting");
    return false;
  }
  // The Vita IME output is untrusted: reject control characters and delimiters.
  for (const unsigned char* p=(const unsigned char*)entered; *p; ++p)
    if (*p < 0x20 || *p > 0x7e) {
      update("Invalid setting");
      return false;
    }
  if (field == 0 && entered[0]) {
    if (strncmp(entered,"https://",8) || !isalnum((unsigned char)entered[8]) ||
        strpbrk(entered,"@?# \t\r\n") != nullptr) {
      update("HTTPS server only");
      return false;
    }
  }
  char endpoint[256] = {}, key[256] = {};
  ytv_download_config_read(0,endpoint,sizeof(endpoint));
  ytv_download_config_read(1,key,sizeof(key));
  if (field == 0) snprintf(endpoint,sizeof(endpoint),"%s",entered);
  else snprintf(key,sizeof(key),"%s",entered);
  sceIoMkdir(kRoot,0777);
  SceUID fd = sceIoOpen(kConfig, SCE_O_WRONLY|SCE_O_CREAT|SCE_O_TRUNC, 0600);
  if (fd < 0) {
    update("Storage error");
    return false;
  }
  char bytes[640];
  const int n = snprintf(bytes,sizeof(bytes),
     "api=%s\nkey=%s\nquality=480\nmode=video\n",endpoint,key);
  const bool good = n > 0 && n < (int)sizeof(bytes) &&
                    sceIoWrite(fd,bytes,(unsigned)n) == n;
  sceIoClose(fd);
  memset(bytes,0,sizeof(bytes));
  memset(key,0,sizeof(key));
  if (!good) {
    update("Storage error");
    return false;
  }
  update(field == 0 ? "Server configured" : "Key updated");
  SbLogRaw(field == 0 ? "YTV_DOWNLOAD_SERVER_SET\n" : "YTV_DOWNLOAD_KEY_SET\n");
  return true;
}

extern "C" int ytv_download_start_or_cancel() {
  pthread_mutex_lock(&s_lock);
  if (s_running) {
    s_cancel = 1;
    pthread_mutex_unlock(&s_lock);
    update("Cancelling");
    return 0;
  }
  SbLogRaw("YTV_DOWNLOAD_REQUEST\n");
  s_running = 1;
  s_cancel = 0;
  snprintf(s_status, sizeof(s_status), "Starting");
  s_percent = -1;
  pthread_mutex_unlock(&s_lock);
  // On Vita, pthread's default stack is 0x8000 (32 KiB). Even after
  // moving the API response to heap, give TLS/curl a known safe stack.
  pthread_attr_t attr;
  const int attr_ok = pthread_attr_init(&attr);
  const int stack_ok = attr_ok == 0 ? pthread_attr_setstacksize(&attr, 128 * 1024) : -1;
  if (stack_ok != 0) {
    if (attr_ok == 0) pthread_attr_destroy(&attr);
    update("Thread stack error");
    pthread_mutex_lock(&s_lock);
    s_running = 0;
    pthread_mutex_unlock(&s_lock);
    return -1;
  }
  pthread_t thread;
  const int rc = pthread_create(&thread, &attr, worker, nullptr);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    update("Thread error");
    pthread_mutex_lock(&s_lock);
    s_running = 0;
    pthread_mutex_unlock(&s_lock);
    return -1;
  }
  pthread_detach(thread);
  return 1;
}
extern "C" void ytv_download_get_status(char* out, size_t capacity) {
  if (!out || !capacity) return;
  pthread_mutex_lock(&s_lock);
  if (s_running && s_percent >= 0 && !strcmp(s_status, "Downloading"))
    snprintf(out, capacity, "%d%%", s_percent);
  else snprintf(out, capacity, "%s", s_status);
  pthread_mutex_unlock(&s_lock);
}
