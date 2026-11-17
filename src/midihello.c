// midihello -- PSG Synth: the rev C cart as a USB MIDI host.
//
// The cart is a USB host. A MIDI keyboard on its USB-C (through a powered
// hub: the cart can't power one) plays the console's PSG as a little synth
// (synth.c: nine instruments on the keypad, three voices and the noise),
// and the cart sends it MIDI clock, so its arpeggiator and sequencer run in
// time with a tempo set on the controller. The TV shows the synth (ui.c).
//
//   core 1  serves midihello.asm's 32K image to the console, exactly as
//           cvcart_revc serves a ROM: one table lookup a read. It also
//           notes the kernel's reports (mailbox.h): reads of $FF80-$FFFF
//           whose address carries the message
//   core 0  TinyUSB host + midi_host.c; MIDI -> synth.c -> the PSG bytes
//           in a mailbox at $FF00 inside that image, which the kernel
//           reads as ROM; the synth steps its envelopes every 2 ms; the
//           screen drawn here and sent as VDP write lists at $A000
//           (screen.c); MIDI clock out
//
// Two builds (CMakeLists.txt), both no_flash binaries that run from SRAM:
//
//   midihello  picotool loads it into SRAM and runs it, and sdplayer stays
//              in flash. The cart's RESET button (or losing power) brings
//              sdplayer back. Load it with the console on and the cart on
//              USB; the console's RESET then starts the kernel, and the
//              Mac's cable can be swapped for the hub (the console keeps the
//              cart powered).
//   psgsynth   (PSG_SYNTH_CHAINED) kept in flash at 0x10E00000: sdplayer's
//              menu lists it as PSG SYNTH, and picking it starts it (the
//              bootrom copies it into SRAM). The console's RESET goes back
//              to the menu: core 1 takes a settled header read ($8000) more
//              than a second after it started as the BIOS starting over, and
//              switches to handoff.asm inside that read; the console parks
//              in its own RAM, and the cart reboots into sdplayer, whose
//              loader the parked console then boots. Turning the console off
//              reboots into sdplayer too.
//
// The header is always 55 AA: the cart is already running whenever the
// console reads it, so the BIOS's first read is always answered (see
// cvcart_revc.c for the cold-boot fallback this doesn't need).
//
// Power: as in cvcart_revc, the data outputs are driven only while nCART
// shows the console is on. A hub that feeds 5 V back up the cable keeps the
// cart running with the console off, and the outputs must stay off then.
//
// LED: solid while the console is on (blinks off for each note); slow blink
// while waiting for the console.
//
// Debugging: TinyUSB's log (usblog.c) shows on the info page (keypad #: the
// last two lines, and the first failed assertion) and is kept in flash for
// reading back.
#include <stdio.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include "host/hcd.h"

#include "mailbox.h"
#include "midi_host.h"
#include "pad.h"
#include "rom_image.h"
#if PSG_SYNTH_CHAINED
#include "handoff.h"
#include "../sdplayer/htrace.h"
#endif
#include "screen.h"
#include "synth.h"
#include "usblog.h"
#include "usbmidi.h"

#define NCART_PIN   15
#define NCART_BIT   (1u << NCART_PIN)
#define PD_SHIFT    16
#define PD_MASK     (0xffu << PD_SHIFT)
#define LED_PIN     PICO_DEFAULT_LED_PIN
#define LED_BIT     (1u << LED_PIN)
#define OFF_US      50000           // nCART low this long = console off
#define FLASH_US    30000           // LED off this long for a note
#define BEAT_US     100000          // the beat light stays lit this long
#define UI_US       8000            // the screen is brought up to date this often (at most)
#define REPORT_GAP_US 5000          // the kernel reads each report twice: one a frame counts
#define BOOT_US     1000000         // a header read this long after core 1 starts is RESET
#define PARK_US     60000           // RESET: not parked this long after, the BIOS took the title path
#define PARK_GIVE_UP_US 20000000    // ...and reboot into the menu anyway after this

