#pragma once

#include <cstddef>
#include <string>

/**
 * Pure helpers for InkHoard credentials (host-testable; no Arduino deps).
 * INKHOARD: plan 007
 */
namespace inkhoard {

constexpr size_t MAX_URL_BYTES = 256;
constexpr size_t MAX_TOKEN_BYTES = 128;
constexpr size_t MAX_DISPLAY_NAME_BYTES = 64;

/** UTF-8 byte length. */
size_t utf8ByteLength(const std::string& value);

/** Truncate to ≤ maxBytes on a code-point boundary. */
std::string truncateUtf8(const std::string& value, size_t maxBytes);

/**
 * Normalize a typed server URL.
 * - Empty → empty
 * - No scheme → prepend https://
 * - Trailing slashes stripped
 * - http:// rejected when allowHttp is false (release builds)
 * Returns false if rejected; outUrl unchanged on failure.
 */
bool normalizeServerUrl(const std::string& input, bool allowHttp, std::string& outUrl);

/** ink_dev_…abcd (last 4 chars) or empty if no token. Never returns the full token. */
std::string redactedToken(const std::string& token);

}  // namespace inkhoard
