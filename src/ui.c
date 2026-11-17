// ui.c -- see ui.h.
//
// Every 8-pixel run of a row may hold two colours (vdp.h), so the layout
// keeps edges, text and keys from meeting a third: text starts on an
// 8-pixel column inside its box, keys are 16 pixels wide with each black
// key straddling one boundary, the meter's cells are a column each. The
// tests render every state and count runs with three colours: none.
#include "ui.h"

#include <stdio.h>
#include <string.h>

// C to B round the colour wheel; every one shows on a white key and on black
const uint8_t ui_pitch_colour[12] = {C_MRED, C_LRED, C_DYELLOW, C_LYELLOW, C_LGREEN, C_MGREEN,
                                     C_DGREEN, C_CYAN, C_LBLUE, C_DBLUE, C_MAGENTA, C_DRED};

// layout, in pixels
#define TITLE_X   8
#define TITLE_Y   4
#define LINE2_Y   24
#define BEAT_X    176
#define BEAT_Y    8
#define TEMPO_R   240               // the tempo's right edge (2x digits)
#define GRID_X    12                // three cells 72 wide, 8 apart
#define GRID_DX   80
#define GRID_W    72
#define GRID_Y    38                // four rows 14 high, 2 apart
#define GRID_DY   16
#define GRID_H    14
#define METER_Y   106
#define METER_DY  10
#define BAR_X     120               // 15 cells, a column each
#define KB_X      8                 // 15 white keys, 16 wide
#define KB_Y      152
#define KB_H      36
#define KB_BLACK_H 22
#define KB_KEYS   25                // two octaves and a C
#define FELT_Y    149

static const char *const cell_key[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "0", "#"};
static const char *const cell_name[12] = {"LEAD", "ORGAN", "PLUCK", "PAD", "MAJOR", "MINOR",
                                          "BUZZ", "ZAP", "DRUMS", "MUTE", "CLOCK", "INFO"};

void ui_init(ui_t *u) {
    memset(u, 0, sizeof *u);
    u->all = true;
    u->kb_lo = 48;                  // C3-C5: the KeyStep's keys, at its middle octave
}

static void text_right(canvas_t *c, int right, int y, const char *t, uint8_t colour, int scale) {
    canvas_text(c, right - (int)strlen(t) * 8 * scale, y, t, colour, scale);
}

// ---- the main page -------------------------------------------------------------------
static void draw_beat(canvas_t *c, int beat) {
    if (beat == BEAT_OFF) {
        canvas_fill(c, BEAT_X, BEAT_Y, 8, 8, C_GRAY);
        canvas_fill(c, BEAT_X + 1, BEAT_Y + 1, 6, 6, C_BLACK);
    } else {
        canvas_fill(c, BEAT_X, BEAT_Y, 8, 8, beat == BEAT_DOWN ? C_LRED : C_LYELLOW);
    }
}

static void draw_tempo(canvas_t *c, int bpm) {
    char t[8];
    snprintf(t, sizeof t, "%d", bpm);
    canvas_fill(c, TEMPO_R - 48, TITLE_Y, 48, 16, C_BLACK);
    text_right(c, TEMPO_R, TITLE_Y, t, C_WHITE, 2);
}

static void line2_text(const ui_state_t *s, char left[33], char right[33]) {
    right[0] = 0;
    if (s->connected) snprintf(right, 33, "%s", !s->has_out ? "NO CLOCK OUT" : s->running ? "PLAYING" : "STOPPED");
    int room = 30 - (int)strlen(right) - (right[0] ? 1 : 0);
    snprintf(left, 33, "%.*s", room, s->device ? s->device : "");
}

static void draw_line2(canvas_t *c, const char *left, const char *right, bool connected) {
    canvas_fill(c, 0, LINE2_Y, VDP_W, 8, C_BLACK);
    canvas_text(c, TITLE_X, LINE2_Y, left, connected ? C_WHITE : C_GRAY, 1);
    text_right(c, 248, LINE2_Y, right, C_GRAY, 1);
}