static uint8_t rom_ram[0x8000] __attribute__((aligned(4)));
#if PSG_SYNTH_CHAINED
static uint8_t handoff_ram[0x8000] __attribute__((aligned(4)));   // handoff.asm, RST 0 past it
static volatile bool to_menu;               // core 1 -> core 0: RESET, the handoff is served
static uint32_t t_serve;                    // when core 1 started
#endif

// Reports: a read in $FF80-$FFFF (A7-A14 high, nCART low) counts once the
// pins have held still for 3 samples, as the console's address lines settle
// at different times after nCART falls (see sdplayer.c). The report goes in
// report[kind] (kind = A5 | A6 << 1, REPORT_KIND_*) and report_n[kind]
// counts them.
#define SETTLED 3
static volatile uint32_t report[4], report_n[4];
static uint32_t win_mask, win_val, a5_gpio, a6_gpio;   // set before core 1 starts
static uint8_t line_gpio[15];                          // address line -> GPIO

#if PSG_SYNTH_CHAINED
// $8000 (GP0-15 all low), settled: the BIOS reading the header. Within a
// second of starting, that is the kernel booting; later, RESET.
static bool __scratch_x("serve") __attribute__((noinline)) on_header(void) {
    for (uint i = 0; i < SETTLED; i++)
        if (sio_hw->gpio_in & 0xffffu) return false;
    return timer_hw->timerawl - t_serve > BOOT_US;
}

// The handoff trace (htrace.h; sdplayer prints it): a bus loop that also
// records each settled cart read (3 equal samples). A record is ~50
// cycles, done well before the next read can start (0.84 us on); a header
// read ($8000) only counts (ht_hdr), so that $8001 is in time. (The whole
// image runs from SRAM: no_flash.)
//   Y_BOOT   the kernel's image, from serving until the BIOS's first header
//            read, which ends it at once, so that serve() takes over. A
//            header read over a second in is RESET instead (the kernel
//            never ran): as below, from this sample on.
//   Y_RESET  the handoff, until core 0 reboots: HT_PARKED reads of the
//            title in a row (the console parked) tell it to. Entered inside
//            the RESET read, which isn't counted.
static ht_unperm_t ht_u;
static volatile bool parked;
static void __attribute__((noinline)) __not_in_flash_func(serve_traced)(const uint8_t *img, int p) {
    ht_phase_t *ph = &HTR->ph[p];
    uint32_t prev = ~0u, same = 0, park_n = 0;
    bool done = p == HT_Y_RESET;
    for (;;) {
        uint32_t in = sio_hw->gpio_in;
        sio_hw->gpio_out = (uint32_t)img[in & 0x7fffu] << PD_SHIFT | LED_BIT;
        uint32_t pins = in & 0xffffu;
        if (pins & NCART_BIT) { done = false; prev = ~0u; continue; }
        if (done) continue;
        if (pins != prev) { prev = pins; same = 0; continue; }
        if (++same < 2) continue;
        done = true;
        uint32_t off = ht_off(&ht_u, pins), t = timer_hw->timerawl;
        if (off == 0) {
            if (p == HT_Y_BOOT && t - t_serve > BOOT_US) {
                img = handoff_ram;          // RESET: both headers are 55 AA
                to_menu = true;
                p = HT_Y_RESET;
                ph = &HTR->ph[p];
            }
            ht_hdr(ph, t, img[0]);
            if (p == HT_Y_BOOT) return;     // the BIOS boots the kernel
            continue;
        }
        ht_bus(ph, t, off, img[pins & 0x7fffu]);
        if (p != HT_Y_RESET || !ht_park_read(off)) park_n = 0;    // (at boot it parked in sdplayer's handoff)
        else if (++park_n == HT_PARKED) parked = true;
    }
}
#endif

