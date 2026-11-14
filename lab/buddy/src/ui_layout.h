#pragma once

#if defined(BOARD_LILYGO_EPD47)

// LilyGo T5 4.7" EPD — 960×540, grayscale, 3 buttons
static const int UI_FONT_BODY     = 2;
static const int UI_FONT_TITLE    = 4;
static const int UI_FONT_BIG      = 6;
static const int UI_FONT_HUGE     = 8;

static const int UI_GLYPH_W       = 12;
static const int UI_GLYPH_H       = 16;

static const int UI_MENU_W        = 400;
static const int UI_MENU_ITEM_H   = 32;
static const int UI_MENU_PAD_X    = 16;
static const int UI_MENU_PAD_Y    = 20;
static const int UI_MENU_HINT_H   = 32;
static const int UI_MENU_CORNER   = 8;
static const int UI_MENU_VAL_OFF  = 96;

static const int UI_PANEL_TOP     = 160;
static const int UI_LINE_H        = 20;
static const int UI_SECTION_GAP   = 14;
static const int UI_TEXT_MARGIN   = 12;

static const int UI_WRAP_WIDTH    = 60;
static const int UI_WRAP_COLS     = 64;

static const int UI_APPROVAL_H    = 180;
static const int UI_HINT_CHARS    = 60;

static const int UI_HUD_LINES     = 6;
static const int UI_HUD_LINE_H   = 20;

static const int UI_CLOCK_TIME_SZ = 8;
static const int UI_CLOCK_SEC_SZ  = 4;
static const int UI_CLOCK_DATE_SZ = 3;
static const int UI_CLOCK_TIME_Y  = 300;
static const int UI_CLOCK_SEC_Y   = 380;
static const int UI_CLOCK_DATE_Y  = 430;
static const int UI_CLOCK_CLEAR_Y = 200;

static const int UI_PASSKEY_LBL_Y  = 120;
static const int UI_PASSKEY_CODE_Y = 240;
static const int UI_PASSKEY_HINT_Y = 400;
static const int UI_PASSKEY_CODE_SZ = 6;

static const int UI_HEART_R       = 5;
static const int UI_HEART_SPACE   = 36;
static const int UI_STAT_DOT_R    = 5;
static const int UI_STAT_DOT_SPACE = 20;
static const int UI_ENERGY_W      = 20;
static const int UI_ENERGY_H      = 14;
static const int UI_ENERGY_SPACE  = 28;
static const int UI_LEVEL_W       = 100;
static const int UI_LEVEL_H       = 32;
static const int UI_LEVEL_R       = 8;

static const int UI_BTN_W         = 0;
static const int UI_BTN_H         = 0;
static const int UI_BTN_R         = 0;
static const int UI_BTN_MARGIN    = 0;

#elif defined(BOARD_ATD35_S3)

// Font sizes
static const int UI_FONT_BODY     = 2;
static const int UI_FONT_TITLE    = 4;
static const int UI_FONT_BIG      = 6;
static const int UI_FONT_HUGE     = 8;

// Glyph metrics at body size
static const int UI_GLYPH_W       = 12;
static const int UI_GLYPH_H       = 16;

// Menu panels
static const int UI_MENU_W        = 280;
static const int UI_MENU_ITEM_H   = 28;
static const int UI_MENU_PAD_X    = 12;
static const int UI_MENU_PAD_Y    = 16;
static const int UI_MENU_HINT_H   = 28;
static const int UI_MENU_CORNER   = 8;
static const int UI_MENU_VAL_OFF  = 72;

// Info/Pet panels
static const int UI_PANEL_TOP     = 140;
static const int UI_LINE_H        = 18;
static const int UI_SECTION_GAP   = 12;
static const int UI_TEXT_MARGIN   = 8;

// Word wrap
static const int UI_WRAP_WIDTH    = 26;
static const int UI_WRAP_COLS     = 28;

// Approval panel
static const int UI_APPROVAL_H    = 160;
static const int UI_HINT_CHARS    = 26;

// HUD (transcript)
static const int UI_HUD_LINES     = 4;
static const int UI_HUD_LINE_H    = 18;

