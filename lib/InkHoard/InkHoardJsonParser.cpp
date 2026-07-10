#include "InkHoardJsonParser.h"

#include <cstring>

namespace {

bool keyEq(const char* key, size_t len, const char* lit) {
  const size_t n = std::strlen(lit);
  return len == n && std::memcmp(key, lit, n) == 0;
}

int parseInt(const char* value, size_t len) {
  int sign = 1;
  size_t i = 0;
  if (len > 0 && value[0] == '-') {
    sign = -1;
    i = 1;
  }
  int v = 0;
  for (; i < len; ++i) {
    if (value[i] < '0' || value[i] > '9') break;
    v = v * 10 + (value[i] - '0');
  }
  return sign * v;
}

bool isUtf8Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

/** Copy ≤ maxPayload bytes on a UTF-8 code-point boundary into dest (NUL-terminated). */
void copyUtf8Bounded(char* dest, size_t destCap, const char* src, size_t len) {
  if (!dest || destCap == 0) return;
  const size_t maxPayload = destCap - 1;
  size_t n = len < maxPayload ? len : maxPayload;
  while (n > 0 && isUtf8Continuation(static_cast<unsigned char>(src[n]))) {
    --n;
  }
  if (n > 0) std::memcpy(dest, src, n);
  dest[n] = '\0';
}

}  // namespace

InkHoardJsonParser::InkHoardJsonParser()
    : parser(JsonCallbacks{this, sOnKey, sOnString, sOnNumber, sOnBool, sOnNull, sOnObjectStart, sOnObjectEnd,
                           sOnArrayStart, sOnArrayEnd}) {
  reset(Kind::LibraryPage);
}

InkHoardJsonParser::~InkHoardJsonParser() = default;

bool InkHoardJsonParser::ensurePageBuffers() {
  if (kind == Kind::LibraryPage) {
    if (!library) {
      library = std::make_unique<inkhoard::LibraryPage>();
      if (!library) return false;
    }
    *library = {};
    search.reset();
  } else if (kind == Kind::SearchPage) {
    if (!search) {
      search = std::make_unique<inkhoard::SearchPage>();
      if (!search) return false;
    }
    *search = {};
    library.reset();
  } else {
    library.reset();
    search.reset();
  }
  return true;
}

const inkhoard::LibraryPage& InkHoardJsonParser::libraryPage() const {
  static const inkhoard::LibraryPage kEmpty{};
  return library ? *library : kEmpty;
}

const inkhoard::SearchPage& InkHoardJsonParser::searchPage() const {
  static const inkhoard::SearchPage kEmpty{};
  return search ? *search : kEmpty;
}

void InkHoardJsonParser::reset(Kind k) {
  kind = k;
  position = Position::TOP;
  lastKey = LastKey::NONE;
  depth = 0;
  itemDepth = 0;
  error = false;
  oversize = false;
  totalBytes = 0;
  itemsSeen = false;
  skippingExtraItem = false;
  detail = {};
  apiErr = {};
  scratchItem = {};
  if (!ensurePageBuffers()) {
    error = true;
  }
  parser.reset();
}

bool InkHoardJsonParser::ok() const {
  if (hasError()) return false;
  switch (kind) {
    case Kind::LibraryPage:
      return library && library->valid;
    case Kind::SearchPage:
      return search && search->valid;
    case Kind::CompactItem:
      return detail.valid;
    case Kind::ApiError:
      return apiErr.valid;
  }
  return false;
}

void InkHoardJsonParser::feed(const char* data, size_t len) {
  if (oversize || error) return;
  if (totalBytes + len >= inkhoard::MAX_JSON_RESPONSE_BYTES) {
    oversize = true;
    error = true;
    return;
  }
  totalBytes += len;
  parser.feed(data, len);
  if (parser.hasError()) error = true;
}

void InkHoardJsonParser::copyField(char* dest, size_t destCap, const char* src, size_t len) {
  copyUtf8Bounded(dest, destCap, src, len);
}

inkhoard::CompactItem* InkHoardJsonParser::currentItem() {
  if (skippingExtraItem) return nullptr;
  if (kind == Kind::CompactItem && (position == Position::IN_ITEM || position == Position::IN_TAGS)) {
    return &detail;
  }
  if (position == Position::IN_ITEM || position == Position::IN_TAGS) return &scratchItem;
  return nullptr;
}

void InkHoardJsonParser::finishItem() {
  if (skippingExtraItem) {
    skippingExtraItem = false;
    return;
  }
  scratchItem.valid = scratchItem.id[0] != '\0';
  if (!scratchItem.valid) return;

  if (kind == Kind::LibraryPage && library) {
    if (library->itemCount < inkhoard::MAX_PAGE_ITEMS) {
      library->items[library->itemCount++] = scratchItem;
    }
  } else if (kind == Kind::SearchPage && search) {
    if (search->itemCount < inkhoard::MAX_PAGE_ITEMS) {
      search->items[search->itemCount++] = scratchItem;
    }
  }
  scratchItem = {};
}