static void __scratch_x("serve") __attribute__((noreturn)) serve(void) {
    const uint32_t mask = win_mask, val = win_val, a5 = a5_gpio, a6 = a6_gpio;
    const uint8_t *img = rom_ram;
    uint32_t prev = 0, same = 0;
    save_and_disable_interrupts();
#if PSG_SYNTH_CHAINED
    t_serve = timer_hw->timerawl;
    serve_traced(rom_ram, HT_Y_BOOT);       // returns as the kernel boots
#endif
    for (;;) {
        uint32_t in = sio_hw->gpio_in;
        sio_hw->gpio_out = (uint32_t)img[in & 0x7fffu] << PD_SHIFT | LED_BIT;
        if ((in & mask) != val) {
            prev = 0;
#if PSG_SYNTH_CHAINED
            if (!(in & 0xffffu) && on_header()) {       // RESET: the handoff, from the next sample on
                to_menu = true;
                serve_traced(handoff_ram, HT_Y_RESET);
            }
#endif
            continue;
        }
        in &= 0xffffu;
        if (in != prev) {
            prev = in;
            same = 1;
        } else if (++same == SETTLED) {
            uint32_t k = (in >> a5 & 1) | (in >> a6 & 1) << 1;
            report[k] = in;
            report_n[k] = report_n[k] + 1;
        }
    }
}

// a report's A0-A5
static uint8_t report_bits(uint32_t in) {
    uint8_t a = 0;
    for (uint l = 0; l < 6; l++) if (in >> line_gpio[l] & 1) a |= (uint8_t)(1u << l);
    return a;
}

// ---- the mailbox ---------------------------------------------------------------
static uint8_t perm_lut[256];               // console byte -> GP16-23 order
static uint16_t fidx_lo[256], fidx_hi[128]; // ROM offset (A0-A7, A8-A14) -> rom_ram index bits
static uint8_t mb[MB_SIZE], mb_live[MB_SIZE];

static inline uint32_t findex(uint32_t a) { return fidx_lo[a & 0xFF] | fidx_hi[a >> 8 & 0x7F]; }

static void mailbox_setup(void) {
    for (uint v = 0; v < 256; v++) {
        uint8_t p = 0;
        for (uint k = 0; k < 8; k++) if (v >> data_bit[k] & 1) p |= (uint8_t)(1u << k);
        perm_lut[v] = p;
    }
    for (uint g = 0; g < 15; g++) line_gpio[addr_line[g]] = (uint8_t)g;
    for (uint v = 0; v < 256; v++) {
        uint32_t lo = 0, hi = 0;
        for (uint l = 0; l < 8; l++) if (v >> l & 1) lo |= 1u << line_gpio[l];
        for (uint l = 0; l < 7; l++) if (v >> l & 1) hi |= 1u << line_gpio[8 + l];
        fidx_lo[v] = (uint16_t)lo;
        if (v < 128) fidx_hi[v] = (uint16_t)hi;
    }
    win_val = 0;
    for (uint l = 7; l < 15; l++) win_val |= 1u << line_gpio[l];
    win_mask = win_val | NCART_BIT;
    a5_gpio = line_gpio[5];
    a6_gpio = line_gpio[6];
}

static inline void mb_put(uint i, uint8_t v) {
    rom_ram[findex(MB_BASE - 0x8000 + i)] = perm_lut[v];
    mb_live[i] = v;
}

// What changed: the bytes first, then the sequence numbers. NSEQ goes
// before SND_SEQ: the kernel reads it only after a new SND_SEQ, and must
// then find NCTL new too.
static void mailbox_publish(void) {
    for (uint i = 0; i < MB_SIZE; i++)
        if (i != MB_SND_SEQ && i != MB_LIST_SEQ && i != MB_NSEQ && mb[i] != mb_live[i]) mb_put(i, mb[i]);
    __dmb();
    if (mb[MB_NSEQ] != mb_live[MB_NSEQ]) mb_put(MB_NSEQ, mb[MB_NSEQ]);
    __dmb();
    if (mb[MB_SND_SEQ] != mb_live[MB_SND_SEQ]) mb_put(MB_SND_SEQ, mb[MB_SND_SEQ]);
    if (mb[MB_LIST_SEQ] != mb_live[MB_LIST_SEQ]) mb_put(MB_LIST_SEQ, mb[MB_LIST_SEQ]);
}

