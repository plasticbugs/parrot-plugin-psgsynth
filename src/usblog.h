// usblog.h -- TinyUSB's debug log (CFG_TUSB_DEBUG_PRINTF), kept three ways:
// in RAM, the last lines and the first failed assertion for the screen, and
// a copy in flash that survives a reset, read back from the Mac with
//
//   picotool save -r 0x10FC0000 0x10FD0000 usblog.bin -f && picotool reboot
//
// (text, each line prefixed with milliseconds since boot; it ends at the
// first 0xFF). midihello is a RAM-only build, so programming flash while it
// runs is safe: no code runs from flash. The area is 256K below the top of
// the 16 MB flash, below cvcart_revc's boot records, far above sdplayer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define USBLOG_FLASH_OFF  0xFC0000u     // offset in flash (XIP 0x10FC0000)
#define USBLOG_SIZE       0x10000u

// Erase the flash area and start the log (takes up to a second or two).
void usblog_init(void);

// The hook TinyUSB prints through (tusb_config.h). Safe from interrupts.
int usblog_printf(const char *fmt, ...);

// Program what's new into flash, once the log has been quiet for a while.
void usblog_flush(void);

// For the screen: the last two complete lines, the first assertion, and
// whether any of them changed since the last call.
bool usblog_screen(char last1[33], char last2[33], char assert_line[33]);
