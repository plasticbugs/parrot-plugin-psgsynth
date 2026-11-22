// TinyUSB configuration for midihello: host only, on the RP2350's own USB
// port, through hubs (a "hub" box often holds two hub chips).
#pragma once

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_PICO
#endif
// The log (CFG_TUSB_DEBUG, set by LOG in CMakeLists.txt) goes to usblog.c:
// the screen, and flash for reading back.
#define CFG_TUSB_DEBUG_PRINTF       usblog_printf

#define CFG_TUH_ENABLED             1
#define CFG_TUH_MAX_SPEED           OPT_MODE_FULL_SPEED
#define CFG_TUH_ENUMERATION_BUFSIZE 1024    // the whole configuration descriptor
// A device that can't get an address (a hub beyond CFG_TUH_HUB) stopped all
// enumeration for good in TinyUSB 0.18; 0.21 gives up on that one device.
// Room to spare all the same.
#define CFG_TUH_HUB                 3
#define CFG_TUH_DEVICE_MAX          8
#define CFG_TUH_ENDPOINT_MAX        8
#define CFG_TUH_INTERFACE_MAX       8

// TinyUSB's own MIDI host driver off: midi_host.c is this one's, and takes a
// MIDI device's Audio Control + MIDIStreaming interfaces as one function (a
// second MIDI device then gets no driver at all, as before)
#define CFG_TUH_MIDI                0
