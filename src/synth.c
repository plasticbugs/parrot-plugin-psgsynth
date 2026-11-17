// synth.c -- see synth.h.
//
// Envelopes work in the PSG's own unit, attenuation (2 dB a step), so a
// steady fall in level is an exponential fade, as a real decay is. Times in
// the instrument tables are for the whole range, 15 steps (30 dB): a decay
// to sustain 3 with decay_ms 600 takes 120 ms.
#include "synth.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PSG_CLOCK 3579545.0f
// Periodic noise clocked by tone channel 3 (noise control 0xE3): the LFSR
// shifts once per tone-3 cycle and a single bit goes round it, one pulse
// every 15 shifts on the SN76489A (MAME's model; test/wavcheck.py measures it).
#define BUZZ_DIV  15.0f
#define NCTL_BUZZ 0xE3          // periodic, clocked by tone 3
#define NCTL_IDLE 0xE4          // white, the fastest fixed rate (a valid control byte, silent)

// Keypad 1-9. Names fit the screen (9 characters) and match midihello.asm's help.
const instrument_t instruments[] = {
    {"LEAD", MODE_TONE, .attack_ms = 4, .decay_ms = 600, .sustain = 3, .release_ms = 300,
     .vib_delay_ms = 300, .vib_hz10 = 60, .vib_cents = 20},
    {"ORGAN", MODE_TONE, .sustain = 0, .release_ms = 40},
    {"PLUCK", MODE_TONE, .decay_ms = 700, .sustain = 15, .release_ms = 150},
    {"PAD", MODE_TONE, .attack_ms = 500, .decay_ms = 1000, .sustain = 2, .release_ms = 1500,
     .vib_delay_ms = 600, .vib_hz10 = 45, .vib_cents = 12},
    {"MAJOR ARP", MODE_TONE, .decay_ms = 1500, .sustain = 2, .release_ms = 200,
     .arp = {0, 4, 7}, .arp_n = 3, .arp_ms = 16},
    {"MINOR ARP", MODE_TONE, .decay_ms = 1500, .sustain = 2, .release_ms = 200,
     .arp = {0, 3, 7}, .arp_n = 3, .arp_ms = 16},
    {"BUZZ BASS", MODE_BUZZ, .decay_ms = 900, .sustain = 3, .release_ms = 150},
    {"ZAP", MODE_TONE, .decay_ms = 450, .sustain = 15, .sweep_semis = 36, .sweep_ms = 200},
    {"DRUMS", .mode = MODE_DRUMS},
};
const int n_instruments = (int)(sizeof instruments / sizeof instruments[0]);

// MODE_DRUMS: by pitch class, in General MIDI's order from C (kick on C2,
// MIDI 36). Tone parts are one-shots on the tone voices; the noise parts
// share the one noise channel, the newest hit taking it. The PSG's lowest
// tone is 109 Hz, so the kick is a fast fall onto A2 with a thump of noise.
#define HIT(ms, up, glide_ms) {.mode = MODE_TONE, .decay_ms = ms, .sustain = 15, \
                               .sweep_semis = up, .sweep_ms = glide_ms}
const drum_t drums[12] = {
    {"KICK", 45, HIT(220, 24, 50), 0xE6, 20, 3},
    {"RIM", 74, HIT(60, 0, 0), 0, 0, 0},
    {"SNARE", 55, HIT(150, 7, 30), 0xE5, 300, 0},
    {"CLAP", -1, HIT(0, 0, 0), 0xE5, 200, 1},
    {"SNARE 2", 60, HIT(100, 12, 20), 0xE4, 250, 0},
    {"LOW TOM", 50, HIT(450, 7, 120), 0xE6, 30, 6},
    {"CLOSED HAT", -1, HIT(0, 0, 0), 0xE4, 60, 2},
    {"MID TOM", 55, HIT(420, 7, 120), 0xE6, 30, 6},
    {"PEDAL HAT", -1, HIT(0, 0, 0), 0xE4, 100, 4},
    {"HIGH TOM", 60, HIT(400, 7, 120), 0xE6, 30, 6},
    {"OPEN HAT", -1, HIT(0, 0, 0), 0xE4, 450, 2},
    {"CRASH", -1, HIT(0, 0, 0), 0xE4, 1500, 0},
};

// ---- pitch -----------------------------------------------------------------------
static float hz_of(float semis) { return 440.0f * exp2f((semis - 69.0f) / 12.0f); }

// whole octaves until lo <= n <= hi
static uint16_t fold(float n, float lo, float hi) {
    while (n > hi) n *= 0.5f;
    while (n < lo) n *= 2.0f;
    long p = lroundf(n);
    return (uint16_t)(p < 1 ? 1 : p > 1023 ? 1023 : p);
}

