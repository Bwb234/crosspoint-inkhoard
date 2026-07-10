#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Fixed-size device-api models (plan 001 contract bounds).
 * INKHOARD: plan 008
 */
namespace inkhoard {

constexpr size_t MAX_ID_BYTES = 64;
constexpr size_t MAX_TITLE_BYTES = 256;
constexpr size_t MAX_URL_FIELD_BYTES = 1024;
constexpr size_t MAX_SOURCE_BYTES = 32;
constexpr size_t MAX_TYPE_BYTES = 32;
constexpr size_t MAX_STATUS_BYTES = 32;
constexpr size_t MAX_ISO_BYTES = 40;
constexpr size_t MAX_TAG_BYTES = 32;
constexpr size_t MAX_TAGS = 8;
constexpr size_t MAX_CURSOR_BYTES = 256;
constexpr size_t MAX_ERROR_MSG_BYTES = 256;
constexpr size_t MAX_ERROR_CODE_BYTES = 64;
/** Client-requested page size cap (contract max 50; budget for ~20 default). */
constexpr size_t MAX_PAGE_ITEMS = 25;
/** Abort JSON responses at or above this size. */
constexpr size_t MAX_JSON_RESPONSE_BYTES = 32768;

struct CompactItem {
  char id[MAX_ID_BYTES + 1] = {};
  char title[MAX_TITLE_BYTES + 1] = {};
  bool titleIsNull = true;
  char url[MAX_URL_FIELD_BYTES + 1] = {};
  char source[MAX_SOURCE_BYTES + 1] = {};
  char type[MAX_TYPE_BYTES + 1] = {};
  char status[MAX_STATUS_BYTES + 1] = {};
  char createdAt[MAX_ISO_BYTES + 1] = {};
  char updatedAt[MAX_ISO_BYTES + 1] = {};
  char contentVersion[MAX_ISO_BYTES + 1] = {};
  char tags[MAX_TAGS][MAX_TAG_BYTES + 1] = {};
  uint8_t tagCount = 0;
  bool epubAvailable = false;
  bool valid = false;
};

struct LibraryPage {
  CompactItem items[MAX_PAGE_ITEMS];
  uint8_t itemCount = 0;
  char nextCursor[MAX_CURSOR_BYTES + 1] = {};
  bool hasNextCursor = false;
  bool hasMore = false;
  bool valid = false;
};

struct SearchPage {
  CompactItem items[MAX_PAGE_ITEMS];
  uint8_t itemCount = 0;
  int limit = 0;
  int offset = 0;
  bool hasMore = false;
  bool valid = false;
};

struct ApiError {
  char error[MAX_ERROR_MSG_BYTES + 1] = {};
  char code[MAX_ERROR_CODE_BYTES + 1] = {};
  bool valid = false;
};

enum class ClientResult : uint8_t {
  Ok = 0,
  AuthInvalid,      // 401
  AuthForbidden,    // 403
  NotFound,         // 404
  ContentNotReady,  // 409
  ContentFailed,    // 422
  BadRequest,       // 400
  ServerError,      // 5xx
  NotModified,      // 304
  TransportError,
  TlsError,
  OversizeResponse,
  ParseError,
  NoCredentials,
  LowMemory,
  FileError,
  Aborted,
};

const char* clientResultLabel(ClientResult r);

}  // namespace inkhoard
