#include "CpFontAdapter.h"

#include <SdCardFont.h>
#include <Utf8.h>

namespace {

// SD-backed families must answer layout metrics from resident metadata + the
// persistent advance table. The generic getGlyph() path loads the FULL BITMAP
// through an 8-slot overflow ring on every miss — during a chapter build that
// is one SD read per character occurrence and the build crawls (observed as
// an endless "SDCF Overflow: loaded U+..." stream while paginating).
SdCardFont* sdFontFor(const EpdFontFamily* family, const EpdFontFamily::Style style, uint8_t* styleIdxOut) {
  const EpdFontData* data = family->getData(style);
  if (data == nullptr || data->glyphMissCtx == nullptr) return nullptr;
  *styleIdxOut = SdCardFont::styleIdxFromMissCtx(data->glyphMissCtx);
  return SdCardFont::fromMissCtx(data->glyphMissCtx);
}

}  // namespace

bool CpFontAdapter::addSize(const uint16_t sizePx, const EpdFontFamily* family) {
  if (family == nullptr || count_ >= kMaxLadder) return false;
  if (count_ > 0 && sizePx <= ladder_[count_ - 1].sizePx) return false;  // ascending only
  ladder_[count_++] = {sizePx, family};
  return true;
}

uint16_t CpFontAdapter::quantize(const uint16_t sizePx) const {
  if (count_ == 0) return sizePx;
  const Rung* best = &ladder_[0];
  for (uint8_t i = 1; i < count_; ++i) {
    const int cur = ladder_[i].sizePx > sizePx ? ladder_[i].sizePx - sizePx : sizePx - ladder_[i].sizePx;
    const int prev = best->sizePx > sizePx ? best->sizePx - sizePx : sizePx - best->sizePx;
    if (cur < prev) best = &ladder_[i];  // ties keep the smaller rung
  }
  return best->sizePx;
}

const EpdFontFamily* CpFontAdapter::familyFor(const uint16_t sizePx) const {
  if (count_ == 0) return nullptr;
  const uint16_t q = quantize(sizePx);
  for (uint8_t i = 0; i < count_; ++i) {
    if (ladder_[i].sizePx == q) return ladder_[i].family;
  }
  return ladder_[0].family;
}

int16_t CpFontAdapter::advance(const uint32_t codepoint, const uint16_t sizePx, uint8_t) {
  // GfxRenderer does not advance the cursor for combining marks (they center
  // over the previous base glyph); report the same.
  if (utf8IsCombiningMark(codepoint)) return 0;
  const EpdFontFamily* family = familyFor(sizePx);
  if (family == nullptr) return 0;
  uint8_t sdStyle = 0;
  if (SdCardFont* sd = sdFontFor(family, style_, &sdStyle)) {
    return static_cast<int16_t>(fp4::toPixel(sd->ensureAdvance(sdStyle, codepoint)));
  }
  const EpdGlyph* glyph = family->getGlyph(codepoint, style_);
  return glyph != nullptr ? static_cast<int16_t>(fp4::toPixel(glyph->advanceX)) : 0;
}

int16_t CpFontAdapter::lineHeight(const uint16_t sizePx) {
  // advanceY is the newline distance GfxRenderer::getLineHeight() reports —
  // the same figure the legacy reader spaced lines with (CrossPoint parity).
  const EpdFontFamily* family = familyFor(sizePx);
  return family != nullptr ? static_cast<int16_t>(family->getData(style_)->advanceY) : 0;
}

int16_t CpFontAdapter::ascent(const uint16_t sizePx) {
  const EpdFontFamily* family = familyFor(sizePx);
  return family != nullptr ? static_cast<int16_t>(family->getData(style_)->ascender) : 0;
}

int16_t CpFontAdapter::kerning(const uint32_t left, const uint32_t right, const uint16_t sizePx, uint8_t) {
  if (utf8IsCombiningMark(left) || utf8IsCombiningMark(right)) return 0;
  const EpdFontFamily* family = familyFor(sizePx);
  if (family == nullptr) return 0;
  int32_t advFP = 0;  // 12.4 fixed-point
  uint8_t sdStyle = 0;
  if (SdCardFont* sd = sdFontFor(family, style_, &sdStyle)) {
    advFP = sd->ensureAdvance(sdStyle, left);
    if (advFP == 0) return 0;
  } else {
    const EpdGlyph* leftGlyph = family->getGlyph(left, style_);
    if (leftGlyph == nullptr) return 0;
    advFP = leftGlyph->advanceX;
  }
  const int32_t kernFP = family->getKerning(left, right, style_);  // 4.4 fixed-point
  // Differential-rounding parity: layout adds advance(left) + kerning(left,
  // right); returning the delta between the renderer's fused snap and the
  // advance's own snap makes the sum equal the renderer's cursor step.
  return static_cast<int16_t>(fp4::toPixel(advFP + kernFP) - fp4::toPixel(advFP));
}

uint32_t CpFontAdapter::ligature(const uint32_t left, const uint32_t right, uint8_t) {
  // Size-independent in cpfonts; use the largest rung's table.
  if (count_ == 0) return 0;
  return ladder_[count_ - 1].family->getLigature(left, right, style_);
}

bool CpFontAdapter::hasGlyph(const uint32_t codepoint) const {
  if (count_ == 0) return false;
  const EpdFontFamily* family = ladder_[count_ - 1].family;
  uint8_t sdStyle = 0;
  if (SdCardFont* sd = sdFontFor(family, style_, &sdStyle)) {
    return sd->hasGlyphMeta(sdStyle, codepoint);
  }
  return family->hasGlyph(codepoint, style_);
}

const freeink::book::GlyphBitmap* CpFontAdapter::rasterize(uint32_t, uint16_t) {
  // CrossPoint draws page records with GfxRenderer (glyph groups, SD overflow
  // fetch, 2-bit AA all stay in the renderer); the engine's PageRenderer is
  // not used, so nothing ever rasterizes through this adapter.
  return nullptr;
}
