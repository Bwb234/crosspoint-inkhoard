#pragma once

#include <cstddef>
#include <cstring>
#include <string>

/**
 * Pure path helpers for InkHoard SD layout (plan 009).
 * Host-testable; no Arduino / HalStorage dependency.
 */
namespace inkhoard {

inline constexpr const char* ROOT_DIR = "/InkHoard";
inline constexpr const char* ITEMS_DIR = "/InkHoard/items";
inline constexpr const char* TMP_DIR = "/InkHoard/tmp";
inline constexpr size_t MAX_ID_PATH_BYTES = 64;
inline constexpr size_t MAX_SIDECAR_BYTES = 1024;
inline constexpr size_t MAX_SCAN_SIDECARS = 64;

/** Reject path traversal / separators in bookmark ids used as filenames. */
inline bool isSafeItemId(const char* id) {
  if (!id || !id[0]) return false;
  if (std::strcmp(id, ".") == 0 || std::strcmp(id, "..") == 0) return false;
  size_t n = 0;
  for (const char* p = id; *p; ++p, ++n) {
    if (n >= MAX_ID_PATH_BYTES) return false;
    const unsigned char c = static_cast<unsigned char>(*p);
    if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
        c == '|') {
      return false;
    }
    if (c == '.' && p[1] == '.') return false;
  }
  return true;
}

inline std::string itemEpubPath(const char* id) {
  if (!isSafeItemId(id)) return {};
  return std::string(ITEMS_DIR) + "/" + id + ".epub";
}

inline std::string itemSidecarPath(const char* id) {
  if (!isSafeItemId(id)) return {};
  return std::string(ITEMS_DIR) + "/" + id + ".json";
}

inline std::string itemPartPath(const char* id) {
  if (!isSafeItemId(id)) return {};
  return std::string(TMP_DIR) + "/" + id + ".part";
}

inline std::string itemSidecarPartPath(const char* id) {
  if (!isSafeItemId(id)) return {};
  return std::string(TMP_DIR) + "/" + id + ".json.part";
}

struct Sidecar {
  char id[MAX_ID_PATH_BYTES + 1] = {};
  char title[256 + 1] = {};
  char etag[256 + 1] = {};
  char contentVersion[40 + 1] = {};
  size_t size = 0;
  char downloadedAt[40 + 1] = {};
  bool valid = false;
};

/**
 * Minimal sidecar JSON parse (fixed keys, bounded). Corrupt → valid=false.
 * Expected shape:
 * {"id":"...","title":"...","etag":"...","contentVersion":"...","size":123,"downloadedAt":"..."}
 */
bool parseSidecarJson(const char* json, size_t len, Sidecar& out);

/** Build sidecar JSON into buf; returns bytes written (excluding NUL) or 0 on failure. */
size_t formatSidecarJson(char* buf, size_t cap, const Sidecar& s);

}  // namespace inkhoard
