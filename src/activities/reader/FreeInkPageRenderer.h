#pragma once

// Draws FreeInkBook page records through GfxRenderer — the migration's
// renderer contract: each run's UTF-8 text at (x, baselineY) with the font
// for (sizePx, styleFlags) using that font's own advances/kerning, a line
// under StyleUnderline runs, image rects, and link collection for the
// footnote UI. Text is never reordered, shaped, or spaced here: runs arrive
// in visual order with justification baked into their x positions
// (drawText is called with BidiBaseDir::NONE).
//
// Images decode once per (href, placement) into a 2-bit cache file beside
// the page caches, then every render pass — BW and both grayscale planes,
// including per-band strip re-renders — streams that file instead of
// re-decoding the PNG/JPEG.

#include <Epub/FootnoteEntry.h>
#include <layout/ChapterLayout.h>

#include <string>
#include <vector>

class GfxRenderer;
class BookPaginator;

namespace FreeInkPageRenderer {

// Draws the page's text runs and (unless SETTINGS disables images or the
// renderer is in a font-cache scan pass) its images, honoring the renderer's
// current render mode (BW / GRAYSCALE_LSB / GRAYSCALE_MSB).
void drawPage(GfxRenderer& renderer, BookPaginator& paginator, const freeink::book::Page& page,
              const std::string& cacheDir);

// True when the page places at least one image.
inline bool hasImages(const freeink::book::Page& page) { return page.imageCount > 0; }

// Union of all image rects (for the blank-then-refresh e-ink technique).
bool imageBoundingBox(const freeink::book::Page& page, int16_t* x, int16_t* y, int16_t* w, int16_t* h);

// The page's tappable links as footnote entries (href = target#fragment).
std::vector<FootnoteEntry> collectFootnotes(const freeink::book::Page& page);

// Concatenated run text (QR display, bookmark summaries).
std::string pageText(const freeink::book::Page& page);

}  // namespace FreeInkPageRenderer
