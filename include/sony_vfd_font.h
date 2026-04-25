// sony_vfd_font.h — 16-segment "Union Jack" ASCII glyph table for PT6315 glass.
//
// Each entry is a 16-bit mask: bit N (1..16) set means segment SGN is lit.
// Bit 0 is the decimal point. The physical layout is:
/*
                 SG16 (top horizontal)
               +---------+---------+
            SG11 \   SG13|SG14   / SG12
                   \     |     /
                    \    |    /
              SG8 --------+-------- SG10     (SG9 = middle pip, unused)
                    /    |    \
                   /     |     \
            SG6  /   SG3 SG4 SG5  \ SG7
               +---------+---------+
                 SG2 (bottom horizontal)
                                    . SG1 (decimal point)
*/
// Ported/adapted from the BasicText SonyVFD font table. Only 0x20..0x5F
// (space through uppercase Z) are mapped; lowercase and non-ASCII fall
// back to space.

#pragma once

#include <cstdint>

// Convert an ASCII character to its 16-segment bitmask (dot not included).
// Bit (N-1) of the return value corresponds to segment SGN.
uint16_t sonyVfdFontGlyph(char c);

// Same as above but with the decimal-point segment (SG1) set.
uint16_t sonyVfdFontGlyphWithDot(char c);