// Clock face (portrait only on ATD35)
static const int UI_CLOCK_TIME_SZ = 8;
static const int UI_CLOCK_SEC_SZ  = 4;
static const int UI_CLOCK_DATE_SZ = 2;
static const int UI_CLOCK_TIME_Y  = 280;
static const int UI_CLOCK_SEC_Y   = 350;
static const int UI_CLOCK_DATE_Y  = 400;
static const int UI_CLOCK_CLEAR_Y = 180;

// Passkey screen
static const int UI_PASSKEY_LBL_Y  = 112;
static const int UI_PASSKEY_CODE_Y = 220;
static const int UI_PASSKEY_HINT_Y = 368;
static const int UI_PASSKEY_CODE_SZ = 6;

// Pet stats
static const int UI_HEART_R       = 4;
static const int UI_HEART_SPACE   = 32;
static const int UI_STAT_DOT_R    = 4;
static const int UI_STAT_DOT_SPACE = 18;
static const int UI_ENERGY_W      = 18;
static const int UI_ENERGY_H      = 12;
static const int UI_ENERGY_SPACE  = 26;
static const int UI_LEVEL_W       = 84;
static const int UI_LEVEL_H       = 28;
static const int UI_LEVEL_R       = 6;

// Approval buttons
static const int UI_BTN_W         = 140;
static const int UI_BTN_H         = 48;
static const int UI_BTN_R         = 8;
static const int UI_BTN_MARGIN    = 10;

#else // M5StickC Plus — original values

static const int UI_FONT_BODY     = 1;
static const int UI_FONT_TITLE    = 2;
static const int UI_FONT_BIG      = 3;
static const int UI_FONT_HUGE     = 4;

static const int UI_GLYPH_W       = 6;
static const int UI_GLYPH_H       = 8;

static const int UI_MENU_W        = 118;
static const int UI_MENU_ITEM_H   = 14;
static const int UI_MENU_PAD_X    = 6;
static const int UI_MENU_PAD_Y    = 8;
static const int UI_MENU_HINT_H   = 14;
static const int UI_MENU_CORNER   = 4;
static const int UI_MENU_VAL_OFF  = 36;

static const int UI_PANEL_TOP     = 70;
static const int UI_LINE_H        = 8;
static const int UI_SECTION_GAP   = 6;
static const int UI_TEXT_MARGIN   = 4;

static const int UI_WRAP_WIDTH    = 21;
static const int UI_WRAP_COLS     = 24;

static const int UI_APPROVAL_H    = 78;
static const int UI_HINT_CHARS    = 21;

static const int UI_HUD_LINES     = 3;
static const int UI_HUD_LINE_H    = 8;

static const int UI_CLOCK_TIME_SZ = 4;
static const int UI_CLOCK_SEC_SZ  = 2;
static const int UI_CLOCK_DATE_SZ = 1;
static const int UI_CLOCK_TIME_Y  = 140;
static const int UI_CLOCK_SEC_Y   = 175;
static const int UI_CLOCK_DATE_Y  = 200;
static const int UI_CLOCK_CLEAR_Y = 90;

static const int UI_PASSKEY_LBL_Y  = 56;
static const int UI_PASSKEY_CODE_Y = 110;
static const int UI_PASSKEY_HINT_Y = 184;
static const int UI_PASSKEY_CODE_SZ = 3;

static const int UI_HEART_R       = 2;
static const int UI_HEART_SPACE   = 16;
static const int UI_STAT_DOT_R    = 2;
static const int UI_STAT_DOT_SPACE = 9;
static const int UI_ENERGY_W      = 9;
static const int UI_ENERGY_H      = 6;
static const int UI_ENERGY_SPACE  = 13;
static const int UI_LEVEL_W       = 42;
static const int UI_LEVEL_H       = 14;
static const int UI_LEVEL_R       = 3;

static const int UI_BTN_W         = 0;
static const int UI_BTN_H         = 0;
static const int UI_BTN_R         = 0;
static const int UI_BTN_MARGIN    = 0;

#endif