void InkHoardJsonParser::sOnKey(void* ctx, const char* key, size_t len) {
  static_cast<InkHoardJsonParser*>(ctx)->onKey(key, len);
}
void InkHoardJsonParser::sOnString(void* ctx, const char* value, size_t len) {
  static_cast<InkHoardJsonParser*>(ctx)->onString(value, len);
}
void InkHoardJsonParser::sOnNumber(void* ctx, const char* value, size_t len) {
  static_cast<InkHoardJsonParser*>(ctx)->onNumber(value, len);
}
void InkHoardJsonParser::sOnBool(void* ctx, bool value) { static_cast<InkHoardJsonParser*>(ctx)->onBool(value); }
void InkHoardJsonParser::sOnNull(void* ctx) { static_cast<InkHoardJsonParser*>(ctx)->onNull(); }
void InkHoardJsonParser::sOnObjectStart(void* ctx) { static_cast<InkHoardJsonParser*>(ctx)->onObjectStart(); }
void InkHoardJsonParser::sOnObjectEnd(void* ctx) { static_cast<InkHoardJsonParser*>(ctx)->onObjectEnd(); }
void InkHoardJsonParser::sOnArrayStart(void* ctx) { static_cast<InkHoardJsonParser*>(ctx)->onArrayStart(); }
void InkHoardJsonParser::sOnArrayEnd(void* ctx) { static_cast<InkHoardJsonParser*>(ctx)->onArrayEnd(); }

void InkHoardJsonParser::onKey(const char* key, size_t len) {
  if (keyEq(key, len, "items"))
    lastKey = LastKey::ITEMS;
  else if (keyEq(key, len, "next_cursor"))
    lastKey = LastKey::NEXT_CURSOR;
  else if (keyEq(key, len, "has_more"))
    lastKey = LastKey::HAS_MORE;
  else if (keyEq(key, len, "limit"))
    lastKey = LastKey::LIMIT;
  else if (keyEq(key, len, "offset"))
    lastKey = LastKey::OFFSET;
  else if (keyEq(key, len, "id"))
    lastKey = LastKey::ID;
  else if (keyEq(key, len, "title"))
    lastKey = LastKey::TITLE;
  else if (keyEq(key, len, "url"))
    lastKey = LastKey::URL;
  else if (keyEq(key, len, "source"))
    lastKey = LastKey::SOURCE;
  else if (keyEq(key, len, "type"))
    lastKey = LastKey::TYPE;
  else if (keyEq(key, len, "status"))
    lastKey = LastKey::STATUS;
  else if (keyEq(key, len, "created_at"))
    lastKey = LastKey::CREATED_AT;
  else if (keyEq(key, len, "updated_at"))
    lastKey = LastKey::UPDATED_AT;
  else if (keyEq(key, len, "content_version"))
    lastKey = LastKey::CONTENT_VERSION;
  else if (keyEq(key, len, "tags"))
    lastKey = LastKey::TAGS;
  else if (keyEq(key, len, "epub_available"))
    lastKey = LastKey::EPUB_AVAILABLE;
  else if (keyEq(key, len, "error"))
    lastKey = LastKey::ERROR;
  else if (keyEq(key, len, "code"))
    lastKey = LastKey::CODE;
  else
    lastKey = LastKey::UNKNOWN;
}

void InkHoardJsonParser::onString(const char* value, size_t len) {
  if (position == Position::IN_TAGS) {
    auto* item = currentItem();
    if (item && item->tagCount < inkhoard::MAX_TAGS) {
      copyField(item->tags[item->tagCount], inkhoard::MAX_TAG_BYTES + 1, value, len);
      item->tagCount++;
    }
    return;
  }

  if (kind == Kind::ApiError || position == Position::IN_ERROR_OBJ ||
      (position == Position::TOP && (lastKey == LastKey::ERROR || lastKey == LastKey::CODE))) {
    if (lastKey == LastKey::ERROR) {
      copyField(apiErr.error, sizeof(apiErr.error), value, len);
      apiErr.valid = apiErr.code[0] != '\0' || apiErr.error[0] != '\0';
    } else if (lastKey == LastKey::CODE) {
      copyField(apiErr.code, sizeof(apiErr.code), value, len);
      apiErr.valid = true;
    }
    lastKey = LastKey::NONE;
    return;
  }

  auto* item = currentItem();
  if (item) {
    switch (lastKey) {
      case LastKey::ID:
        copyField(item->id, sizeof(item->id), value, len);
        break;
      case LastKey::TITLE:
        copyField(item->title, sizeof(item->title), value, len);
        item->titleIsNull = false;
        break;
      case LastKey::URL:
        copyField(item->url, sizeof(item->url), value, len);
        break;
      case LastKey::SOURCE:
        copyField(item->source, sizeof(item->source), value, len);
        break;
      case LastKey::TYPE:
        copyField(item->type, sizeof(item->type), value, len);
        break;
      case LastKey::STATUS:
        copyField(item->status, sizeof(item->status), value, len);
        break;
      case LastKey::CREATED_AT:
        copyField(item->createdAt, sizeof(item->createdAt), value, len);
        break;
      case LastKey::UPDATED_AT:
        copyField(item->updatedAt, sizeof(item->updatedAt), value, len);
        break;
      case LastKey::CONTENT_VERSION:
        copyField(item->contentVersion, sizeof(item->contentVersion), value, len);
        break;
      default:
        break;
    }
    lastKey = LastKey::NONE;
    return;
  }

  if (position == Position::TOP) {
    if (lastKey == LastKey::NEXT_CURSOR && kind == Kind::LibraryPage && library) {
      copyField(library->nextCursor, sizeof(library->nextCursor), value, len);
      library->hasNextCursor = library->nextCursor[0] != '\0';
    }
  }
  lastKey = LastKey::NONE;
}

