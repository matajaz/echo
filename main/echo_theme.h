// echo_theme.h — shared "Echo Dark" visual language.
//
// Single source of truth for colours, spacing and font-size roles so every
// page (Overview, Temperature graph, Temperatures, Batteries, Diagnostics)
// looks consistent. Header-only: just colour/spacing constants + tiny inline
// helpers. No LVGL objects are created here.
#pragma once

#include "lvgl.h"

// ---- Echo Dark palette (from the design spec) --------------------------
#define ECHO_BG        0x0B1020   // original Overview/graph navy
#define ECHO_CARD      0x182238   // original Overview card blue
#define ECHO_CARD_TOP  0x233550   // gentle highlight above the darker card base
#define ECHO_EDGE      0x354D70
#define ECHO_BUTTON    0x2A3550   // original secondary-control blue
#define ECHO_ACTION    0x2F6BD0   // original primary-control blue
#define ECHO_COOL      0x4FB0FF   // original outdoor-temperature blue
#define ECHO_SOLAR     0xF0C020   // original sun yellow
#define ECHO_TEXT      0xF4F7FB   // primary text (near-white)
#define ECHO_TEXT_DIM  0x8AA0C0   // secondary/label text
#define ECHO_BLUE      0x39C6DC   // accent: outdoor / cool
#define ECHO_AMBER     0xE7B45A   // accent: warn / low
#define ECHO_GREEN     0x30D070   // ok / connected / healthy
#define ECHO_RED       0xFF5A50   // critical

// ---- spacing (px) ------------------------------------------------------
#define ECHO_MARGIN    8          // outer margin
#define ECHO_ROW_GAP   6          // between compact rows
#define ECHO_CARD_RAD  10         // card corner radius

// ---- font roles --------------------------------------------------------
// title ~18-20, reading ~14-16, secondary ~11-12 (nearest enabled Montserrat)
#define ECHO_FONT_TITLE   (&lv_font_montserrat_20)
#define ECHO_FONT_READ    (&lv_font_montserrat_14)
#define ECHO_FONT_SMALL   (&lv_font_montserrat_12)
LV_FONT_DECLARE(echo_sun_20);
#define ECHO_SUN_GLYPH "\xEF\x86\x85"

// Legacy helper; new pages use ui_card() and the shared long-lived styles.
static inline void echo_card_style(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(ECHO_CARD_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(ECHO_CARD), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(o, ECHO_CARD_RAD, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, lv_color_hex(ECHO_EDGE), LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
