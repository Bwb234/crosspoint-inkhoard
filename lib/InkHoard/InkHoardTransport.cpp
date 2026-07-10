#include "InkHoardTransport.h"

#include <cstring>

namespace inkhoard {

ClientResult mapHttpStatus(int httpCode, const char* errorCode) {
  if (httpCode == 200) return ClientResult::Ok;
  if (httpCode == 304) return ClientResult::NotModified;
  if (httpCode == 401) return ClientResult::AuthInvalid;
  if (httpCode == 403) return ClientResult::AuthForbidden;
  if (httpCode == 404) return ClientResult::NotFound;
  if (httpCode == 409) return ClientResult::ContentNotReady;
  if (httpCode == 422) return ClientResult::ContentFailed;
  if (httpCode == 400) return ClientResult::BadRequest;
  if (httpCode >= 500 && httpCode <= 599) return ClientResult::ServerError;

  // Prefer machine code when status is ambiguous
  if (errorCode && errorCode[0]) {
    if (std::strcmp(errorCode, "unauthorized") == 0) return ClientResult::AuthInvalid;
    if (std::strcmp(errorCode, "forbidden") == 0) return ClientResult::AuthForbidden;
    if (std::strcmp(errorCode, "not_found") == 0) return ClientResult::NotFound;
    if (std::strcmp(errorCode, "content_not_ready") == 0) return ClientResult::ContentNotReady;
    if (std::strcmp(errorCode, "content_failed") == 0) return ClientResult::ContentFailed;
    if (std::strcmp(errorCode, "bad_request") == 0) return ClientResult::BadRequest;
    if (std::strcmp(errorCode, "server_error") == 0) return ClientResult::ServerError;
  }

  if (httpCode > 0) return ClientResult::ServerError;
  return ClientResult::TransportError;
}

bool shouldRetry(ClientResult r) {
  return r == ClientResult::TransportError || r == ClientResult::TlsError || r == ClientResult::ServerError;
}

}  // namespace inkhoard