static void draw_cell(canvas_t *c, int i, bool lit) {
    int x = GRID_X + (i % 3) * GRID_DX, y = GRID_Y + (i / 3) * GRID_DY;
    bool fn = i >= 9;
    canvas_fill(c, x, y, GRID_W, GRID_H, fn ? C_DBLUE : lit ? C_LYELLOW : C_GRAY);
    canvas_text(c, x + 4, y + 3, cell_key[i], fn ? C_LYELLOW : C_DRED, 1);
    canvas_text(c, x + 20, y + 3, cell_name[i], fn ? C_WHITE : C_BLACK, 1);
}

static void meter_of(const ui_state_t *s, const uint8_t level[4], int row, char name[12], uint8_t *lvl, uint8_t *colour) {
    const synth_t *syn = s->syn;
    name[0] = 0;
    *lvl = level[row];
    *colour = C_WHITE;
    if (row < 3) {
        const voice_t *v = &syn->v[row];
        if (v->stage == ENV_IDLE) { *lvl = 0; return; }
        if (v->drum >= 0) snprintf(name, 12, "%s", drums[v->drum].name);
        else note_name(v->note, name);
        *colour = ui_pitch_colour[v->note % 12];
    } else if (*lvl && syn->noise.drum >= 0) {
        snprintf(name, 12, "%s", drums[syn->noise.drum].name);
    } else {
        *lvl = 0;
    }
}

static void draw_meter(canvas_t *c, int row, const char *name, int lvl, uint8_t colour) {
    static const char *const label[4] = {"1", "2", "3", "N"};
    int y = METER_Y + row * METER_DY;
    canvas_fill(c, 0, y, BAR_X, 8, C_BLACK);
    canvas_text(c, 16, y, label[row], C_GRAY, 1);
    canvas_text(c, 32, y, name, C_WHITE, 1);
    for (int i = 0; i < 15; i++) {
        int x = BAR_X + 8 * i;
        canvas_fill(c, x, y + 1, 7, 6, i < lvl ? colour : C_BLACK);
        if (i >= lvl) canvas_fill(c, x + 3, y + 4, 1, 1, C_GRAY);
    }
}

static const int white_pc[7] = {0, 2, 4, 5, 7, 9, 11};

static void draw_keyboard(canvas_t *c, int lo, const uint8_t lit[KB_KEYS]) {
    for (int j = 0; j < 15; j++) {                        // white keys, then the black over them
        int x = KB_X + 16 * j, k = 12 * (j / 7) + white_pc[j % 7];
        canvas_fill(c, x, KB_Y, 15, KB_H, lit[k] ? lit[k] : C_WHITE);
        canvas_fill(c, x + 15, KB_Y, 1, KB_H, C_BLACK);
    }
    for (int j = 0; j < 14; j++) {
        int pc = white_pc[j % 7];
        if (pc == 4 || pc == 11) continue;                // no black key after E or B
        int k = 12 * (j / 7) + pc + 1;
        canvas_fill(c, KB_X + 16 * (j + 1) - 4, KB_Y, 8, KB_BLACK_H, lit[k] ? lit[k] : C_BLACK);
    }
    (void)lo;
}

