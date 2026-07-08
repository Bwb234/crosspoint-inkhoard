#include <gtest/gtest.h>

#include <cstdint>

#include "CpFontAdapter.h"
#include "lib/EpdFont/EpdFont.h"
#include "lib/EpdFont/EpdFontData.h"
#include "lib/EpdFont/EpdFontFamily.h"
#include "lib/Utf8/Utf8.h"

// ============================================================================
// Synthetic fonts (same convention as DifferentialRoundingTest): metrics
// chosen so per-glyph rounding and differential rounding disagree unless the
// adapter's kerning-delta trick is applied.
//
// Glyphs: 'T' (0x54), 'a' (0x61), 'o' (0x6F), 'x' (0x78), U+FFFD.
// Kern pairs (4.4 FP): T->a -5, T->o -7, o->a -2, o->o -3.
// Ligature: 'T'+'x' -> U+E000 (private use; also a glyph in the font).
// ============================================================================

namespace {

// clang-format off
const EpdGlyph kGlyphs16[] = {
  /* 0 'T'      */ { 8, 12, 137, 0, 12, 0, 0 },
  /* 1 'a'      */ { 7,  8, 130, 0,  8, 0, 0 },
  /* 2 'o'      */ { 8,  8, 145, 0,  8, 0, 0 },
  /* 3 'x'      */ { 7,  8, 136, 0,  8, 0, 0 },
  /* 4 U+E000   */ { 12, 12, 200, 0, 12, 0, 0 },
  /* 5 U+FFFD   */ { 9, 12, 160, 0, 12, 0, 0 },
};

const EpdUnicodeInterval kIntervals16[] = {
  { 0x54,   0x54,   0 },
  { 0x61,   0x61,   1 },
  { 0x6F,   0x6F,   2 },
  { 0x78,   0x78,   3 },
  { 0xE000, 0xE000, 4 },
  { 0xFFFD, 0xFFFD, 5 },
};

const EpdKernClassEntry kKernLeft[]  = { { 0x54, 1 }, { 0x6F, 2 } };
const EpdKernClassEntry kKernRight[] = { { 0x61, 1 }, { 0x6F, 2 } };
const int8_t kKernMatrix[] = { -5, -7, -2, -3 };

const EpdLigaturePair kLigatures[] = {
  { (0x54u << 16) | 0x78u, 0xE000 },  // 'T' + 'x'
};
// clang-format on

EpdFontData makeFontData(const EpdGlyph* glyphs, const EpdUnicodeInterval* intervals,
                         uint32_t intervalCount, uint8_t advanceY, int ascender, int descender) {
  EpdFontData d{};
  d.bitmap = nullptr;
  d.glyph = glyphs;
  d.intervals = intervals;
  d.intervalCount = intervalCount;
  d.advanceY = advanceY;
  d.ascender = ascender;
  d.descender = descender;
  d.kernLeftClasses = kKernLeft;
  d.kernRightClasses = kKernRight;
  d.kernMatrix = kKernMatrix;
  d.kernLeftEntryCount = 2;
  d.kernRightEntryCount = 2;
  d.kernLeftClassCount = 2;
  d.kernRightClassCount = 2;
  d.ligaturePairs = kLigatures;
  d.ligaturePairCount = 1;
  return d;
}

// A second, wider font standing in for the 12 px ladder rung so quantization
// is observable ('T' advance differs).
const EpdGlyph kGlyphs12[] = {
    /* 0 'T' */ {6, 9, 96, 0, 9, 0, 0},  // 6.0 px exactly
};
const EpdUnicodeInterval kIntervals12[] = {{0x54, 0x54, 0}};

// The reference: GfxRenderer's differential-rounding cursor walk
// (drawText/getTextWidth in GfxRenderer.cpp) — each step snaps
// (prev advance FP + kern FP) as one unit.
int rendererWidth(const EpdFontFamily& family, const uint32_t* cps, int n,
                  EpdFontFamily::Style style) {
  int width = 0;
  int32_t prevAdvanceFP = 0;
  uint32_t prevCp = 0;
  for (int i = 0; i < n; ++i) {
    const uint32_t cp = cps[i];
    if (prevCp != 0) {
      width += fp4::toPixel(prevAdvanceFP + family.getKerning(prevCp, cp, style));
    }
    const EpdGlyph* glyph = family.getGlyph(cp, style);
    prevAdvanceFP = glyph ? glyph->advanceX : 0;
    prevCp = cp;
  }
  width += fp4::toPixel(prevAdvanceFP);
  return width;
}

// The engine's accounting: advance(cp) plus kerning(prev, cp) per glyph
// (ChapterLayout::advanceFor).
int layoutWidth(CpFontAdapter& font, const uint32_t* cps, int n, uint16_t sizePx) {
  int width = 0;
  uint32_t prevCp = 0;
  for (int i = 0; i < n; ++i) {
    width += font.advance(cps[i], sizePx, 0);
    if (prevCp != 0) width += font.kerning(prevCp, cps[i], sizePx, 0);
    prevCp = cps[i];
  }
  return width;
}

class CpFontAdapterTest : public ::testing::Test {
 protected:
  CpFontAdapterTest()
      : data16_(makeFontData(kGlyphs16, kIntervals16, 6, 20, 14, -5)),
        data12_(makeFontData(kGlyphs12, kIntervals12, 1, 15, 11, -4)),
        font16_(&data16_),
        font12_(&data12_),
        family16_(&font16_),
        family12_(&font12_) {
    adapter_.addSize(12, &family12_);
    adapter_.addSize(16, &family16_);
  }

