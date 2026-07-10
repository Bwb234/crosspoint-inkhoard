#include "InkHoardDownloadPaths.h"

#include <cstdio>
#include <cstring>

namespace inkhoard {
namespace {

bool extractString(const char* json, size_t len, const char* key, char* out, size_t outCap) {
  if (!json || !key || !out || outCap == 0) return false;
  out[0] = '\0';
  char needle[48];
  std::snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = json;
  const char* end = json + len;
  while (p < end) {
    const char* found = static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(end - p)));
    if (!found) return false;
    if (static_cast<size_t>(end - found) >= std::strlen(needle) && std::strncmp(found, needle, std::strlen(needle)) == 0) {
      const char* colon = found + std::strlen(needle);
      while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r')) ++colon;
      if (colon >= end || *colon != ':') {
        p = found + 1;
        continue;
      }
      ++colon;
      while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r')) ++colon;
      if (colon >= end || *colon != '"') return false;
      ++colon;
      size_t i = 0;
      while (colon < end && *colon != '"') {
        if (*colon == '\\' && colon + 1 < end) {
          ++colon;
        }
        if (i + 1 >= outCap) return false;
        out[i++] = *colon++;
      }
      out[i] = '\0';
      return true;
    }
    p = found + 1;
  }
  return false;
}

bool extractSize(const char* json, size_t len, size_t& out) {
  out = 0;
  const char* key = "\"size\"";
  const char* found = std::strstr(json, key);
  if (!found || found >= json + len) return false;
  const char* p = found + std::strlen(key);
  const char* end = json + len;
  while (p < end && (*p == ' ' || *p == '\t' || *p == ':' || *p == '\n' || *p == '\r')) ++p;
  if (p >= end || *p < '0' || *p > '9') return false;
  size_t v = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    v = v * 10 + static_cast<size_t>(*p - '0');
    ++p;
  }
  out = v;
  return true;
}

void jsonEscape(const char* in, char* out, size_t outCap) {
  if (!out || outCap == 0) return;
  size_t o = 0;
  if (!in) {
    out[0] = '\0';
    return;
  }
  for (const char* p = in; *p && o + 1 < outCap; ++p) {
    if (*p == '"' || *p == '\\') {
      if (o + 2 >= outCap) break;
      out[o++] = '\\';
      out[o++] = *p;
    } else if (static_cast<unsigned char>(*p) < 0x20) {
      continue;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = '\0';
}

}  // namespace

bool parseSidecarJson(const char* json, size_t len, Sidecar& out) {
  out = Sidecar{};
  if (!json || len == 0 || len >= MAX_SIDECAR_BYTES) return false;

  if (!extractString(json, len, "id", out.id, sizeof(out.id))) return false;
  if (!isSafeItemId(out.id)) return false;
  extractString(json, len, "title", out.title, sizeof(out.title));
  extractString(json, len, "etag", out.etag, sizeof(out.etag));
  extractString(json, len, "contentVersion", out.contentVersion, sizeof(out.contentVersion));
  extractString(json, len, "downloadedAt", out.downloadedAt, sizeof(out.downloadedAt));
  extractSize(json, len, out.size);
  out.valid = true;
  return true;
}

size_t formatSidecarJson(char* buf, size_t cap, const Sidecar& s) {
  if (!buf || cap < 32 || !isSafeItemId(s.id)) return 0;
  char titleEsc[512];
  char etagEsc[512];
  char verEsc[96];
  char atEsc[96];
  jsonEscape(s.title, titleEsc, sizeof(titleEsc));
  jsonEscape(s.etag, etagEsc, sizeof(etagEsc));
  jsonEscape(s.contentVersion, verEsc, sizeof(verEsc));
  jsonEscape(s.downloadedAt, atEsc, sizeof(atEsc));
  const int n = std::snprintf(buf, cap,
                              "{\"id\":\"%s\",\"title\":\"%s\",\"etag\":\"%s\",\"contentVersion\":\"%s\","
                              "\"size\":%u,\"downloadedAt\":\"%s\"}",
                              s.id, titleEsc, etagEsc, verEsc, static_cast<unsigned>(s.size), atEsc);
  if (n <= 0 || static_cast<size_t>(n) >= cap || static_cast<size_t>(n) >= MAX_SIDECAR_BYTES) return 0;
  return static_cast<size_t>(n);
}

}  // namespace inkhoard
