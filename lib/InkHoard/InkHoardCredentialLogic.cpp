#include "InkHoardCredentialLogic.h"

namespace inkhoard {
namespace {

bool isContinuationByte(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

size_t utf8ByteLength(const std::string& value) { return value.size(); }

std::string truncateUtf8(const std::string& value, size_t maxBytes) {
  if (value.size() <= maxBytes) {
    return value;
  }
  size_t end = maxBytes;
  while (end > 0 && isContinuationByte(static_cast<unsigned char>(value[end]))) {
    --end;
  }
  return value.substr(0, end);
}

bool normalizeServerUrl(const std::string& input, bool allowHttp, std::string& outUrl) {
  std::string url = input;
  // Trim ASCII whitespace
  while (!url.empty() && (url.front() == ' ' || url.front() == '\t')) {
    url.erase(url.begin());
  }
  while (!url.empty() && (url.back() == ' ' || url.back() == '\t')) {
    url.pop_back();
  }
  if (url.empty() || url == "https://" || url == "http://") {
    outUrl.clear();
    return true;
  }

  const bool hasScheme = url.find("://") != std::string::npos;
  if (!hasScheme) {
    url = "https://" + url;
  } else if (url.rfind("http://", 0) == 0 && !allowHttp) {
    return false;
  } else if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
    return false;
  }

  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }

  if (utf8ByteLength(url) > MAX_URL_BYTES) {
    return false;
  }

  outUrl = url;
  return true;
}

std::string redactedToken(const std::string& token) {
  if (token.empty()) {
    return "";
  }
  if (token.size() <= 4) {
    return "…";
  }
  return "ink_dev_…" + token.substr(token.size() - 4);
}

}  // namespace inkhoard
