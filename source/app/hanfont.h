#pragma once

#include <cstdint>

// The glyphs 3dsDL draws itself, baked from Noto Sans CJK SC so Chinese shows
// on a console whose shared font has none.
namespace hanfont {

struct Glyph {
    uint32_t codePoint;
    int column;
    int row;
    int advance;
};

extern const int Cell;
extern const int Columns;
extern const int AtlasWidth;
extern const int AtlasHeight;
extern const int Em;
extern const int LineFeed;
extern const Glyph Glyphs[];
extern const int Count;

// The glyph for a code point, or nullptr when this font does not carry it.
const Glyph *find(uint32_t codePoint);

} // namespace hanfont