static uint16_t tone_period(float semis) { return fold(PSG_CLOCK / (32.0f * hz_of(semis)), 0.0f, 1023.49f); }

// Only whole periods: from A3 up (N 34) neighbours are half a semitone apart
// and more, so high notes come out of tune. Kept so (rather than folded down
// an octave): the keyboard keeps rising.
static uint16_t buzz_period(float semis) { return fold(PSG_CLOCK / (32.0f * BUZZ_DIV * hz_of(semis)), 0.0f, 1023.49f); }

static float bend_semis(int bend) { return (float)bend * (2.0f / 8192.0f); }

uint16_t synth_period(int note, int bend) { return tone_period((float)note + bend_semis(bend)); }

uint16_t synth_buzz_period(int note, int bend) { return buzz_period((float)(note - 12) + bend_semis(bend)); }

// the pitch a voice sounds now: its note, the bend, and the instrument's
// sweep, vibrato and arpeggio at this point in the note
static float semis_now(const synth_t *s, const voice_t *v) {
    const instrument_t *in = v->ins;
    float n = v->base + bend_semis(s->bend), ms = (float)v->t_us * 1e-3f;
    if (in->sweep_ms && ms < in->sweep_ms) n += in->sweep_semis * (1.0f - ms / in->sweep_ms);
    if (in->vib_cents && ms >= in->vib_delay_ms)
        n += in->vib_cents * 0.01f * sinf(6.2831853f * in->vib_hz10 * 0.1f * (ms - in->vib_delay_ms) * 1e-3f);
    if (in->arp_n) n += in->arp[v->t_us / (in->arp_ms * 1000u) % in->arp_n];
    return n;
}

static uint16_t period_now(const synth_t *s, const voice_t *v) {
    float n = semis_now(s, v);
    return v->ins->mode == MODE_BUZZ ? buzz_period(n) : tone_period(n);
}

// ---- envelopes ---------------------------------------------------------------------
// level change per tick for a whole-range time
static int32_t rate(uint16_t ms) {
    return ms ? (int32_t)((int64_t)LVL_SILENT * TICK_US / (1000 * (int64_t)ms)) : LVL_SILENT;
}

static int32_t vel_level(uint8_t vel) { return (int32_t)((127 - vel) >> 4) * LVL_ONE; }   // 0-7 steps

static uint8_t att_of(int32_t level) {
    int32_t a = (level + LVL_ONE / 2) / LVL_ONE;
    return (uint8_t)(a < 0 ? 0 : a > 15 ? 15 : a);
}

static uint8_t voice_att(const voice_t *v) {
    return v->stage == ENV_IDLE ? 15 : att_of(v->level + vel_level(v->vel));
}

// stages that take no time pass at once
static void env_settle(voice_t *v) {
    const instrument_t *in = v->ins;
    if (v->stage == ENV_ATTACK && !in->attack_ms) {
        v->level = 0;
        v->stage = ENV_DECAY;
    }
    if (v->stage == ENV_DECAY && !in->decay_ms) {
        v->level = in->sustain * LVL_ONE;
        v->stage = in->sustain >= 15 ? ENV_IDLE : ENV_SUSTAIN;
    }
}

static void env_tick(voice_t *v) {
    const instrument_t *in = v->ins;
    switch (v->stage) {
    case ENV_ATTACK:
        v->level -= rate(in->attack_ms);
        if (v->level <= 0) {
            v->level = 0;
            v->stage = ENV_DECAY;
        }
        break;
    case ENV_DECAY: {
        int32_t sus = in->sustain * LVL_ONE;
        v->level += rate(in->decay_ms);
        if (v->level >= sus) {
            v->level = sus;
            v->stage = in->sustain >= 15 ? ENV_IDLE : ENV_SUSTAIN;
        }
        break;
    }
    case ENV_RELEASE:
        v->level += rate(in->release_ms);
        if (v->level >= LVL_SILENT) {
            v->level = LVL_SILENT;
            v->stage = ENV_IDLE;
        }
        break;
    default:
        break;
    }
}

// ---- keys and voices -----------------------------------------------------------------
static void key_up(synth_t *s, uint8_t note, uint8_t ch) {
    int j = 0;
    for (int i = 0; i < s->n_keys; i++)
        if (s->keys[i].note != note || s->keys[i].ch != ch) s->keys[j++] = s->keys[i];
    s->n_keys = (uint8_t)j;
}

static void key_down(synth_t *s, uint8_t note, uint8_t ch) {
    const int max = (int)(sizeof s->keys / sizeof s->keys[0]);
    key_up(s, note, ch);
    if (s->n_keys == max) {
        memmove(s->keys, s->keys + 1, (max - 1) * sizeof s->keys[0]);
        s->n_keys--;
    }
    s->keys[s->n_keys].note = note;
    s->keys[s->n_keys].ch = ch;
    s->n_keys++;
}

