The plugin now sees a USB music keyboard on a powered hub that was already plugged in when it started, or that is plugged in or pulled out while it runs. Some hubs never report these, such as an Anker hub with a Genesys GL3523 chip.

It now uses the TinyUSB 0.21 in `sdk/`, so you no longer need the pico-sdk's TinyUSB submodule to build it.