// a VDP write list to LIST_BASE (screen.c has made sure the Z80 isn't reading it)
static void list_put(const uint8_t *list, size_t n) {
    for (size_t i = 0; i < n; i++) rom_ram[findex(LIST_BASE - 0x8000 + i)] = perm_lut[list[i]];
}

// ---- console power -----------------------------------------------------------
static bool driving, console_off;
static uint32_t last_high, flash_until;

// Late (after a flash write stalled the loop), one low sample may be a
// cart read in progress, not the console off: nCART goes high between the
// Z80's memory cycles, under a microsecond apart, so look again for ~7 us.
// (The menu's build reboots into sdplayer when the console goes off.)
static inline void watch(uint32_t in, uint32_t t) {
    if (!(in & NCART_BIT) && driving && t - last_high > OFF_US)
        for (int i = 0; i < 256 && !(in & NCART_BIT); i++) in = sio_hw->gpio_in;
    if (in & NCART_BIT) {
        last_high = t;
        if (!driving) {
            sio_hw->gpio_oe_set = PD_MASK;
            driving = true;
#if PSG_SYNTH_CHAINED
            ht_mark(to_menu ? HT_Y_RESET : HT_Y_BOOT, t, HM_DRIVE, 0);
#endif
        }
    } else if (driving && t - last_high > OFF_US) {
        sio_hw->gpio_oe_clr = PD_MASK;
        driving = false;
        console_off = true;
#if PSG_SYNTH_CHAINED
        ht_mark(to_menu ? HT_Y_RESET : HT_Y_BOOT, t, HM_UNDRIVE, t - last_high);
#endif
    }
    bool led = driving ? (int32_t)(t - flash_until) >= 0 : (t >> 19 & 1);
    if (led) sio_hw->gpio_oe_set = LED_BIT;
    else     sio_hw->gpio_oe_clr = LED_BIT;
}

// ---- MIDI clock out ------------------------------------------------------------------
// 24 a quarter note, always while a device with a MIDI OUT is there; Start
// when it arrives (if playing) and Start / Stop from the left fire button
// or keypad 0. The clock runs without a device too, for the beat light: a
// beat every 24 ticks from Start while playing, every 4th the bar's first.
#define START_BPM 120
static pad_t pad;
static uint8_t clock_dev;                   // the device the clock is running for
static uint32_t clock_t0, clock_ticks;      // since the last tempo change
static int clock_bpm = START_BPM;
static uint32_t song_ticks, beat_at;        // ticks since Start; the last beat's time
static bool beat_down;

static void send_rt(uint8_t b) {
    const uint8_t pkt[4] = {0x0F, b, 0, 0};
    midi_host_send(pkt);
}

static bool clock_out(void) { return midi_host_daddr() && midi_host_ep_out(); }

// Start (or a new device while playing): the next tick now, and the bar from it
static void clock_start(uint32_t t) {
    if (clock_out()) send_rt(0xFA);
    clock_t0 = t;
    clock_ticks = 0;
    clock_bpm = pad.bpm;
    song_ticks = 0;
}

static void clock_task(uint32_t t) {
    uint8_t d = clock_out() ? midi_host_daddr() : 0;
    if (d != clock_dev) {
        clock_dev = d;
        if (d && pad.running) clock_start(t);
    }
    if (pad.bpm != clock_bpm) {             // the next tick at the new tempo, from the last one
        clock_t0 += (uint32_t)((uint64_t)clock_ticks * 2500000u / (uint32_t)clock_bpm);
        clock_ticks = 0;
        clock_bpm = pad.bpm;
    }
    for (int n = 0; n < 4; n++) {           // late (a flash write): catch up a little, then skip
        uint32_t due = clock_t0 + (uint32_t)((uint64_t)clock_ticks * 2500000u / (uint32_t)clock_bpm);
        if ((int32_t)(t - due) < 0) return;
        if (d) send_rt(0xF8);
        clock_ticks++;
        if (pad.running) {
            if (song_ticks % 24 == 0) {
                beat_at = t;
                beat_down = song_ticks % 96 == 0;
            }
            song_ticks++;
        }
    }
    clock_t0 = t;                           // that last one was now; on from there
    clock_ticks = 1;
}

