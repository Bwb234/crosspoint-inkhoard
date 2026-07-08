// Link stubs for the adapter's SD-backed metric path. The suite's test
// families have no glyphMissCtx, so CpFontAdapter never calls into SdCardFont
// here — but the symbols must resolve, and SdCardFont.cpp itself needs
// Arduino/SD and cannot build on host.
#include <SdCardFont.h>

bool SdCardFont::hasGlyphMeta(uint8_t, uint32_t) const { return false; }
uint16_t SdCardFont::ensureAdvance(uint8_t, uint32_t) { return 0; }
uint8_t SdCardFont::styleIdxFromMissCtx(void*) { return 0; }
SdCardFont* SdCardFont::fromMissCtx(void*) { return nullptr; }
