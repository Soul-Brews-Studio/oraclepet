/*
 * board.h — which pet hardware this build is for, and the screen layout that follows.
 *
 * One firmware source, two boards (pick with `idf.py -DPET_BOARD=lcd28 ...`, see
 * ../tools/build-lcd28.sh; the default is the JC3248):
 *
 *   jc3248  Guition JC3248W535: 320x480 AXS15231B QSPI, AXS15231 touch, NS4168 amp.
 *           Project name jc3248_pet_idf (unchanged, so existing pets keep updating).
 *   lcd28   2.8" ESP32-S3 board (Freenove ESP32-S3 Display 2.8 / LCDWIKI ES3C28P
 *           pinout): 240x320 ILI9341 SPI, FT6336 touch, ES8311 codec + amp, 16 MB
 *           flash. Project name oraclepet_lcd28, so the OTA name check refuses a
 *           JC3248 image on this board and the other way round.
 *   sc01    Panlee/Wireless-Tag WT32-SC01 Plus (ZX3D50CE08S-USRC-4832, WROVER-N16R2):
 *           320x480 ST7796UI 8-bit 8080 parallel, FT6336U touch (SDA6/SCL5), NS4168
 *           amp, 2 MB quad PSRAM, no mic. Project name oraclepet_sc01.
 *
 * Layout knobs (pixels): the cat GIF is GIF_SCALE x its 96x100 frames, placed below
 * a TOP_H clock strip and above the "who" line and a HUD_H strip at the bottom.
 */
#pragma once

#if defined(PET_BOARD_LCD28)
#define BOARD_NAME        "lcd28"
#define DISP_W            240
#define DISP_H            320
#define BOARD_GIF_SCALE   2       /* 192x200 cat */
#define BOARD_HUD_H       76
#define BOARD_TOP_H       20      /* one-line clock: time right, date left */
#define BOARD_SIG_H       20      /* the oracle-name line under the cat */
#define BOARD_NAME_SCALE  2       /* HUD pack name: 8 chars fit beside the Ψ */
#define BOARD_CLK_COMPACT 1
/* ILI9341 portrait = MADCTL MX (Adafruit "rotation 0"); without it the picture is a
 * mirror image. Flip both to turn it upside down instead. */
#define BOARD_LCD_MIRROR_X 1
#define BOARD_LCD_MIRROR_Y 0
#elif defined(PET_BOARD_SC01)
/* Same 320x480 portrait as the JC3248, so the same layout. */
#define BOARD_NAME        "sc01"
#define DISP_W            320
#define DISP_H            480
#define BOARD_GIF_SCALE   3       /* 288x300 cat */
#define BOARD_HUD_H       80
#define BOARD_TOP_H       0
#define BOARD_SIG_H       0
#define BOARD_NAME_SCALE  3
#define BOARD_CLK_COMPACT 0
/* ESPHome's WT32-SC01-PLUS model mirrors X; without it the picture is a mirror image. */
#define BOARD_LCD_MIRROR_X 1
#define BOARD_LCD_MIRROR_Y 0
#else
#define BOARD_NAME        "jc3248"
#define DISP_W            320
#define DISP_H            480
#define BOARD_GIF_SCALE   3       /* 288x300 cat */
#define BOARD_HUD_H       80
#define BOARD_TOP_H       0       /* cat centred in [0, 400) as it always was */
#define BOARD_SIG_H       0
#define BOARD_NAME_SCALE  3
#define BOARD_CLK_COMPACT 0
#endif
