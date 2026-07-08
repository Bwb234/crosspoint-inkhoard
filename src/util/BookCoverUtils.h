#pragma once

// EPUB cover artwork for UI chrome (home screen thumbnails, sleep screen),
// on the FreeInkBook container: the cover manifest item (EPUB 3
// properties="cover-image" or EPUB 2 <meta name="cover">) is streamed out of
// the ZIP into a temp file and fed to the existing PNG/JPEG-to-BMP
// converters, so the generated files are byte-compatible with what the
// legacy Epub pipeline produced (same cache paths, same formats).

#include <cstdint>
#include <string>

namespace BookCoverUtils {

// "<cacheDir>/cover.bmp" or "<cacheDir>/cover_crop.bmp" (legacy naming).
std::string coverBmpPath(const std::string& epubPath, bool cropped);
// "<cacheDir>/thumb_<height>.bmp" (legacy naming).
std::string thumbBmpPath(const std::string& epubPath, int height);
// The [HEIGHT]-templated form RecentBooksStore carries.
std::string thumbBmpPathTemplate(const std::string& epubPath);

// Screen-sized cover for the sleep screen. No-op when the file exists.
bool generateCoverBmp(const std::string& epubPath, bool cropped);

// 1-bit thumbnail for the home screen (fast BW blit). No-op when a real
// thumbnail exists. A provably coverless book writes a 1-byte sentinel so it
// is not re-probed every visit; a legacy/stale ZERO-byte sentinel (left by a
// transient failure) is healed by retrying.
bool generateThumbBmp(const std::string& epubPath, int height);

// True when a generation attempt is warranted for the resolved thumb path:
// the file is missing OR is a stale zero-byte sentinel. A 1-byte coverless
// sentinel and any real BMP both return false. Callers gate the loading
// popup (and the per-book container open) on this.
bool thumbAttemptNeeded(const std::string& thumbPath);

// Book title/author straight from the OPF (for recents entries).
bool readMetadata(const std::string& epubPath, std::string* titleOut, std::string* authorOut);

}  // namespace BookCoverUtils