static int beat_light(uint32_t t) {
    if (!pad.running || t - beat_at >= BEAT_US || !song_ticks) return BEAT_OFF;
    return beat_down ? BEAT_DOWN : BEAT_ON;
}

// ---- USB -------------------------------------------------------------------------
static synth_t syn;
static bool snd_dirty = true;
static char midi_name[24];                  // the MIDI device's product string
static uint16_t name_buf[40];
static uint8_t other_daddr;                 // a device that isn't MIDI

void midi_host_packet(const uint8_t pkt[4]) {
    uint8_t msg[3];
    if (!usbmidi_channel_msg(pkt, msg)) return;
    synth_midi(&syn, msg[0], msg[1], msg[2]);
    if ((msg[0] & 0xF0) == 0x90 && msg[2]) flash_until = time_us_32() + FLASH_US;
    snd_dirty = true;
}

void midi_host_gone(uint8_t daddr) {
    (void)daddr;
    synth_all_off(&syn);
    strcpy(syn.last, "PLAY A KEY");
    midi_name[0] = 0;
    snd_dirty = true;
}

static void name_cb(tuh_xfer_t *x) {
    if (x->result != XFER_RESULT_SUCCESS) return;
    const uint8_t *d = (const uint8_t *)name_buf;
    uint n = d[0] >= 2 ? (d[0] - 2u) / 2u : 0, j = 0;
    for (uint i = 0; i < n && j + 1 < sizeof midi_name; i++) {
        uint16_t c = name_buf[1 + i];
        midi_name[j++] = c >= 32 && c < 127 ? (char)c : ' ';
    }
    midi_name[j] = 0;
}

void tuh_mount_cb(uint8_t daddr) {
    if (daddr == midi_host_daddr()) {
        midi_name[0] = 0;
        tuh_descriptor_get_product_string(daddr, 0x0409, name_buf, sizeof name_buf, name_cb, 0);
    } else {
        other_daddr = daddr;
    }
}

void tuh_umount_cb(uint8_t daddr) {
    if (daddr == other_daddr) other_daddr = 0;
}

// what is on the cart's USB port: the info page's lines, and the main
// page's (the keyboard's name, or what to do)
static char st_usb[40], st_ids[40], st_dev[40], st_device[40], log1[33], log2[33], log_assert[33];

static void usb_lines(void) {
    const size_t n = sizeof st_usb;
    uint16_t vid, pid;
    // every device with an address, hubs marked H
    st_dev[0] = 0;
    for (uint8_t a = 1, k = 0; a <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB && k < 3; a++)
        if (tuh_vid_pid_get(a, &vid, &pid)) {
            size_t l = strlen(st_dev);
            snprintf(st_dev + l, n - l, "%04X:%04X%s", vid, pid, a > CFG_TUH_DEVICE_MAX ? "H " : "  ");
            k++;
        }
    uint8_t d = midi_host_daddr();
    if (d && tuh_vid_pid_get(d, &vid, &pid)) {
        snprintf(st_device, n, "%s", midi_name[0] ? midi_name : "MIDI KEYBOARD");
        snprintf(st_usb, n, "USB   %s", st_device);
        if (midi_host_ep_out())
            snprintf(st_ids, n, "      %04X:%04X  IN %02X  OUT %02X", vid, pid, midi_host_ep(), midi_host_ep_out());
        else
            snprintf(st_ids, n, "      %04X:%04X  IN %02X", vid, pid, midi_host_ep());
        return;
    }
    if (other_daddr && tuh_vid_pid_get(other_daddr, &vid, &pid)) {
        snprintf(st_device, n, "%04X:%04X IS NOT MIDI", vid, pid);
        snprintf(st_usb, n, "USB   %s", st_device);
        snprintf(st_ids, n, " ");
        return;
    }
    for (uint8_t h = CFG_TUH_DEVICE_MAX + 1; h <= CFG_TUH_DEVICE_MAX + CFG_TUH_HUB; h++)
        if (tuh_vid_pid_get(h, &vid, &pid)) {
            snprintf(st_device, n, "HUB FOUND: PLUG A KEYBOARD IN");
            snprintf(st_usb, n, "USB   HUB %04X:%04X", vid, pid);
            snprintf(st_ids, n, "      NO MIDI DEVICE ON IT YET");
            return;
        }
    if (hcd_port_connect_status(0)) {
        snprintf(st_device, n, "STARTING USB...");
        snprintf(st_usb, n, "USB   SOMETHING IS PLUGGED IN");
        snprintf(st_ids, n, "      STARTING IT...");
        return;
    }
    snprintf(st_device, n, "PLUG A KEYBOARD INTO A HUB");
    snprintf(st_usb, n, "USB   WAITING FOR A DEVICE");
    snprintf(st_ids, n, "      THROUGH A POWERED HUB");
}

