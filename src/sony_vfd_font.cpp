// sony_vfd_font.cpp — 16-segment font table, ported verbatim from the
// Arduino SonyVFDFont.h. The bit assignments match the reverse-engineered
// glass map documented in the Sony VFD integration README.

#include "sony_vfd_font.h"

#include <cstdint>

namespace {

// Segment bit helper. SG1 is the LSB (bit 0); SG16 is bit 15.
constexpr uint16_t SG(int n) { return static_cast<uint16_t>(1u << (n - 1)); }

// Named segments, keeping the font table readable.
constexpr uint16_t S_DOT = SG(1);
constexpr uint16_t S_BOT = SG(2);
constexpr uint16_t S_BL  = SG(3);
constexpr uint16_t S_BV  = SG(4);
constexpr uint16_t S_BR  = SG(5);
constexpr uint16_t S_LV  = SG(6);
constexpr uint16_t S_RV  = SG(7);
constexpr uint16_t S_ML  = SG(8);
constexpr uint16_t S_MC  = SG(9);
constexpr uint16_t S_MR  = SG(10);
constexpr uint16_t S_LV2 = SG(11);
constexpr uint16_t S_RV2 = SG(12);
constexpr uint16_t S_TL  = SG(13);
constexpr uint16_t S_TV  = SG(14);
constexpr uint16_t S_TR  = SG(15);
constexpr uint16_t S_TOP = SG(16);

constexpr uint16_t S_LEFT  = S_LV  | S_LV2;
constexpr uint16_t S_RIGHT = S_RV  | S_RV2;
constexpr uint16_t S_MID   = S_ML  | S_MC  | S_MR;

constexpr uint16_t FONT[] = {
    /* ' ' */ 0,
    /* '!' */ S_RV | S_RV2,
    /* '"' */ S_LV2 | S_RV2,
    /* '#' */ S_BV | S_TV | S_MID,
    /* '$' */ S_TOP | S_LV2 | S_MID | S_RV | S_BOT | S_BV | S_TV,
    /* '%' */ S_TL | S_BR | S_MID | S_LV2 | S_RV,
    /* '&' */ S_TOP | S_LV2 | S_ML | S_BL | S_BR | S_TR | S_RV,
    /* ''' */ S_RV2,
    /* '(' */ S_TR | S_BR,
    /* ')' */ S_TL | S_BL,
    /* '*' */ S_TL | S_TV | S_TR | S_BL | S_BV | S_BR | S_MID,
    /* '+' */ S_TV | S_BV | S_MID,
    /* ',' */ S_BL,
    /* '-' */ S_MID,
    /* '.' */ S_DOT,
    /* '/' */ S_TR | S_BL,
    /* '0' */ S_TOP | S_BOT | S_LEFT | S_RIGHT | S_TR | S_BL,
    /* '1' */ S_RIGHT | S_TR,
    /* '2' */ S_TOP | S_RV2 | S_MID | S_LV | S_BOT,
    /* '3' */ S_TOP | S_RIGHT | S_MC | S_ML | S_BOT,
    /* '4' */ S_LV2 | S_RIGHT | S_MID,
    /* '5' */ S_TOP | S_LV2 | S_ML | S_MC | S_RV | S_BOT,
    /* '6' */ S_TOP | S_LEFT | S_ML | S_MC | S_RV | S_BOT,
    /* '7' */ S_TOP | S_RIGHT,
    /* '8' */ S_TOP | S_BOT | S_LEFT | S_RIGHT | S_MID,
    /* '9' */ S_TOP | S_LV2 | S_MID | S_RIGHT | S_BOT,
    /* ':' */ S_TV | S_BV,
    /* ';' */ S_TV | S_BL,
    /* '<' */ S_TR | S_BR,
    /* '=' */ S_MID | S_BOT,
    /* '>' */ S_TL | S_BL,
    /* '?' */ S_TOP | S_RV2 | S_MC | S_BV,
    /* '@' */ S_TOP | S_LV2 | S_ML | S_TV | S_RIGHT | S_BOT,
    /* 'A' */ S_TOP | S_LEFT | S_RIGHT | S_MID,
    /* 'B' */ S_TOP | S_RIGHT | S_MC | S_MR | S_BOT | S_TV | S_BV,
    /* 'C' */ S_TOP | S_LEFT | S_BOT,
    /* 'D' */ S_TOP | S_RIGHT | S_BOT | S_TV | S_BV,
    /* 'E' */ S_TOP | S_LEFT | S_ML | S_MC | S_BOT,
    /* 'F' */ S_TOP | S_LEFT | S_ML | S_MC,
    /* 'G' */ S_TOP | S_LEFT | S_MC | S_MR | S_RV | S_BOT,
    /* 'H' */ S_LEFT | S_RIGHT | S_MID,
    /* 'I' */ S_TOP | S_BOT | S_TV | S_BV,
    /* 'J' */ S_RIGHT | S_BOT | S_LV,
    /* 'K' */ S_LEFT | S_ML | S_TR | S_BR,
    /* 'L' */ S_LEFT | S_BOT,
    /* 'M' */ S_LEFT | S_RIGHT | S_TL | S_TR,
    /* 'N' */ S_LEFT | S_RIGHT | S_TL | S_BR,
    /* 'O' */ S_TOP | S_LEFT | S_RIGHT | S_BOT,
    /* 'P' */ S_TOP | S_LEFT | S_RV2 | S_ML | S_MC,
    /* 'Q' */ S_TOP | S_LEFT | S_RIGHT | S_BOT | S_BR,
    /* 'R' */ S_TOP | S_LEFT | S_RV2 | S_ML | S_MC | S_BR,
    /* 'S' */ S_TOP | S_LV2 | S_ML | S_MC | S_RV | S_BOT,
    /* 'T' */ S_TOP | S_TV | S_BV,
    /* 'U' */ S_LEFT | S_RIGHT | S_BOT,
    /* 'V' */ S_LEFT | S_BL | S_TR,
    /* 'W' */ S_LEFT | S_RIGHT | S_BL | S_BR,
    /* 'X' */ S_TL | S_TR | S_BL | S_BR,
    /* 'Y' */ S_TL | S_TR | S_BV,
    /* 'Z' */ S_TOP | S_TR | S_BL | S_BOT,
    /* '[' */ S_TOP | S_LEFT | S_BOT,
    /* '\\' */ S_TL | S_BR,
    /* ']' */ S_TOP | S_RIGHT | S_BOT,
    /* '^' */ S_BL | S_BR,
    /* '_' */ S_BOT,
};

constexpr int FIRST_CHAR = 0x20;
constexpr int LAST_CHAR  = 0x5F;

}  // namespace

uint16_t sonyVfdFontGlyph(char c) {
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    uint8_t u = static_cast<uint8_t>(c);
    if (u < FIRST_CHAR || u > LAST_CHAR) return 0;
    return FONT[u - FIRST_CHAR];
}

uint16_t sonyVfdFontGlyphWithDot(char c) {
    return sonyVfdFontGlyph(c) | S_DOT;
}
