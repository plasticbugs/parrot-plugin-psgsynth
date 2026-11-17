// synth.h -- MIDI messages in, PSG bytes out.
//
// Three PSG tone channels and the noise channel, played as instruments:
// volume envelopes, vibrato, pitch sweeps, chip arpeggios, a "buzz" voice
// (periodic noise clocked by tone channel 3: a thin pulse wave) and drums.
//
// Plain C with no Pico dependencies: the firmware and the Mac tests share it.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "mailbox.h"

#define VOICES   3              // the PSG's three tone channels
#define TICK_US  2000           // synth_tick() steps envelopes and pitch this often

enum { MODE_TONE, MODE_BUZZ, MODE_DRUMS };

// Envelope times are for the whole range, 15 steps (30 dB); a shorter fall
// takes proportionally less.
typedef struct {
    const char *name;           // up to 9 characters
    uint8_t mode;
    uint16_t attack_ms;         // rise, after the key
    uint16_t decay_ms;          // fall, from the top to the sustain level
    uint8_t sustain;            // held level: attenuation 0 (loudest) - 15 (silent: the note dies)
    uint16_t release_ms;        // fall after the key goes up (0: a one-shot, which ignores it)
    int8_t sweep_semis;         // start this far from the note...
    uint16_t sweep_ms;          // ...and glide onto it in this time
    uint16_t vib_delay_ms;      // vibrato starts after this...
    uint8_t vib_hz10;           // ...at this rate (tenths of Hz)...
    uint8_t vib_cents;          // ...and depth
    int8_t arp[3];              // chip arpeggio: semitone offsets, cycled
    uint8_t arp_n;
    uint8_t arp_ms;             // one offset per this many ms
} instrument_t;

typedef struct {                // MODE_DRUMS: one per pitch class, C to B
    const char *name;
    int8_t tone_note;           // -1: no tone part
    instrument_t tone;          // its envelope and sweep
    uint8_t nctl;               // 0: no noise part; else the noise control byte
    uint16_t noise_ms;          // noise decay to silence
    uint8_t noise_level;        // noise starting attenuation
} drum_t;

extern const instrument_t instruments[];
extern const int n_instruments;
extern const drum_t drums[12];

enum { ENV_IDLE, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

// Envelope levels are attenuation in 1/4096 PSG steps: 0 loudest, LVL_SILENT off.
#define LVL_ONE     4096
#define LVL_SILENT  (15 * LVL_ONE)

typedef struct {
    const instrument_t *ins;    // what this voice plays (a drum's tone part, for drums)
    uint8_t note, vel, ch;      // the key (ch 1-16)
    bool held;                  // key still down
    uint8_t stage;              // ENV_*
    int8_t drum;                // MODE_DRUMS: the pitch class it plays, else -1
    int32_t level;              // envelope attenuation (LVL_*)
    uint32_t t_us;              // since the key
    uint32_t age;               // synth_t.clock at the key
    float base;                 // the note it sounds (drums: their own)
    uint16_t period;            // PSG period now (kept after release)
} voice_t;

typedef struct {
    voice_t v[VOICES];
    int ins;                    // index into instruments[]
    struct {                    // the noise channel, for drums
        int32_t level;          // attenuation (LVL_*)
        int32_t step;           // added each tick
        int8_t drum;            // the drum it plays, -1 if none
    } noise;
    uint8_t nctl, nseq;         // noise control byte; nseq changes when it must be (re)written
    struct { uint8_t note, ch; } keys[16];  // keys held, newest last
    uint8_t n_keys;
    uint32_t clock;             // counts keys, for stealing the oldest voice
    int bend;                   // -8192..8191, +-2 semitones, on every voice
    uint32_t events;            // channel messages seen
    char last[33];              // the last message, as text
} synth_t;

void synth_init(synth_t *s);

// Pick instruments[i]; silences everything first.
void synth_set_instrument(synth_t *s, int i);

// One channel message (status 0x80-0xEF and its data bytes). Others are ignored.
void synth_midi(synth_t *s, uint8_t status, uint8_t d1, uint8_t d2);

// Silence every voice and the noise at once (device gone, All Notes Off, panic).
void synth_all_off(synth_t *s);

// Step envelopes, sweeps, vibrato and arpeggios by TICK_US.
void synth_tick(synth_t *s);

// The PSG period for a MIDI note with a bend: clock / (32 f), moved up whole
// octaves until it fits the PSG's 10 bits (notes below A2 sound an octave up).
uint16_t synth_period(int note, int bend);

// The same for MODE_BUZZ: periodic noise clocked by tone 3, an octave below
// the key (55 Hz is 136). Only whole periods, so from A3 (34) up the notes
// are out of tune, more so the higher they go.
uint16_t synth_buzz_period(int note, int bend);

// The 10 PSG bytes: tone low (latch), tone high, volume for channels 0-2,
// then the noise volume.
void synth_psg(const synth_t *s, uint8_t out[MB_SND_LEN]);

// The newest key held, or -1.
int synth_newest_key(const synth_t *s);

// How loud each voice sounds now, 0 (silent) - 15, as the PSG has it: tone
// channels 0-2 (the buzz bass's voice 3 is its noise), then the noise as
// drums use it (0 for the buzz bass).
void synth_levels(const synth_t *s, uint8_t level[4]);

// Update the sound part of the mailbox image `mb` (MB_SIZE bytes, Z80 view):
// SND, NCTL, and their sequence numbers when they changed.
void mbox_sound(const synth_t *s, uint8_t mb[MB_SIZE]);

// "C#4" style name (MIDI 60 = C4) into buf (at least 5 bytes).
void note_name(int note, char *buf);
