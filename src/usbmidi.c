// usbmidi.c -- see usbmidi.h.
#include "usbmidi.h"

enum { DESC_INTERFACE = 4, DESC_ENDPOINT = 5, CLASS_AUDIO = 1, SUBCLASS_MIDISTREAMING = 3 };

bool usbmidi_find(const uint8_t *desc, uint32_t len, usbmidi_found_t *out) {
    const uint8_t *p = desc, *end = desc + len;
    bool in_ms = false;
    out->ep_in = out->ep_out = 0;
    while (end - p >= 2 && p[0] >= 2 && p[0] <= end - p) {
        if (p[1] == DESC_INTERFACE && p[0] >= 9) {
            if (out->ep_in) break;                              // that MIDIStreaming interface is done
            in_ms = p[5] == CLASS_AUDIO && p[6] == SUBCLASS_MIDISTREAMING;
            if (in_ms) out->ms_itf = p[2];
        } else if (p[1] == DESC_ENDPOINT && p[0] >= 7 && in_ms && (p[3] & 3) >= 2) {   // bulk or interrupt
            if (p[2] & 0x80) {
                if (!out->ep_in) out->ep_in = p;
            } else if (!out->ep_out) {
                out->ep_out = p;
            }
        }
        p += p[0];
    }
    while (end - p >= 2 && p[0] >= 2 && p[0] <= end - p &&
           !(p[1] == DESC_INTERFACE && p[0] >= 9 && p[2] != out->ms_itf))
        p += p[0];
    out->len = (uint32_t)(p - desc);
    return out->ep_in != 0;
}

bool usbmidi_channel_msg(const uint8_t pkt[4], uint8_t msg[3]) {
    uint8_t cin = pkt[0] & 0x0F;
    // CIN 8-E are the channel messages, and they must agree with the status byte
    if (cin < 0x8 || cin > 0xE || pkt[1] >> 4 != cin) return false;
    msg[0] = pkt[1];
    msg[1] = pkt[2] & 0x7F;
    msg[2] = (cin == 0xC || cin == 0xD) ? 0 : (pkt[3] & 0x7F);
    return true;
}