void InkHoardJsonParser::onNumber(const char* value, size_t len) {
  if (position == Position::TOP && kind == Kind::SearchPage && search) {
    if (lastKey == LastKey::LIMIT) search->limit = parseInt(value, len);
    if (lastKey == LastKey::OFFSET) search->offset = parseInt(value, len);
  }
  lastKey = LastKey::NONE;
}

void InkHoardJsonParser::onBool(bool value) {
  if (lastKey == LastKey::HAS_MORE && position == Position::TOP) {
    if (kind == Kind::LibraryPage && library) library->hasMore = value;
    if (kind == Kind::SearchPage && search) search->hasMore = value;
  } else if (lastKey == LastKey::EPUB_AVAILABLE) {
    if (auto* item = currentItem()) item->epubAvailable = value;
  }
  lastKey = LastKey::NONE;
}

void InkHoardJsonParser::onNull() {
  if (lastKey == LastKey::TITLE) {
    if (auto* item = currentItem()) {
      item->title[0] = '\0';
      item->titleIsNull = true;
    }
  } else if (lastKey == LastKey::NEXT_CURSOR && kind == Kind::LibraryPage && library) {
    library->nextCursor[0] = '\0';
    library->hasNextCursor = false;
  }
  lastKey = LastKey::NONE;
}

void InkHoardJsonParser::onObjectStart() {
  depth++;
  if (kind == Kind::CompactItem && depth == 1) {
    position = Position::IN_ITEM;
    itemDepth = 1;
    detail = {};
    return;
  }
  if (kind == Kind::ApiError && depth == 1) {
    position = Position::IN_ERROR_OBJ;
    return;
  }
  if (position == Position::IN_ITEMS) {
    // Starting a new item object
    const uint8_t count = (kind == Kind::LibraryPage && library)   ? library->itemCount
                          : (kind == Kind::SearchPage && search) ? search->itemCount
                                                                 : inkhoard::MAX_PAGE_ITEMS;
    if (count >= inkhoard::MAX_PAGE_ITEMS) {
      skippingExtraItem = true;
    } else {
      scratchItem = {};
    }
    position = Position::IN_ITEM;
    itemDepth = depth;
  }
}

void InkHoardJsonParser::onObjectEnd() {
  if (position == Position::IN_ITEM && depth == itemDepth) {
    if (kind == Kind::CompactItem) {
      detail.valid = detail.id[0] != '\0';
      position = Position::TOP;
    } else {
      finishItem();
      position = Position::IN_ITEMS;
    }
  } else if (position == Position::IN_ERROR_OBJ && depth == 1) {
    position = Position::TOP;
  } else if (depth == 1 && (kind == Kind::LibraryPage || kind == Kind::SearchPage)) {
    if (kind == Kind::LibraryPage && library) library->valid = itemsSeen;
    if (kind == Kind::SearchPage && search) search->valid = itemsSeen;
  }
  if (depth > 0) depth--;
  lastKey = LastKey::NONE;
}

void InkHoardJsonParser::onArrayStart() {
  depth++;
  if (lastKey == LastKey::ITEMS && position == Position::TOP) {
    position = Position::IN_ITEMS;
    itemsSeen = true;
  } else if (lastKey == LastKey::TAGS && (position == Position::IN_ITEM || kind == Kind::CompactItem)) {
    position = Position::IN_TAGS;
  }
  lastKey = LastKey::NONE;
}

void InkHoardJsonParser::onArrayEnd() {
  if (position == Position::IN_TAGS) {
    position = Position::IN_ITEM;
  } else if (position == Position::IN_ITEMS) {
    position = Position::TOP;
  }
  if (depth > 0) depth--;
  lastKey = LastKey::NONE;
}