// ---- the screen ----------------------------------------------------------------------
static screen_t scr;
static uint8_t list[SCREEN_LIST_MAX];
static bool info_page;

static void screen_task(uint32_t t) {
    if (!scr.ready || scr.out) return;
    usb_lines();
    ui_state_t st = {
        .syn = &syn, .device = st_device, .connected = midi_host_daddr() != 0,
        .has_out = midi_host_ep_out() != 0, .bpm = pad.bpm, .running = pad.running,
        .beat = beat_light(t), .info = info_page,
        .status = {[ST_USB] = st_usb, [ST_IDS] = st_ids, [ST_DEVICES] = st_dev,
                   [ST_ASSERT] = log_assert, [ST_LOG1] = log1, [ST_LOG2] = log2}};
    size_t n = screen_update(&scr, &st, list, sizeof list, t);
    if (!n) return;
    list_put(list, n);
    mb[MB_LIST_FULL] = scr.out_full;
    mb[MB_LIST_SEQ] = scr.seq;
    mailbox_publish();
}

// the kernel's reports, one a frame of each kind
static uint32_t seen[4], heard_at[4];

static void reports(uint32_t t) {
    for (uint k = 0; k < 4; k++) {
        uint32_t c = report_n[k];
        if (c == seen[k]) continue;
        seen[k] = c;
        if (t - heard_at[k] < REPORT_GAP_US) continue;     // the same frame's second read
        heard_at[k] = t;
        uint8_t a = report_bits(report[k]);
        if (k == REPORT_KIND_ACK || k == REPORT_KIND_HELLO) {
            screen_report(&scr, k == REPORT_KIND_HELLO, a & 31, t);
            continue;
        }
        bool was = pad.running;
        if (!pad_report(&pad, a, t)) continue;
        snd_dirty = true;
        if (pad.pressed == 0) pad.running = !pad.running;  // keypad 0: as the left fire button
        if (pad.running != was) {
            if (pad.running) clock_start(t);
            else if (clock_out()) send_rt(0xFC);
        }
        if (pad.pressed >= 1 && pad.pressed <= 9) synth_set_instrument(&syn, pad.pressed - 1);
        else if (pad.pressed == PAD_STAR) synth_all_off(&syn);
        else if (pad.pressed == PAD_HASH) info_page = !info_page;
        pad.pressed = -1;
    }
}

#if PSG_SYNTH_CHAINED
// back to the menu: sdplayer is the flash's boot image (a watchdog reboot
// is a normal boot). The outputs go off first, so the pads come out of the
// reset undriven: the parked console reads $00 until the loader is served.
static void __attribute__((noreturn)) to_sdplayer(uint32_t why) {
    multicore_reset_core1();
    ht_mark(HT_Y_RESET, timer_hw->timerawl, HM_REBOOT, why);
    sio_hw->gpio_oe_clr = PD_MASK;
    watchdog_reboot(0, 0, 1);
    for (;;) tight_loop_contents();
}
#endif

