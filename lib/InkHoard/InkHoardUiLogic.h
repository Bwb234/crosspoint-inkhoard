#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "InkHoardModels.h"

/**
 * Pure UI helpers for InkHoard library screens (plan 009). Host-testable.
 */
namespace inkhoard {

/** Cursor stack: page 0 has empty cursor; push nextCursor when advancing. */
class CursorStack {
 public:
  void reset() {
    cursors_.clear();
    cursors_.emplace_back();  // page 0
  }

  size_t pageIndex() const { return cursors_.empty() ? 0 : cursors_.size() - 1; }

  const char* currentCursor() const {
    if (cursors_.empty() || cursors_.back().empty()) return nullptr;
    return cursors_.back().c_str();
  }

  bool canGoPrev() const { return cursors_.size() > 1; }

  void pushNext(const char* nextCursor) {
    if (!nextCursor || !nextCursor[0]) return;
    cursors_.emplace_back(nextCursor);
  }

  bool popPrev() {
    if (cursors_.size() <= 1) return false;
    cursors_.pop_back();
    return true;
  }

 private:
  std::vector<std::string> cursors_;
};

/** Map client result to a coarse UI state for library/item screens. */
enum class UiFailureKind : uint8_t {
  None = 0,
  NoCredentials,
  Auth,
  Forbidden,
  Transport,
  Server,
  Content,
  SdError,  // avoid colliding with HalStorage's Storage macro
  Other,
};

inline UiFailureKind mapClientToUiFailure(ClientResult r) {
  switch (r) {
    case ClientResult::Ok:
    case ClientResult::NotModified:
      return UiFailureKind::None;
    case ClientResult::NoCredentials:
      return UiFailureKind::NoCredentials;
    case ClientResult::AuthInvalid:
      return UiFailureKind::Auth;
    case ClientResult::AuthForbidden:
      return UiFailureKind::Forbidden;
    case ClientResult::TransportError:
    case ClientResult::TlsError:
    case ClientResult::LowMemory:
    case ClientResult::OversizeResponse:
    case ClientResult::ParseError:
    case ClientResult::Aborted:
      return UiFailureKind::Transport;
    case ClientResult::ServerError:
      return UiFailureKind::Server;
    case ClientResult::ContentNotReady:
    case ClientResult::ContentFailed:
    case ClientResult::NotFound:
    case ClientResult::BadRequest:
      return UiFailureKind::Content;
    case ClientResult::FileError:
      return UiFailureKind::SdError;
    default:
      return UiFailureKind::Other;
  }
}

}  // namespace inkhoard
