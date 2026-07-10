#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "InkHoardModels.h"

/**
 * Transport seam for InkHoardClient (mockable in host tests).
 * INKHOARD: plan 008
 */
namespace inkhoard {

struct TransportResponse {
  int httpCode = 0;
  bool ok = false;              // transport completed (may still be non-2xx)
  bool tlsFailure = false;
  bool transportFailure = false;
  bool lowMemory = false;
  bool oversize = false;
  std::string body;             // capped by caller / transport
  char etag[256] = {};          // optional response ETag
  size_t contentLength = 0;     // 0 if unknown
};

class InkHoardTransport {
 public:
  virtual ~InkHoardTransport() = default;

  /** GET with Authorization: Bearer. Never log the token. */
  virtual TransportResponse getJson(const std::string& url, const std::string& bearerToken) = 0;
};

/** Map HTTP status (+ optional error code string) to ClientResult. Host-testable. */
ClientResult mapHttpStatus(int httpCode, const char* errorCode = nullptr);

/** True if this result should be retried (transport/TLS/5xx). */
bool shouldRetry(ClientResult r);

}  // namespace inkhoard
