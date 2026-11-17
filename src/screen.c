// screen.c -- see screen.h.
#include "screen.h"

#include <string.h>

void screen_init(screen_t *s) {
    memset(s, 0, sizeof *s);
    vdp_init(&s->vdp);
    canvas_init(&s->canvas);
    ui_init(&s->ui);
}

void screen_report(screen_t *s, bool hello, uint8_t seq5, uint32_t now_us) {
    bool done = seq5 == (s->seq & 31);
    if (s->out && (done || (hello && now_us - s->sent_us > SCREEN_TIMEOUT_US))) s->out = false;
    if (!hello) return;
    s->ready = true;
    if (!(s->out && s->out_full)) s->full = true;   // else that is the answer to an earlier HELLO
}

size_t screen_update(screen_t *s, const ui_state_t *st, uint8_t *list, size_t max, uint32_t now_us) {
    if (!s->ready || s->out) return 0;
    bool full = s->full;
    if (full) {
        s->full = false;
        vdp_init(&s->vdp);
        canvas_all_dirty(&s->canvas);
        ui_init(&s->ui);
        s->starts++;
    }
    ui_draw(&s->ui, &s->canvas, st);
    size_t n = vdp_list(&s->vdp, &s->canvas, list, max);
    if (!n) return 0;
    s->seq++;
    s->out = true;
    s->out_full = full;
    s->sent_us = now_us;
    s->lists++;
    return n;
}
