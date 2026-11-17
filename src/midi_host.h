// midi_host.h -- a minimal USB MIDI host class driver for TinyUSB 0.18 (the
// pico-sdk's, which has MIDI device support but no MIDI host). One device:
// its MIDI IN, and its MIDI OUT if it has one.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Keep an IN transfer queued, and send what midi_host_send() queued; call
// from the main loop after tuh_task().
void midi_host_task(void);

// Queue one USB-MIDI event packet for the device. False if it has no OUT
// endpoint (or none is mounted) or the queue is full.
bool midi_host_send(const uint8_t pkt[4]);

// The device address of the mounted MIDI device, or 0.
uint8_t midi_host_daddr(void);

// The IN and OUT endpoints' addresses (for the status line), 0 if none.
uint8_t midi_host_ep(void);
uint8_t midi_host_ep_out(void);

// Supplied by the application, called from tuh_task():
void midi_host_packet(const uint8_t pkt[4]);   // one USB-MIDI event packet
void midi_host_gone(uint8_t daddr);            // the MIDI device was unplugged
