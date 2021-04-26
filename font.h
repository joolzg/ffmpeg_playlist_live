// Font data for Anonymous Pro 8pt
#include <stdint.h>

typedef enum _JUSTIFY
{
    FONT_JUSTIFY_OFF            = 0x00000,
    FONT_JUSTIFY_LEFT           = 0x00001,
    FONT_JUSTIFY_CENTER         = 0x00002,
    FONT_JUSTIFY_RIGHT          = 0x00004,

    FONT_JUSTIFY_TOP            = 0x00000,
    FONT_JUSTIFY_MIDDLE         = 0x00008,
    FONT_JUSTIFY_BOTTOM         = 0x00010,
    FONT_JUSTIFY_ABOVE          = 0x00020,
    FONT_JUSTIFY_BELOW          = 0x00040,
} justify_e;

typedef enum {
    YCrCb_BLACK = 0x108080,
    YCrCb_WHITE = 0xea8080,
    YCrCb_GREY              = 0x7d8080,
    YCrCb_BLUE              = 0x1dff6b,
    YCrCb_RED               = 0x4c54ff,
    YCrCb_RED_LT            = 0x4462d8,
    YCrCb_GREEN             = 0x952b15,
    YCrCb_GREEN_LT          = 0x754636,
    YCrCb_YELLOW            = 0xd21092,
    YCrCb_YELLOW_LT         = 0xa9288e,
} colour_e;

typedef struct _FONT_CHAR_INFO {
    uint8_t width;
    short offset;
} FONT_CHAR_INFO;

typedef struct _FONT_INFO {
    int char_height;
    int start_character;
    int end_character;
    int space_width;
    const FONT_CHAR_INFO *descriptors;
    const uint8_t *bitmap;
} FONT_INFO;

// Font data for Anonymous Pro 9pt
extern const uint8_t _9ptBitmaps[];
extern const FONT_INFO _9ptFontInfo;
extern const FONT_CHAR_INFO _9ptDescriptors[];
