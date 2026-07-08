#pragma once

// CpFontAdapter — one .cpfont style (regular/bold/italic/bold-italic) exposed
// through FreeInkBook's RenderFont interface, so the engine lays out with the
// exact metrics GfxRenderer will draw with.
//
// .cpfonts exist on a fixed size ladder (12/14/16/18 px). The engine requests
// arbitrary sizePx (headings scale the base size); every request is quantized
// to the nearest ladder entry identically in ALL metrics, and the render path
// quantizes the same way when picking a font id, so measurement and drawing
// can never disagree about which font a run uses.
//
// Width parity is exact by construction: GfxRenderer advances the cursor with
// differential fixed-point rounding — each step is toPixel(advFP(prev) +
// kernFP(prev, cur)). This adapter reports advance(cp) = toPixel(advFP(cp))
// and kerning(l, r) = toPixel(advFP(l) + kernFP(l, r)) - toPixel(advFP(l));
// layout's per-glyph sum then telescopes to precisely the renderer's total.
// (One knowingly accepted divergence: the renderer kerns across combining
// marks against the base letter; layout sees the mark as `prev` and skips
// that kern — sub-pixel, and only on mark-bearing text.)
//
// Metrics-only and host-compilable (EpdFont + Utf8 + FreeInkBook headers).
// rasterize() intentionally returns nullptr: CrossPoint renders page records
// through GfxRenderer's own glyph pipeline (see the migration renderer
// contract), never through the engine's PageRenderer.

#include <BookFont.h>
#include <EpdFontFamily.h>

class CpFontAdapter : public freeink::book::RenderFont {
 public:
  static constexpr uint8_t kMaxLadder = 6;

  explicit CpFontAdapter(EpdFontFamily::Style style = EpdFontFamily::REGULAR) : style_(style) {}

  // Register one ladder rung. Call in ascending sizePx order.
  bool addSize(uint16_t sizePx, const EpdFontFamily* family);

  // The ladder size a request resolves to (also used by the render path to
  // pick the matching font id — keep the two in lockstep).
  uint16_t quantize(uint16_t sizePx) const;

  // freeink::book::RenderFont
  int16_t advance(uint32_t codepoint, uint16_t sizePx, uint8_t styleFlags) override;
  int16_t lineHeight(uint16_t sizePx) override;
  int16_t ascent(uint16_t sizePx) override;
  int16_t kerning(uint32_t left, uint32_t right, uint16_t sizePx, uint8_t styleFlags) override;
  uint32_t ligature(uint32_t left, uint32_t right, uint8_t styleFlags) override;
  bool hasGlyph(uint32_t codepoint) const override;
  const freeink::book::GlyphBitmap* rasterize(uint32_t codepoint, uint16_t sizePx) override;

 private:
  const EpdFontFamily* familyFor(uint16_t sizePx) const;

  struct Rung {
    uint16_t sizePx;
    const EpdFontFamily* family;
  };
  Rung ladder_[kMaxLadder] = {};
  uint8_t count_ = 0;
  EpdFontFamily::Style style_;
};
