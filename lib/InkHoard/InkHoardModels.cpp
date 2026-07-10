#include "InkHoardModels.h"

namespace inkhoard {

const char* clientResultLabel(ClientResult r) {
  switch (r) {
    case ClientResult::Ok:
      return "ok";
    case ClientResult::AuthInvalid:
      return "auth_invalid";
    case ClientResult::AuthForbidden:
      return "auth_forbidden";
    case ClientResult::NotFound:
      return "not_found";
    case ClientResult::ContentNotReady:
      return "content_not_ready";
    case ClientResult::ContentFailed:
      return "content_failed";
    case ClientResult::BadRequest:
      return "bad_request";
    case ClientResult::ServerError:
      return "server_error";
    case ClientResult::NotModified:
      return "not_modified";
    case ClientResult::TransportError:
      return "transport_error";
    case ClientResult::TlsError:
      return "tls_error";
    case ClientResult::OversizeResponse:
      return "oversize_response";
    case ClientResult::ParseError:
      return "parse_error";
    case ClientResult::NoCredentials:
      return "no_credentials";
    case ClientResult::LowMemory:
      return "low_memory";
    case ClientResult::FileError:
      return "file_error";
    case ClientResult::Aborted:
      return "aborted";
  }
  return "unknown";
}

}  // namespace inkhoard
