#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "InkHoardJsonParser.h"
#include "InkHoardModels.h"
#include "InkHoardTransport.h"

/**
 * Authenticated device-api client: bounded JSON GETs + streamed EPUB download.
 * INKHOARD: plan 008
 */
class InkHoardClient {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  struct Outcome {
    inkhoard::ClientResult result = inkhoard::ClientResult::NoCredentials;
    int httpCode = 0;
    inkhoard::ApiError error;
    uint8_t attempts = 0;
  };

  explicit InkHoardClient(inkhoard::InkHoardTransport* transport = nullptr);

  void setTransport(inkhoard::InkHoardTransport* transport) { transport_ = transport; }

  /** Auth probe only — does not allocate/parse a LibraryPage (TLS heap budget). */
  Outcome testConnection();

  /**
   * Fetch a library page. Allocates `out` only AFTER the HTTP/TLS exchange so
   * the ~45KB page buffer does not starve the TLS heap gate under Wi‑Fi.
   */
  Outcome fetchLibraryPage(std::unique_ptr<inkhoard::LibraryPage>& out, const char* cursor = nullptr,
                           int limit = 20);
  Outcome search(std::unique_ptr<inkhoard::SearchPage>& out, const char* query, int offset = 0, int limit = 20);
  Outcome fetchItemDetail(inkhoard::CompactItem& out, const char* id);

  /**
   * Stream EPUB to destPath (device build). Host tests should mock at a higher layer.
   * If ifNoneMatch is non-empty, sends If-None-Match. On 304 returns NotModified.
   */
  Outcome downloadEpub(const char* id, const char* destPath, const char* ifNoneMatch = nullptr,
                       char* etagOut = nullptr, size_t etagOutCap = 0, ProgressCallback progress = nullptr);

 private:
  Outcome performJsonGet(const std::string& pathAndQuery, InkHoardJsonParser::Kind kind, void* outStruct);
  std::string buildUrl(const std::string& pathAndQuery) const;

  inkhoard::InkHoardTransport* transport_ = nullptr;
};
