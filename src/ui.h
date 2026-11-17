// ui.h -- PSG Synth's screens, drawn on the cart into a canvas (vdp.h).
//
//   PSG SYNTH                 [] 120      title; beat light; tempo
//   KEYSTEP                     PLAYING   the keyboard; the MIDI clock
//   [1 LEAD ] [2 ORGAN] [3 PLUCK]         the sounds, laid out as the
//   [4 PAD  ] [5 MAJOR] [6 MINOR]         controller's keypad; the one
//   [7 BUZZ ] [8 ZAP  ] [9 DRUMS]         playing lit
//   [* MUTE ] [0 CLOCK] [# INFO ]
//   1 C4         ========.......          each voice: its note (or drum)
//   2 E4         ======.........          and how loud it is now
//   3 G4         ===............
//   N OPEN HAT   ==.............          the noise, for drums
//   |#|#||#|#|#||#|#||#|#|#||#|#||        two octaves: keys held light in
//                                         their pitch's colour
// The info page (#) has the USB details, TinyUSB's log and the controls.
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "synth.h"
#include "vdp.h"

// the info page's USB lines, from the firmware (any may be NULL)
enum { ST_USB, ST_IDS, ST_DEVICES, ST_ASSERT, ST_LOG1, ST_LOG2, ST_COUNT };

enum { BEAT_OFF, BEAT_ON, BEAT_DOWN };      // the beat light: off, a beat, the bar's first

typedef struct {
    const synth_t *syn;
    const char *device;         // the MIDI keyboard's name, or what to do when there's none
    bool connected;             // a MIDI keyboard is there
    bool has_out;               // ...and it takes MIDI clock
    int bpm;
    bool running;               // the clock is playing
    int beat;                   // BEAT_*
    bool info;                  // show the info page
    const char *status[ST_COUNT];
} ui_state_t;

#define UI_INFO_LINES 16

typedef struct {
    bool all;                   // draw everything next time
    int page;                   // as drawn: 0 main, 1 info
    char line2[2][33];
    int bpm, beat, sel;
    struct { char name[12]; uint8_t level, colour; } meter[4];
    int kb_lo;                  // the keyboard's lowest note (a C)
    uint8_t lit[25];
    char info[UI_INFO_LINES][33];
} ui_t;

// The pitch classes' colours, C to B: keys and meters.
extern const uint8_t ui_pitch_colour[12];

// Start over: the next ui_draw() draws everything.
void ui_init(ui_t *u);

// Bring the canvas up to date with s, drawing only what changed.
void ui_draw(ui_t *u, canvas_t *c, const ui_state_t *s);
