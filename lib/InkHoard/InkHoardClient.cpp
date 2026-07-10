#include "InkHoardClient.h"

#include <cstdio>
#include <cstring>
#include <memory>

#ifdef ARDUINO
#include <Logging.h>

#include "InkHoardCredentialStore.h"
#include "InkHoardEspTransport.h"
#endif

namespace {
#ifdef ARDUINO
InkHoardEspTransport& defaultTransport() {
  static InkHoardEspTransport t;
  return t;
}

bool hasCreds() { return INKHOARD_STORE.hasCredentials(); }
std::string baseUrl() { return INKHOARD_STORE.getBaseUrl(); }
const std::string& token() { return INKHOARD_STORE.getToken(); }
#else
// Host unit tests inject transport and must also inject credentials via a test double
// by calling methods that take explicit URL/token — see performJsonGetUrl.
bool hasCreds() { return true; }
std::string baseUrl() { return "https://test.invalid"; }
std::string tokenStorage;
const std::string& token() { return tokenStorage; }
#endif

void delayRetry(uint8_t attemptIndex) {
#ifdef ARDUINO
  delay(attemptIndex == 0 ? 1000 : 3000);
#else
  (void)attemptIndex;
#endif
}

}  // namespace

InkHoardClient::InkHoardClient(inkhoard::InkHoardTransport* transport) : transport_(transport) {
#ifdef ARDUINO
  if (!transport_) transport_ = &defaultTransport();
#endif
}

std::string InkHoardClient::buildUrl(const std::string& pathAndQuery) const {
  std::string base = baseUrl();
  if (base.empty()) return {};
  if (!pathAndQuery.empty() && pathAndQuery[0] != '/') {
    return base + "/" + pathAndQuery;
  }
  return base + pathAndQuery;
}

InkHoardClient::Outcome InkHoardClient::performJsonGet(const std::string& pathAndQuery,
                                                       InkHoardJsonParser::Kind kind, void* outStruct) {
  Outcome outcome;
  if (!hasCreds()) {
    outcome.result = inkhoard::ClientResult::NoCredentials;
    return outcome;
  }
  if (!transport_) {
    outcome.result = inkhoard::ClientResult::TransportError;
    return outcome;
  }

  const std::string url = buildUrl(pathAndQuery);
  if (url.empty()) {
    outcome.result = inkhoard::ClientResult::NoCredentials;
    return outcome;
  }

  constexpr uint8_t MAX_ATTEMPTS = 3;  // initial + 2 retries
  for (uint8_t attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    outcome.attempts = static_cast<uint8_t>(attempt + 1);
    const auto resp = transport_->getJson(url, token());
    outcome.httpCode = resp.httpCode;

    if (resp.lowMemory) {
      outcome.result = inkhoard::ClientResult::LowMemory;
      return outcome;
    }
    if (resp.oversize) {
      outcome.result = inkhoard::ClientResult::OversizeResponse;
      return outcome;
    }
    if (resp.tlsFailure) {
      outcome.result = inkhoard::ClientResult::TlsError;
      if (attempt + 1 < MAX_ATTEMPTS) {
        delayRetry(attempt);
        continue;
      }
      return outcome;
    }
    if (resp.transportFailure || !resp.ok) {
      outcome.result = inkhoard::ClientResult::TransportError;
      if (attempt + 1 < MAX_ATTEMPTS) {
        delayRetry(attempt);
        continue;
      }
      return outcome;
    }

    if (resp.httpCode == 200) {
      // Heap-allocate: parser page buffers are large; never stack them on loopTask.
      auto parser = std::make_unique<InkHoardJsonParser>();
      if (!parser) {
        outcome.result = inkhoard::ClientResult::LowMemory;
        return outcome;
      }
      parser->reset(kind);
      parser->feed(resp.body.data(), resp.body.size());
      if (parser->isOversize()) {
        outcome.result = inkhoard::ClientResult::OversizeResponse;
        return outcome;
      }
      if (!parser->ok()) {
        outcome.result = inkhoard::ClientResult::ParseError;
        return outcome;
      }
      switch (kind) {
        case InkHoardJsonParser::Kind::LibraryPage:
          *static_cast<inkhoard::LibraryPage*>(outStruct) = parser->libraryPage();
          break;
        case InkHoardJsonParser::Kind::SearchPage:
          *static_cast<inkhoard::SearchPage*>(outStruct) = parser->searchPage();
          break;
        case InkHoardJsonParser::Kind::CompactItem:
          *static_cast<inkhoard::CompactItem*>(outStruct) = parser->item();
          break;
        case InkHoardJsonParser::Kind::ApiError:
          break;
      }
      outcome.result = inkhoard::ClientResult::Ok;
      return outcome;
    }

    if (!resp.body.empty()) {
      auto errParser = std::make_unique<InkHoardJsonParser>();
      if (errParser) {
        errParser->reset(InkHoardJsonParser::Kind::ApiError);
        errParser->feed(resp.body.data(), resp.body.size());
        if (errParser->ok()) {
          outcome.error = errParser->apiError();
        }
      }
    }
    outcome.result = inkhoard::mapHttpStatus(resp.httpCode, outcome.error.code);

    if (inkhoard::shouldRetry(outcome.result) && attempt + 1 < MAX_ATTEMPTS) {
      delayRetry(attempt);
      continue;
    }
    return outcome;
  }
  return outcome;
}

