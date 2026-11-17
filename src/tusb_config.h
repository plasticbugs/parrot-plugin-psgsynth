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
// A device that can't get an address (a hub beyond CFG_TUH_HUB) stops all
// enumeration for good in this TinyUSB: its failure path never ends the
// enumeration nor re-arms the hub. So room to spare.
#define CFG_TUH_HUB                 3
#define CFG_TUH_DEVICE_MAX          8
#define CFG_TUH_ENDPOINT_MAX        8
#define CFG_TUH_INTERFACE_MAX       8

// This TinyUSB has no MIDI host driver (midi_host.c is one); the setting only
// makes usbh treat a MIDI device's Audio Control + MIDIStreaming interfaces
// as one function and hand them to the same driver.
#define CFG_TUH_MIDI                1