// The same key still sounding: its voice. Else the voice idle longest; else
// the quietest one whose key is up; else the oldest.
static int pick_voice(const synth_t *s, uint8_t note, uint8_t ch) {
    int best = -1;
    for (int i = 0; i < VOICES; i++)
        if (s->v[i].stage != ENV_IDLE && s->v[i].note == note && s->v[i].ch == ch) return i;
    for (int i = 0; i < VOICES; i++)
        if (s->v[i].stage == ENV_IDLE && (best < 0 || s->v[i].age < s->v[best].age)) best = i;
    if (best >= 0) return best;
    for (int i = 0; i < VOICES; i++)
        if (!s->v[i].held && (best < 0 || s->v[i].level > s->v[best].level)) best = i;
    if (best >= 0) return best;
    best = 0;
    for (int i = 1; i < VOICES; i++)
        if (s->v[i].age < s->v[best].age) best = i;
    return best;
}

static void voice_start(synth_t *s, int i, const instrument_t *in, uint8_t note, uint8_t ch, uint8_t vel,
                        float base, int8_t drum) {
    voice_t *v = &s->v[i];
    if (v->stage == ENV_IDLE) v->level = LVL_SILENT;      // else the attack starts where it is
    v->ins = in;
    v->note = note;
    v->ch = ch;
    v->vel = vel;
    v->held = true;
    v->drum = drum;
    v->t_us = 0;
    v->age = ++s->clock;
    v->base = base;
    v->stage = ENV_ATTACK;
    env_settle(v);
    v->period = period_now(s, v);
}

static void voice_release(voice_t *v) {
    v->held = false;
    if (v->stage != ENV_IDLE && v->ins->release_ms) v->stage = ENV_RELEASE;   // a one-shot plays on
}

static const instrument_t *current(const synth_t *s) { return &instruments[s->ins]; }

static void note_on(synth_t *s, uint8_t ch, uint8_t note, uint8_t vel) {
    const instrument_t *in = current(s);
    key_down(s, note, ch);
    if (in->mode == MODE_TONE) {
        voice_start(s, pick_voice(s, note, ch), in, note, ch, vel, note, -1);
    } else if (in->mode == MODE_BUZZ) {                   // one voice, on tone 3; the newest key
        voice_start(s, 2, in, note, ch, vel, (float)note - 12.0f, -1);
    } else {
        int pc = note % 12;
        const drum_t *d = &drums[pc];
        if (d->tone_note >= 0) voice_start(s, pick_voice(s, note, ch), &d->tone, note, ch, vel, d->tone_note, (int8_t)pc);
        if (d->nctl) {
            s->nctl = d->nctl;
            s->nseq++;                                    // written again even if the same: a fresh hit
            s->noise.level = d->noise_level * LVL_ONE + vel_level(vel);
            s->noise.step = rate(d->noise_ms);
            s->noise.drum = (int8_t)pc;
        }
    }
}

static void note_off(synth_t *s, uint8_t ch, uint8_t note) {
    key_up(s, note, ch);
    if (current(s)->mode == MODE_BUZZ) {
        voice_t *v = &s->v[2];
        if (!v->held || v->note != note || v->ch != ch) return;
        if (s->n_keys) {                                  // back to the newest key still held, no new attack
            v->note = s->keys[s->n_keys - 1].note;
            v->ch = s->keys[s->n_keys - 1].ch;
            v->base = (float)v->note - 12.0f;
            v->period = period_now(s, v);
        } else {
            voice_release(v);
        }
        return;
    }
    for (int i = 0; i < VOICES; i++)
        if (s->v[i].held && s->v[i].note == note && s->v[i].ch == ch) voice_release(&s->v[i]);
}

void synth_all_off(synth_t *s) {
    for (int i = 0; i < VOICES; i++) {
        s->v[i].stage = ENV_IDLE;
        s->v[i].level = LVL_SILENT;
        s->v[i].held = false;
    }
    s->noise.level = LVL_SILENT;
    s->noise.drum = -1;
    s->n_keys = 0;
}

void synth_init(synth_t *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < VOICES; i++) {
        s->v[i].ins = &instruments[0];
        s->v[i].period = synth_period(69, 0);
        s->v[i].drum = -1;
    }
    s->nctl = NCTL_IDLE;
    synth_all_off(s);
    strcpy(s->last, "PLAY A KEY");
}

void synth_set_instrument(synth_t *s, int i) {
    if (i < 0 || i >= n_instruments) return;
    synth_all_off(s);
    s->ins = i;
    if (instruments[i].mode == MODE_BUZZ) {
        s->nctl = NCTL_BUZZ;                              // and the LFSR starts from one bit
        s->nseq++;
    }
}