InkHoardClient::Outcome InkHoardClient::testConnection() {
  // Heap-allocate page buffer (~45KB) — stack would overflow loopTask.
  auto page = std::make_unique<inkhoard::LibraryPage>();
  if (!page) {
    Outcome o;
    o.result = inkhoard::ClientResult::LowMemory;
    return o;
  }
  return performJsonGet("/api/device/v1/library?limit=1", InkHoardJsonParser::Kind::LibraryPage, page.get());
}

InkHoardClient::Outcome InkHoardClient::fetchLibraryPage(inkhoard::LibraryPage& out, const char* cursor, int limit) {
  if (limit < 1) limit = 1;
  if (limit > static_cast<int>(inkhoard::MAX_PAGE_ITEMS)) limit = static_cast<int>(inkhoard::MAX_PAGE_ITEMS);
  char path[384];
  if (cursor && cursor[0]) {
    std::snprintf(path, sizeof(path), "/api/device/v1/library?limit=%d&cursor=%s", limit, cursor);
  } else {
    std::snprintf(path, sizeof(path), "/api/device/v1/library?limit=%d", limit);
  }
  return performJsonGet(path, InkHoardJsonParser::Kind::LibraryPage, &out);
}

InkHoardClient::Outcome InkHoardClient::search(inkhoard::SearchPage& out, const char* query, int offset, int limit) {
  if (!query || !query[0]) {
    Outcome o;
    o.result = inkhoard::ClientResult::BadRequest;
    return o;
  }
  if (limit < 1) limit = 1;
  if (limit > static_cast<int>(inkhoard::MAX_PAGE_ITEMS)) limit = static_cast<int>(inkhoard::MAX_PAGE_ITEMS);
  if (offset < 0) offset = 0;
  if (offset > 500) offset = 500;
  char path[512];
  std::string q;
  for (const char* p = query; *p; ++p) {
    if (*p == ' ')
      q += "%20";
    else if (*p == '&' || *p == '?' || *p == '#')
      continue;
    else
      q += *p;
  }
  std::snprintf(path, sizeof(path), "/api/device/v1/search?q=%s&limit=%d&offset=%d", q.c_str(), limit, offset);
  return performJsonGet(path, InkHoardJsonParser::Kind::SearchPage, &out);
}

InkHoardClient::Outcome InkHoardClient::fetchItemDetail(inkhoard::CompactItem& out, const char* id) {
  if (!id || !id[0]) {
    Outcome o;
    o.result = inkhoard::ClientResult::BadRequest;
    return o;
  }
  char path[160];
  std::snprintf(path, sizeof(path), "/api/device/v1/items/%s", id);
  return performJsonGet(path, InkHoardJsonParser::Kind::CompactItem, &out);
}

#ifndef ARDUINO
// Device build provides downloadEpub in src/inkhoard/InkHoardClientEpub.cpp
// (needs HttpDownloader from src/network — not visible to PlatformIO lib/).
InkHoardClient::Outcome InkHoardClient::downloadEpub(const char* id, const char* destPath, const char* ifNoneMatch,
                                                     char* etagOut, size_t etagOutCap, ProgressCallback progress) {
  (void)id;
  (void)destPath;
  (void)ifNoneMatch;
  (void)etagOut;
  (void)etagOutCap;
  (void)progress;
  Outcome outcome;
  outcome.result = inkhoard::ClientResult::TransportError;
  return outcome;
}
#endif
