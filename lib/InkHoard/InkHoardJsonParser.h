#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "InkHoardModels.h"
#include "StreamingJsonParser.h"

/**
 * Bounded SAX parser for device-api JSON into fixed-size models.
 * Composes StreamingJsonParser (same pattern as ReleaseJsonParser).
 * INKHOARD: plan 008
 *
 * LibraryPage/SearchPage live on the heap — they are ~45KB each and must never
 * be stack-allocated on the ESP32 Arduino loopTask (~8KB).
 */
class InkHoardJsonParser {
 public:
  enum class Kind : uint8_t { LibraryPage, SearchPage, CompactItem, ApiError };

  InkHoardJsonParser();
  ~InkHoardJsonParser();

  InkHoardJsonParser(const InkHoardJsonParser&) = delete;
  InkHoardJsonParser& operator=(const InkHoardJsonParser&) = delete;

  void reset(Kind kind);
  void feed(const char* data, size_t len);

  bool hasError() const { return error || parser.hasError() || oversize; }
  bool isOversize() const { return oversize; }
  size_t bytesSeen() const { return totalBytes; }

  const inkhoard::LibraryPage& libraryPage() const;
  const inkhoard::SearchPage& searchPage() const;
  const inkhoard::CompactItem& item() const { return detail; }
  const inkhoard::ApiError& apiError() const { return apiErr; }

  /** True when the target structure was filled without parse/oversize error. */
  bool ok() const;

 private:
  enum class Position : uint8_t {
    TOP,
    IN_ITEMS,
    IN_ITEM,
    IN_TAGS,
    IN_ERROR_OBJ,
  };

  enum class LastKey : uint8_t {
    NONE,
    ITEMS,
    NEXT_CURSOR,
    HAS_MORE,
    LIMIT,
    OFFSET,
    ID,
    TITLE,
    URL,
    SOURCE,
    TYPE,
    STATUS,
    CREATED_AT,
    UPDATED_AT,
    CONTENT_VERSION,
    TAGS,
    EPUB_AVAILABLE,
    ERROR,
    CODE,
    UNKNOWN,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  void onKey(const char* key, size_t len);
  void onString(const char* value, size_t len);
  void onNumber(const char* value, size_t len);
  void onBool(bool value);
  void onNull();
  void onObjectStart();
  void onObjectEnd();
  void onArrayStart();
  void onArrayEnd();

  void copyField(char* dest, size_t destCap, const char* src, size_t len);
  inkhoard::CompactItem* currentItem();
  void finishItem();
  bool ensurePageBuffers();

  StreamingJsonParser parser;
  Kind kind = Kind::LibraryPage;
  Position position = Position::TOP;
  LastKey lastKey = LastKey::NONE;
  uint8_t depth = 0;
  uint8_t itemDepth = 0;
  bool error = false;
  bool oversize = false;
  size_t totalBytes = 0;
  bool itemsSeen = false;
  bool skippingExtraItem = false;

  std::unique_ptr<inkhoard::LibraryPage> library;
  std::unique_ptr<inkhoard::SearchPage> search;
  inkhoard::CompactItem detail;
  inkhoard::ApiError apiErr;
  inkhoard::CompactItem scratchItem;
};
