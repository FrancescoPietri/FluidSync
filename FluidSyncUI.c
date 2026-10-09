//FluidSync - user interface (LVGL v8.3), landscape 320x240
//Needs LV_FONT_MONTSERRAT_14 and LV_FONT_MONTSERRAT_20 enabled in lv_conf.h
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "lvgl/lvgl.h"
#include "FluidSyncUI.h"


#define SCR_W 320
#define SCR_H 240

#define COL_BG       0x10141B
#define COL_PANEL    0x1B212C
#define COL_TEXT     0xFFFFFF
#define COL_MUTED    0x8A94A6
#define COL_OUTLINE  0x3A4352
#define COL_ACCENT   0x3B82F6

//liquid colors, same order as the pumps: yellow, magenta, cyan, white, black, water
static const uint32_t liquid_color[N_PUMPS] = { 0xFFD400, 0xE4007C, 0x00A3E0, 0xF5F5F5, 0x222222, 0x9FD8FF };
static const char *liquid_name[N_PUMPS] = { "Y", "M", "C", "W", "K", "H2O" };

//colors of the status banner
typedef struct { uint32_t bg; uint32_t fg; const char *icon; const char *title; const char *sub; } banner_t;

//* Mix color
//approximation of paint mixing: weighted geometric mean of each channel (subtractive-like).
//water only dilutes, so it is treated as white.
static lv_color_t mix_color(const uint8_t pct[N_PUMPS]) {
    double acc[3] = { 0, 0, 0 };
    int tot = 0;
    for (int i = 0; i < N_PUMPS; i++) {
        if (pct[i] == 0) continue;
        uint32_t c = (i == 5) ? 0xF5F5F5 : liquid_color[i];
        int ch[3] = { (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF };
        for (int k = 0; k < 3; k++) acc[k] += pct[i] * log(ch[k] < 13 ? 13 : ch[k]);
        tot += pct[i];
    }
    if (tot == 0) return lv_color_hex(COL_OUTLINE);
    int r = (int)exp(acc[0] / tot), g = (int)exp(acc[1] / tot), b = (int)exp(acc[2] / tot);
    return lv_color_make(r, g, b);
}

//* Widgets
static lv_obj_t *lbl_queue;
static lv_obj_t *banner, *banner_title, *banner_sub;

static lv_obj_t *cup[CAROUSEL_CUPS_SLOTS];       //cup outline
static lv_obj_t *cup_fill[CAROUSEL_CUPS_SLOTS];  //liquid inside
static lv_obj_t *cup_label[CAROUSEL_CUPS_SLOTS];
static lv_obj_t *cup_arrow[CAROUSEL_CUPS_SLOTS]; //arrow on the slot under the nozzle

static lv_obj_t *swatch;
static lv_obj_t *chip[N_PUMPS], *chip_label[N_PUMPS];

//last drawn values, to redraw only what changed
static ui_snapshot_t shown;
static bool first = true;
static lv_color_t slot_color[CAROUSEL_CUPS_SLOTS]; //color of the mix poured in each full cup

#define CUP_W 50
#define CUP_H 62

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    //top bar
    lv_obj_t *top = box(scr, 0, 0, SCR_W, 28, COL_PANEL);
    lv_obj_t *name = text(top, &lv_font_montserrat_14, COL_TEXT);
    lv_label_set_text(name, "FluidSync");
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 10, 0);
    lbl_queue = text(top, &lv_font_montserrat_14, COL_MUTED);
    lv_obj_align(lbl_queue, LV_ALIGN_RIGHT_MID, -10, 0);

    //status banner
    banner = box(scr, 0, 28, SCR_W, 52, COL_OUTLINE);
    banner_title = text(banner, &lv_font_montserrat_20, COL_TEXT);
    lv_obj_align(banner_title, LV_ALIGN_TOP_LEFT, 12, 6);
    banner_sub = text(banner, &lv_font_montserrat_14, COL_TEXT);
    lv_obj_align(banner_sub, LV_ALIGN_TOP_LEFT, 12, 31);

    //carousel: 4 cups
    for (int i = 0; i < CAROUSEL_CUPS_SLOTS; i++) {
        int cx = i * (SCR_W / CAROUSEL_CUPS_SLOTS) + (SCR_W / CAROUSEL_CUPS_SLOTS - CUP_W) / 2;

        cup_arrow[i] = text(scr, &lv_font_montserrat_14, COL_ACCENT);
        lv_label_set_text(cup_arrow[i], LV_SYMBOL_DOWN);
        lv_obj_set_pos(cup_arrow[i], cx + CUP_W / 2 - 7, 82);

        cup[i] = box(scr, cx, 100, CUP_W, CUP_H, COL_BG);
        lv_obj_set_style_radius(cup[i], 6, 0);
        lv_obj_set_style_border_width(cup[i], 2, 0);
        lv_obj_set_style_clip_corner(cup[i], true, 0);

        cup_fill[i] = box(cup[i], 0, 0, CUP_W, 0, COL_OUTLINE);
        lv_obj_align(cup_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);

        cup_label[i] = text(scr, &lv_font_montserrat_14, COL_MUTED);
        lv_obj_set_width(cup_label[i], SCR_W / CAROUSEL_CUPS_SLOTS);
        lv_obj_set_style_text_align(cup_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(cup_label[i], i * (SCR_W / CAROUSEL_CUPS_SLOTS), 166);
    }

    //current mix: color swatch + one chip per pump
    lv_obj_t *mixp = box(scr, 0, 188, SCR_W, 52, COL_PANEL);
    swatch = box(mixp, 8, 6, 40, 40, COL_OUTLINE); //color of the mix (approximate)
    lv_obj_set_style_radius(swatch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(swatch, 1, 0);
    lv_obj_set_style_border_color(swatch, lv_color_hex(COL_MUTED), 0);

    for (int i = 0; i < N_PUMPS; i++) {
        int col = i % 3, row = i / 3;
        chip[i] = box(mixp, 56 + col * 88, 4 + row * 23, 84, 21, COL_BG);
        lv_obj_set_style_radius(chip[i], 4, 0);
        lv_obj_set_style_border_width(chip[i], 1, 0);
        lv_obj_set_style_border_color(chip[i], lv_color_hex(COL_OUTLINE), 0);

        lv_obj_t *dot = box(chip[i], 5, 5, 11, 11, liquid_color[i]);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        lv_obj_set_style_border_color(dot, lv_color_hex(COL_MUTED), 0);

        chip_label[i] = text(chip[i], &lv_font_montserrat_14, COL_TEXT);
        lv_obj_set_pos(chip_label[i], 20, 2);
    }

    for (int i = 0; i < CAROUSEL_CUPS_SLOTS; i++) slot_color[i] = lv_color_hex(COL_OUTLINE);
}

static banner_t banner_for(const ui_snapshot_t *s) {
    static char title[32];
    switch (s->state) {
        case UI_WAIT_MIX:
            return (banner_t){ 0xFFB300, 0x1A1A1A, LV_SYMBOL_PAUSE, "PLEASE WAIT", "Finishing the current mix" };
        case UI_USER_TURN:
            return (banner_t){ 0x22A447, 0xFFFFFF, LV_SYMBOL_OK, "CAROUSEL IS YOURS", "Load or remove cups, then press" };
        case UI_NO_CUPS:
            return (banner_t){ 0xD93636, 0xFFFFFF, LV_SYMBOL_WARNING, "NO EMPTY CUP", "Press the button to load cups" };
        default:
            if (s->dispensing) {
                snprintf(title, sizeof title, "DISPENSING  %d%%", s->fill_pct);
                return (banner_t){ 0x2563EB, 0xFFFFFF, LV_SYMBOL_PLAY, title, "Do not touch the carousel" };
            }
            if (s->queue_len == 0)
                return (banner_t){ 0x2B3240, 0xFFFFFF, LV_SYMBOL_LIST, "WAITING FOR ORDERS", "Ready" };
            return (banner_t){ 0x2B3240, 0xFFFFFF, LV_SYMBOL_REFRESH, "CHECKING CUPS", "Looking for an empty cup" };
    }
}

void ui_update(const ui_snapshot_t *s) {
    char buf[48];

    //top bar
    if (first || s->queue_len != shown.queue_len || s->mixes_done != shown.mixes_done) {
        snprintf(buf, sizeof buf, "Queue %d/%d   Done %lu", s->queue_len, s->queue_max, (unsigned long)s->mixes_done);
        lv_label_set_text(lbl_queue, buf);
    }

    //banner (the title also carries the progress while dispensing)
    if (first || s->state != shown.state || s->dispensing != shown.dispensing ||
        s->fill_pct != shown.fill_pct || (s->queue_len == 0) != (shown.queue_len == 0)) {
        banner_t b = banner_for(s);
        lv_obj_set_style_bg_color(banner, lv_color_hex(b.bg), 0);
        snprintf(buf, sizeof buf, "%s  %s", b.icon, b.title);
        lv_label_set_text(banner_title, buf);
        lv_label_set_text(banner_sub, b.sub);
        lv_obj_set_style_text_color(banner_title, lv_color_hex(b.fg), 0);
        lv_obj_set_style_text_color(banner_sub, lv_color_hex(b.fg), 0);
        lv_obj_set_style_text_opa(banner_sub, LV_OPA_80, 0);
    }

    lv_color_t mc = mix_color(s->mix_pct);
    bool mix_changed = first || memcmp(s->mix_pct, shown.mix_pct, sizeof s->mix_pct) != 0;

    //cups
    for (int i = 0; i < CAROUSEL_CUPS_SLOTS; i++) {
        bool filling = s->dispensing && s->active_slot == i;
        bool was_filling = !first && shown.dispensing && shown.active_slot == i;
        if (!first && s->slot[i] == shown.slot[i] && filling == was_filling &&
            !(filling && (s->fill_pct != shown.fill_pct || mix_changed)) &&
            (s->active_slot == i) == (shown.active_slot == i))
            continue;

        if (filling) slot_color[i] = mc; //remember the color for when the cup is full

        uint32_t border = COL_OUTLINE, label_col = COL_MUTED;
        lv_opa_t border_opa = LV_OPA_COVER;
        int level = 0;
        const char *state;
        if (filling) {
            border = COL_ACCENT; label_col = COL_TEXT;
            level = s->fill_pct;
            snprintf(buf, sizeof buf, "%d  %d%%", i + 1, s->fill_pct);
            state = buf;
        } else if (s->slot[i] == 1) {
            border = 0xC8CED8; label_col = COL_TEXT;
            level = 100;
            snprintf(buf, sizeof buf, "%d  FULL", i + 1);
            state = buf;
        } else if (s->slot[i] == 0) {
            border = 0xC8CED8; label_col = COL_TEXT;
            snprintf(buf, sizeof buf, "%d  EMPTY", i + 1);
            state = buf;
        } else {
            border_opa = LV_OPA_40;
            snprintf(buf, sizeof buf, "%d  -", i + 1); //no cup in this slot
            state = buf;
        }

        lv_obj_set_style_border_color(cup[i], lv_color_hex(border), 0);
        lv_obj_set_style_border_opa(cup[i], border_opa, 0);
        lv_obj_set_style_bg_color(cup_fill[i], slot_color[i], 0);
        lv_obj_set_height(cup_fill[i], (CUP_H - 4) * level / 100);
        lv_obj_align(cup_fill[i], LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_label_set_text(cup_label[i], state);
        lv_obj_set_style_text_color(cup_label[i], lv_color_hex(label_col), 0);

        if (s->active_slot == i) lv_obj_clear_flag(cup_arrow[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(cup_arrow[i], LV_OBJ_FLAG_HIDDEN);
    }

    //mix panel
    if (mix_changed) lv_obj_set_style_bg_color(swatch, mc, 0);
    for (int i = 0; i < N_PUMPS; i++) {
        if (!mix_changed && s->pump_state[i] == shown.pump_state[i]) continue;
        uint8_t st = s->pump_state[i];
        if (s->mix_pct[i] == 0) snprintf(buf, sizeof buf, "%s", liquid_name[i]);
        else if (st == UI_PUMP_DONE) snprintf(buf, sizeof buf, "%s %d%% " LV_SYMBOL_OK, liquid_name[i], s->mix_pct[i]);
        else snprintf(buf, sizeof buf, "%s %d%%", liquid_name[i], s->mix_pct[i]);
        lv_label_set_text(chip_label[i], buf);
        lv_obj_set_style_opa(chip[i], s->mix_pct[i] == 0 ? LV_OPA_40 : LV_OPA_COVER, 0);
        //running pump: blue chip
        lv_obj_set_style_bg_color(chip[i], lv_color_hex(st == UI_PUMP_RUNNING ? 0x1E3A8A : COL_BG), 0);
        lv_obj_set_style_border_color(chip[i], lv_color_hex(st == UI_PUMP_RUNNING ? COL_ACCENT : COL_OUTLINE), 0);
        lv_obj_set_style_border_width(chip[i], st == UI_PUMP_RUNNING ? 2 : 1, 0);
    }

    shown = *s;
    first = false;
}