int main(void) {
#if PSG_SYNTH_CHAINED
    __asm volatile ("cpsie i" ::: "memory"); // as after a reset, whatever sdplayer's chain left
#endif
    for (uint p = 0; p < 15; p++) {         // address lines: plain inputs, driven at 5V
        gpio_init(p);
        gpio_disable_pulls(p);
    }
    gpio_init(NCART_PIN);
    gpio_pull_down(NCART_PIN);              // reads low while U5 is unpowered
    for (uint p = PD_SHIFT; p < PD_SHIFT + 8; p++)
        gpio_init(p);                       // outputs only once watch() sees the console
    gpio_init(LED_PIN);

    memcpy(rom_ram, rom_image, sizeof rom_ram);
    mailbox_setup();
#if PSG_SYNTH_CHAINED
    memset(handoff_ram, perm_lut[0xC7], sizeof handoff_ram);
    for (uint32_t a = 0; a < HANDOFF_BIN_LEN; a++) handoff_ram[findex(a)] = perm_lut[handoff_bin[a]];
    ht_init(false);                         // sdplayer started it: add this side
    ht_clear(HT_Y_BOOT);
    ht_clear(HT_Y_RESET);
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_MAIN, 0);
    ht_unperm_init(&ht_u, addr_line);
#endif
    synth_init(&syn);
    pad_init(&pad, START_BPM, true);
    screen_init(&scr);
    mbox_sound(&syn, mb);
    for (uint i = 0; i < MB_SIZE; i++) mb_put(i, mb[i]);
    __dmb();
    multicore_launch_core1(serve);
#if PSG_SYNTH_CHAINED
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_SERVE, 0);
#endif
    watch(sio_hw->gpio_in, time_us_32());   // drive the bus now if the console is on
    usblog_init();                          // erases 64K of flash: up to a second or two
#if PSG_SYNTH_CHAINED
    ht_mark(HT_Y_BOOT, timer_hw->timerawl, HM_USBLOG, 0);
#endif

    tusb_rhport_init_t host = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_AUTO};
    tusb_init(0, &host);

    uint32_t t_tick = time_us_32(), t_ui = t_tick;
#if PSG_SYNTH_CHAINED
    uint32_t t_menu = 0;
    bool titled = false;
#endif
    for (;;) {
        uint32_t t = time_us_32();
        watch(sio_hw->gpio_in, t);
#if PSG_SYNTH_CHAINED
        // RESET: reboot into the menu once the console is seen parked in the
        // handoff (never on a timer alone: the BIOS can be slow to get
        // there). Not parked by PARK_US means it missed the header and is on
        // the title path: AA 55, which that path reads at 252 ms, then it
        // runs the handoff. (Times are signed: t is from before to_menu.)
        if (to_menu && !t_menu) {
            t_menu = t | 1;
            ht_mark(HT_Y_RESET, t, HM_RESET, 0);
        }
        if (console_off) to_sdplayer(1);
        if (t_menu) {
            int32_t since = (int32_t)(t - t_menu);
            if (parked) {
                ht_mark(HT_Y_RESET, t, HM_PARKED, 0);
                to_sdplayer(2);
            }
            if (!titled && since > PARK_US) {
                titled = true;
                handoff_ram[0] = perm_lut[0xAA];
                handoff_ram[findex(1)] = perm_lut[0x55];
                ht_mark(HT_Y_RESET, t, HM_TITLE_PATH, 0);
            }
            if (since > PARK_GIVE_UP_US) to_sdplayer(3);
        }
#endif
        tuh_task();
        clock_task(t);
        midi_host_task();
        usblog_flush();
        usblog_screen(log1, log2, log_assert);
        reports(t);
        if ((int32_t)(t - t_tick) >= 0) {   // envelopes, sweeps, vibrato, arpeggios
            t_tick += TICK_US;
            if ((int32_t)(t - t_tick) > 10 * TICK_US) t_tick = t + TICK_US;
            synth_tick(&syn);
            snd_dirty = true;
        }
        if (snd_dirty) {
            snd_dirty = false;
            mbox_sound(&syn, mb);
            mailbox_publish();
        }
        if (t - t_ui >= UI_US) {
            t_ui = t;
            screen_task(t);
        }
    }
}
