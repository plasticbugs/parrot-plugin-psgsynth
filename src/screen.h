// screen.h -- the cart's side of the screen protocol (mailbox.h): draws the
// UI (ui.h) into a canvas and sends the console what changed as VDP write
// lists (vdp.h), one at a time.
//
// The kernel says HELLO each frame from its start until it has run a list;
// the answer is the whole screen (LIST_FULL), encoded against VRAM as
// vdp_init() leaves it. After that each list is answered by an ACK, and only
// then is the next one made, so a list is never rewritten while the Z80 may
// read it:
//
//   - an ACK or HELLO carrying the newest list's seq: it ran it, or (HELLO)
//     will never run it: it found it there at start, or it is a list of
//     changes, which a kernel that has just started skips. Either way it is
//     done with LIST.
//   - a HELLO with an older seq while a list is out: it started before that
//     list was written. It runs a whole screen (which suits it: its VRAM is
//     as vdp_init() says) and skips changes; either way it says so next.
//   - nothing for SCREEN_TIMEOUT_US: the console is off or elsewhere; a
//     HELLO then starts over whatever it carries.
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ui.h"
#include "vdp.h"

#define SCREEN_LIST_MAX   8192          // a list's bytes: ~70 ms on the Z80
#define SCREEN_TIMEOUT_US 1000000

typedef struct {
    vdp_t vdp;                  // the console's VRAM, as the lists sent so far leave it
    canvas_t canvas;
    ui_t ui;
    bool ready;                 // the kernel has said HELLO
    bool full;                  // the next list is the whole screen
    bool out, out_full;         // a list is out (and it is a whole screen)
    uint8_t seq;                // the newest list's LIST_SEQ
    uint32_t sent_us;
    uint32_t lists, starts;     // lists made, and whole screens among them
} screen_t;

void screen_init(screen_t *s);

// A report from the kernel: HELLO or ACK, and the seq it carries (A0-A4).
void screen_report(screen_t *s, bool hello, uint8_t seq5, uint32_t now_us);

// When the kernel is there and no list is out: bring the canvas up to date
// with st and, if anything changed, write the next list to list (at most max
// bytes) and return its length. The caller puts it at LIST_BASE, then
// LIST_FULL = s->out_full, then LIST_SEQ = s->seq, in that order. 0:
// nothing to send now.
size_t screen_update(screen_t *s, const ui_state_t *st, uint8_t *list, size_t max, uint32_t now_us);
