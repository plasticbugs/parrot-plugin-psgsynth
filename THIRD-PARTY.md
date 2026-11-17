# What in this repository came from elsewhere

The plugin's own code, in `src/`, is under the MIT license (`LICENSE`).
The copy of the cart's SDK in `sdk/` is under the MIT license
(`sdk/LICENSE`). These parts came from elsewhere and keep their own terms.

| Where | What | License |
|---|---|---|
| `sdk/firmware/revc/midihello/tinyusb_fix/hub.c` | TinyUSB 0.18's hub driver with one fix | MIT (its header) |
| `cmake/pico_sdk_import.cmake` | from the [Raspberry Pi Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause |

The built plugin also contains the Raspberry Pi Pico SDK's runtime (BSD
3-Clause) and the pico-sdk's own TinyUSB 0.18 (MIT).

The 8x8 screen font's letter shapes come from the 1982 arcade game Mr. Do!