void synth_tick(synth_t *s) {
    for (int i = 0; i < VOICES; i++) {
        voice_t *v = &s->v[i];
        if (v->stage == ENV_IDLE) continue;
        v->t_us += TICK_US;
        env_tick(v);
        if (v->stage != ENV_IDLE) v->period = period_now(s, v);
    }
    if (s->noise.level < LVL_SILENT) {
        s->noise.level += s->noise.step;
        if (s->noise.level >= LVL_SILENT) {
            s->noise.level = LVL_SILENT;
            s->noise.drum = -1;
        }
    }
}

void note_name(int note, char *buf) {
    static const char *const names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    sprintf(buf, "%s%d", names[note % 12], note / 12 - 1);
}

void synth_midi(synth_t *s, uint8_t status, uint8_t d1, uint8_t d2) {
    uint8_t ch = (uint8_t)((status & 0x0F) + 1);
    char n[8];
    switch (status & 0xF0) {
    case 0x90:
        if (d2) {
            note_on(s, ch, d1, d2);
            note_name(d1, n);
            snprintf(s->last, sizeof s->last, "NOTE ON  %-4s VEL %3u  CH %u", n, d2, ch);
            break;
        }
        /* fall through: note on with velocity 0 is note off */
    case 0x80:
        note_off(s, ch, d1);
        note_name(d1, n);
        snprintf(s->last, sizeof s->last, "NOTE OFF %-4s          CH %u", n, ch);
        break;
    case 0xA0:
        note_name(d1, n);
        snprintf(s->last, sizeof s->last, "PRESSURE %-4s %3u      CH %u", n, d2, ch);
        break;
    case 0xB0:
        if (d1 == 120 || d1 == 123) synth_all_off(s);
        snprintf(s->last, sizeof s->last, "CONTROL %3u TO %3u     CH %u", d1, d2, ch);
        break;
    case 0xC0:
        if (d1 < n_instruments) synth_set_instrument(s, d1);
        snprintf(s->last, sizeof s->last, "PROGRAM %3u            CH %u", d1, ch);
        break;
    case 0xD0:
        snprintf(s->last, sizeof s->last, "PRESSURE %3u           CH %u", d1, ch);
        break;
    case 0xE0:
        s->bend = (int)((d2 & 0x7F) << 7 | (d1 & 0x7F)) - 8192;
        for (int i = 0; i < VOICES; i++)
            if (s->v[i].stage != ENV_IDLE) s->v[i].period = period_now(s, &s->v[i]);
        snprintf(s->last, sizeof s->last, "PITCH BEND %+5d       CH %u", s->bend, ch);
        break;
    default:
        return;                                           // not a channel message
    }
    s->events++;
}

void synth_psg(const synth_t *s, uint8_t out[MB_SND_LEN]) {
    bool buzz = current(s)->mode == MODE_BUZZ;
    uint8_t noise = att_of(s->noise.level);
    for (int i = 0; i < VOICES; i++) {
        uint16_t p = s->v[i].period;
        uint8_t att = voice_att(&s->v[i]);
        if (buzz && i == 2) {                             // tone 3 only clocks the noise
            noise = att;
            att = 15;
        }
        out[3 * i + 0] = (uint8_t)(0x80 | i << 5 | (p & 0x0F));
        out[3 * i + 1] = (uint8_t)(p >> 4 & 0x3F);
        out[3 * i + 2] = (uint8_t)(0x90 | i << 5 | att);
    }
    out[9] = (uint8_t)(0xF0 | noise);
}

int synth_newest_key(const synth_t *s) { return s->n_keys ? s->keys[s->n_keys - 1].note : -1; }

void synth_levels(const synth_t *s, uint8_t level[4]) {
    uint8_t psg[MB_SND_LEN];
    synth_psg(s, psg);
    bool buzz = current(s)->mode == MODE_BUZZ;
    for (int i = 0; i < 3; i++) level[i] = (uint8_t)(15 - (psg[3 * i + 2] & 15));
    if (buzz) level[2] = (uint8_t)(15 - (psg[9] & 15));
    level[3] = buzz ? 0 : (uint8_t)(15 - (psg[9] & 15));
}

void mbox_sound(const synth_t *s, uint8_t mb[MB_SIZE]) {
    uint8_t snd[MB_SND_LEN];
    synth_psg(s, snd);
    bool changed = memcmp(mb + MB_SND, snd, sizeof snd) != 0;
    if (changed) memcpy(mb + MB_SND, snd, sizeof snd);
    if (mb[MB_NSEQ] != s->nseq || mb[MB_NCTL] != s->nctl) {
        mb[MB_NSEQ] = s->nseq;
        mb[MB_NCTL] = s->nctl;
        changed = true;
    }
    if (changed) mb[MB_SND_SEQ]++;
}
