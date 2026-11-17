// usbmidi.h -- USB MIDI 1.0 parsing with no USB stack: finding a device's
// MIDI endpoints in its configuration descriptor, and unpacking the 4-byte
// event packets it sends.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t ms_itf;             // the MIDIStreaming interface's number
    const uint8_t *ep_in;       // its first IN endpoint descriptor (bulk, or interrupt)
    const uint8_t *ep_out;      // its first OUT endpoint descriptor, or NULL
} usbmidi_found_t;

// Walk len bytes of descriptors from an interface descriptor of class Audio
// (Audio Control, or MIDIStreaming itself) and find the MIDIStreaming
// interface and its first IN and OUT endpoints. True if it has an IN.
bool usbmidi_find(const uint8_t *desc, uint32_t len, usbmidi_found_t *out);

// A USB-MIDI event packet (cable/CIN, then up to 3 MIDI bytes): copies a
// channel message (note, pressure, control, program, bend) into msg and
// returns true; anything else (system, SysEx, padding) returns false.
bool usbmidi_channel_msg(const uint8_t pkt[4], uint8_t msg[3]);
