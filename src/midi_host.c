// midi_host.c -- see midi_host.h.
//
// TinyUSB hands an application driver each interface it finds while
// enumerating (usbh_app_driver_get_cb), ahead of its own drivers, with the
// rest of the configuration descriptor; open() returns how much of it the
// driver takes. This one takes a MIDI device's Audio Control interface and
// its MIDIStreaming interface together, as one function, and reports the
// last of them when its set_config is done. A device whose MIDIStreaming
// interface comes on its own (no Audio Control first) works too: open() then
// sees that interface.
//
// On the RP2350's host controller a bulk IN endpoint gets its own polling
// slot, so a transfer left waiting for keys doesn't hold up the control
// transfers to the hub or this device. The OUT endpoint gets one too; what
// midi_host_send() queues goes out up to 16 packets (64 bytes) a transfer.
#include <string.h>

#include "tusb.h"
#include "host/usbh_pvt.h"

#include "midi_host.h"
#include "usbmidi.h"

#define OUTQ 64                 // packets waiting to go out (a power of 2)

static struct {
    uint8_t daddr, ep_in, ep_out, itf_last;
    bool configured, busy, out_busy;
    uint8_t buf[64];            // one full-speed bulk packet: 16 events
    uint8_t out_buf[64];
    uint8_t q[OUTQ][4];
    uint32_t q_head, q_tail;    // free-running: q_tail - q_head packets queued
} m;

static bool mh_init(void) {
    memset(&m, 0, sizeof m);
    return true;
}

static bool mh_deinit(void) { return true; }

// the bytes taken (from itf, its endpoints and what follows them), 0 if none
static uint16_t mh_open(uint8_t rhport, uint8_t daddr, tusb_desc_interface_t const *itf, uint16_t max_len) {
    (void)rhport;
    if (itf->bInterfaceClass != TUSB_CLASS_AUDIO || m.daddr) return 0;
    usbmidi_found_t f;
    if (!usbmidi_find((const uint8_t *)itf, max_len, &f)) return 0;
    if (!tuh_edpt_open(daddr, (tusb_desc_endpoint_t const *)f.ep_in)) return 0;
    m.daddr = daddr;
    m.ep_in = f.ep_in[2];
    m.ep_out = f.ep_out && tuh_edpt_open(daddr, (tusb_desc_endpoint_t const *)f.ep_out) ? f.ep_out[2] : 0;
    m.itf_last = f.ms_itf;
    m.configured = m.busy = m.out_busy = false;
    m.q_head = m.q_tail = 0;
    return (uint16_t)f.len;
}

static bool mh_set_config(uint8_t daddr, uint8_t itf_num) {
    (void)itf_num;
    if (daddr == m.daddr) m.configured = true;
    usbh_driver_set_config_complete(daddr, m.itf_last);
    return true;
}

static bool mh_xfer_cb(uint8_t daddr, uint8_t ep, xfer_result_t result, uint32_t n) {
    if (daddr != m.daddr) return false;
    if (m.ep_out && ep == m.ep_out) {
        m.out_busy = false;                     // sent, or dropped: the clock carries on regardless
        return true;
    }
    if (ep != m.ep_in) return false;
    m.busy = false;
    if (result == XFER_RESULT_SUCCESS)
        for (uint32_t i = 0; i + 4 <= n; i += 4) midi_host_packet(&m.buf[i]);
    return true;
}

static void mh_close(uint8_t daddr) {
    if (daddr != m.daddr) return;
    memset(&m, 0, sizeof m);
    midi_host_gone(daddr);
}

static usbh_class_driver_t const driver = {
    .name = "MIDI",
    .init = mh_init,
    .deinit = mh_deinit,
    .open = mh_open,
    .set_config = mh_set_config,
    .xfer_cb = mh_xfer_cb,
    .close = mh_close,
};

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *count) {
    *count = 1;
    return &driver;
}

static void send_queued(void) {
    uint32_t n = m.q_tail - m.q_head;
    if (!n || m.out_busy) return;
    if (n > sizeof m.out_buf / 4) n = sizeof m.out_buf / 4;
    for (uint32_t i = 0; i < n; i++) memcpy(&m.out_buf[4 * i], m.q[(m.q_head + i) % OUTQ], 4);
    if (!usbh_edpt_claim(m.daddr, m.ep_out)) return;
    if (usbh_edpt_xfer(m.daddr, m.ep_out, m.out_buf, (uint16_t)(4 * n))) {
        m.out_busy = true;
        m.q_head += n;
    } else {
        usbh_edpt_release(m.daddr, m.ep_out);
    }
}

void midi_host_task(void) {
    if (!m.configured || !tuh_mounted(m.daddr)) return;
    if (m.ep_out) send_queued();
    if (m.busy || !usbh_edpt_claim(m.daddr, m.ep_in)) return;
    if (usbh_edpt_xfer(m.daddr, m.ep_in, m.buf, sizeof m.buf)) m.busy = true;
    else usbh_edpt_release(m.daddr, m.ep_in);
}

bool midi_host_send(const uint8_t pkt[4]) {
    if (!m.configured || !m.ep_out || m.q_tail - m.q_head >= OUTQ) return false;
    memcpy(m.q[m.q_tail % OUTQ], pkt, 4);
    m.q_tail++;
    return true;
}

uint8_t midi_host_daddr(void) { return m.configured ? m.daddr : 0; }

uint8_t midi_host_ep(void) { return m.ep_in; }

uint8_t midi_host_ep_out(void) { return m.ep_out; }
