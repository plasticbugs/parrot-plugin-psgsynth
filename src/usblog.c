// usblog.c -- see usblog.h.
#include "usblog.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define QUIET_US 500000                 // flush after this long without a new line

static char buf[USBLOG_SIZE];           // what flash will hold; 0xFF past len
static volatile uint32_t len, t_last;
static uint32_t flushed;                // bytes already programmed
static bool at_line_start = true;
static char cur[33], last[2][33], first_assert[33];
static uint32_t cur_n;
static volatile bool screen_changed;

static void put(char c) {
    if (len < USBLOG_SIZE - 1) buf[len++] = c;
}

int usblog_printf(const char *fmt, ...) {
    char tmp[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return n;
    if (n >= (int)sizeof tmp) n = sizeof tmp - 1;
    uint32_t irq = save_and_disable_interrupts();
    uint32_t now = time_us_32();
    for (int i = 0; i < n; i++) {
        char c = tmp[i];
        if (c == '\r') continue;
        if (at_line_start) {
            char ts[16];
            int k = snprintf(ts, sizeof ts, "%7lu ", (unsigned long)(now / 1000));
            for (int j = 0; j < k; j++) put(ts[j]);
            at_line_start = false;
        }
        put(c);
        if (c == '\n') {
            at_line_start = true;
            cur[cur_n] = 0;
            if (cur_n) {
                memcpy(last[0], last[1], sizeof last[0]);
                memcpy(last[1], cur, sizeof cur);
                if (!first_assert[0] && strstr(cur, "ASSERT")) memcpy(first_assert, cur, sizeof cur);
                screen_changed = true;
            }
            cur_n = 0;
        } else if (cur_n < sizeof cur - 1) {
            cur[cur_n++] = c;
        }
    }
    t_last = now;
    restore_interrupts(irq);
    return n;
}

void usblog_init(void) {
    memset(buf, 0xFF, sizeof buf);
    len = flushed = 0;
    // RAM-only build: nothing runs from flash, so interrupts can stay on
    flash_range_erase(USBLOG_FLASH_OFF, USBLOG_SIZE);
    usblog_printf("midihello USB log: TinyUSB debug level %d\n", CFG_TUSB_DEBUG);
}

void usblog_flush(void) {
    uint32_t end = len;
    if (end == flushed || time_us_32() - t_last < QUIET_US) return;
    // from the start of the last partly written page: its old bytes are
    // programmed again with the same values, its new ones over 0xFF
    uint32_t from = flushed & ~(FLASH_PAGE_SIZE - 1u);
    uint32_t to = (end + FLASH_PAGE_SIZE - 1u) & ~(FLASH_PAGE_SIZE - 1u);
    flash_range_program(USBLOG_FLASH_OFF + from, (const uint8_t *)buf + from, to - from);
    flushed = end;
}

bool usblog_screen(char last1[33], char last2[33], char assert_line[33]) {
    uint32_t irq = save_and_disable_interrupts();
    bool changed = screen_changed;
    screen_changed = false;
    memcpy(last1, last[0], 33);
    memcpy(last2, last[1], 33);
    memcpy(assert_line, first_assert, 33);
    restore_interrupts(irq);
    return changed;
}