  EpdFontData data16_, data12_;
  EpdFont font16_, font12_;
  EpdFontFamily family16_, family12_;
  CpFontAdapter adapter_;
};

TEST_F(CpFontAdapterTest, AdvanceSnapsFixedPointPerGlyph) {
  // 137 FP -> 9 px, 130 -> 8, 145 -> 9, 136 -> 9 (round-half-up at 8/16).
  EXPECT_EQ(adapter_.advance('T', 16, 0), 9);
  EXPECT_EQ(adapter_.advance('a', 16, 0), 8);
  EXPECT_EQ(adapter_.advance('o', 16, 0), 9);
  EXPECT_EQ(adapter_.advance('x', 16, 0), 9);
}

TEST_F(CpFontAdapterTest, KerningDeltaMatchesRendererDifferentialRounding) {
  // Every string the renderer can draw must measure to the renderer's own
  // differentially-rounded width — the telescoping identity the adapter's
  // kerning() implements. "oo" (advance frac 1, kern -3) is the case where
  // naive per-glyph rounding diverges.
  const uint32_t oo[] = {'o', 'o'};
  const uint32_t tao[] = {'T', 'a', 'o'};
  const uint32_t tooao[] = {'T', 'o', 'o', 'a', 'o'};
  EXPECT_EQ(layoutWidth(adapter_, oo, 2, 16), rendererWidth(family16_, oo, 2, EpdFontFamily::REGULAR));
  EXPECT_EQ(layoutWidth(adapter_, tao, 3, 16), rendererWidth(family16_, tao, 3, EpdFontFamily::REGULAR));
  EXPECT_EQ(layoutWidth(adapter_, tooao, 5, 16),
            rendererWidth(family16_, tooao, 5, EpdFontFamily::REGULAR));
}

TEST_F(CpFontAdapterTest, KerningPairValues) {
  // kerning(T,o) = toPixel(137 + (-7)) - toPixel(137) = 8 - 9 = -1.
  EXPECT_EQ(adapter_.kerning('T', 'o', 16, 0), -1);
  // kerning(o,o) = toPixel(145 - 3) - toPixel(145) = 9 - 9 = 0.
  EXPECT_EQ(adapter_.kerning('o', 'o', 16, 0), 0);
  // No kern class pair -> 0.
  EXPECT_EQ(adapter_.kerning('a', 'x', 16, 0), 0);
}

TEST_F(CpFontAdapterTest, QuantizesToNearestRungInMetricsToo) {
  EXPECT_EQ(adapter_.quantize(12), 12);
  EXPECT_EQ(adapter_.quantize(13), 12);
  EXPECT_EQ(adapter_.quantize(14), 12);  // tie keeps the smaller rung
  EXPECT_EQ(adapter_.quantize(15), 16);
  EXPECT_EQ(adapter_.quantize(24), 16);  // above the ladder clamps to the top
  // 'T' at 12 px comes from the 12 px font (96 FP -> 6 px), not a scale.
  EXPECT_EQ(adapter_.advance('T', 12, 0), 6);
  EXPECT_EQ(adapter_.advance('T', 13, 0), 6);
  EXPECT_EQ(adapter_.advance('T', 24, 0), 9);
  EXPECT_EQ(adapter_.lineHeight(13), 15);
  EXPECT_EQ(adapter_.lineHeight(24), 20);
  EXPECT_EQ(adapter_.ascent(13), 11);
  EXPECT_EQ(adapter_.ascent(24), 14);
}

TEST_F(CpFontAdapterTest, HasGlyphIsNotFooledByReplacementFallback) {
  EXPECT_TRUE(adapter_.hasGlyph('T'));
  EXPECT_TRUE(adapter_.hasGlyph(0xFFFD));
  // 'q' is missing; getGlyph would return U+FFFD, hasGlyph must say no.
  EXPECT_FALSE(adapter_.hasGlyph('q'));
  EXPECT_NE(family16_.getGlyph('q', EpdFontFamily::REGULAR), nullptr);
}

TEST_F(CpFontAdapterTest, LigatureLookup) {
  EXPECT_EQ(adapter_.ligature('T', 'x', 0), 0xE000u);
  EXPECT_EQ(adapter_.ligature('T', 'a', 0), 0u);
}

TEST_F(CpFontAdapterTest, CombiningMarksDoNotAdvance) {
  // U+0301 combining acute: the renderer centers it over the base glyph and
  // moves the cursor zero pixels; layout must agree.
  EXPECT_EQ(adapter_.advance(0x0301, 16, 0), 0);
  EXPECT_EQ(adapter_.kerning('a', 0x0301, 16, 0), 0);
}

TEST_F(CpFontAdapterTest, MissingGlyphAdvanceMatchesRendererReplacementDraw) {
  // The renderer draws U+FFFD for a missing codepoint; layout must reserve
  // the replacement glyph's width (160 FP -> 10 px), not zero.
  EXPECT_EQ(adapter_.advance('q', 16, 0), 10);
}

}  // namespace