static void main_page(ui_t *u, canvas_t *c, const ui_state_t *s, bool all) {
    if (all) {
        canvas_fill(c, 0, 0, VDP_W, VDP_H, C_BLACK);
        canvas_text(c, TITLE_X, TITLE_Y, "PSG SYNTH", C_WHITE, 2);
        canvas_fill(c, KB_X, FELT_Y, 16 * 15 - 1, 2, C_DRED);
    }
    if (all || s->beat != u->beat) draw_beat(c, u->beat = s->beat);
    if (all || s->bpm != u->bpm) draw_tempo(c, u->bpm = s->bpm);

    char l[33], r[33];
    line2_text(s, l, r);
    if (all || strcmp(l, u->line2[0]) || strcmp(r, u->line2[1])) {
        strcpy(u->line2[0], l);
        strcpy(u->line2[1], r);
        draw_line2(c, l, r, s->connected);
    }

    int sel = s->syn->ins;
    if (all) {
        for (int i = 0; i < 12; i++) draw_cell(c, i, i == sel);
    } else if (sel != u->sel) {
        draw_cell(c, u->sel, false);
        draw_cell(c, sel, true);
    }
    u->sel = sel;

    uint8_t level[4];
    synth_levels(s->syn, level);
    for (int row = 0; row < 4; row++) {
        char name[12];
        uint8_t lvl, colour;
        meter_of(s, level, row, name, &lvl, &colour);
        if (all || strcmp(name, u->meter[row].name) || lvl != u->meter[row].level || colour != u->meter[row].colour) {
            snprintf(u->meter[row].name, sizeof u->meter[row].name, "%s", name);
            u->meter[row].level = lvl;
            u->meter[row].colour = colour;
            draw_meter(c, row, name, lvl, colour);
        }
    }

    // the keyboard follows the newest key a whole octave at a time
    int n = synth_newest_key(s->syn), lo = u->kb_lo;
    if (n >= 0 && n < lo) lo = n - n % 12;
    if (n >= 0 && n >= lo + KB_KEYS) lo = n - n % 12 - 12;
    if (lo > 96) lo = 96;                                 // C7: the top key in view is C9
    if (lo < 0) lo = 0;
    uint8_t lit[KB_KEYS] = {0};
    for (int i = 0; i < s->syn->n_keys; i++) {
        int k = s->syn->keys[i].note - lo;
        if (k >= 0 && k < KB_KEYS) lit[k] = ui_pitch_colour[s->syn->keys[i].note % 12];
    }
    if (all || lo != u->kb_lo || memcmp(lit, u->lit, sizeof lit)) {
        u->kb_lo = lo;
        memcpy(u->lit, lit, sizeof lit);
        draw_keyboard(c, lo, lit);
    }
}

// ---- the info page ---------------------------------------------------------------------
#if PSG_SYNTH_CHAINED
#define MENU_KEY "RESET"                    // the console's (midihello.c: the menu's build)
#else
#define MENU_KEY "CART'S RESET"             // loaded into SRAM: the cart's reboots it into sdplayer
#endif

static void info_page(ui_t *u, canvas_t *c, const ui_state_t *s, bool all) {
    char line[UI_INFO_LINES][33];
    memset(line, 0, sizeof line);
    const char *const *st = s->status;
    for (int i = 0; i < 3; i++) snprintf(line[i], 33, "%s", st[ST_USB + i] ? st[ST_USB + i] : "");
    snprintf(line[4], 33, "%s", s->syn->last);
    snprintf(line[5], 33, "EVENTS %lu   BEND %+d", (unsigned long)s->syn->events, s->syn->bend);
    snprintf(line[7], 33, "%s", st[ST_ASSERT] ? st[ST_ASSERT] : "");
    snprintf(line[8], 33, "%s", st[ST_LOG1] ? st[ST_LOG1] : "");
    snprintf(line[9], 33, "%s", st[ST_LOG2] ? st[ST_LOG2] : "");
    static const char *const help[5][2] = {
        {"STICK UP/DOWN", "TEMPO +1/-1"}, {"STICK R/L", "TEMPO +10/-10"}, {"LEFT FIRE, 0", "CLOCK PLAY/STOP"},
        {MENU_KEY, "BACK TO SD MENU"}, {"#", "BACK TO THE SYNTH"}};
    for (int i = 0; i < 5; i++) snprintf(line[11 + i], 33, "%-15s%s", help[i][0], help[i][1]);
    if (all) {
        canvas_fill(c, 0, 0, VDP_W, VDP_H, C_BLACK);
        canvas_text(c, TITLE_X, TITLE_Y, "PSG SYNTH", C_WHITE, 2);
        text_right(c, 248, TITLE_Y + 8, "INFO", C_GRAY, 1);
    }
    for (int i = 0; i < UI_INFO_LINES; i++) {
        if (!all && !strcmp(line[i], u->info[i])) continue;
        strcpy(u->info[i], line[i]);
        int y = 28 + 10 * i;
        canvas_fill(c, 0, y, VDP_W, 8, C_BLACK);
        canvas_text(c, 0, y, line[i], i >= 7 && i <= 9 ? C_GRAY : i >= 11 ? C_LBLUE : C_WHITE, 1);
    }
}

void ui_draw(ui_t *u, canvas_t *c, const ui_state_t *s) {
    int page = s->info ? 1 : 0;
    bool all = u->all || page != u->page;
    u->all = false;
    u->page = page;
    if (page) info_page(u, c, s, all);
    else main_page(u, c, s, all);
}
