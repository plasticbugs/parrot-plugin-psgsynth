# PSG Synth: the cart as a USB MIDI host. A keyboard plays the console's
# sound chip (the PSG), and the cart sends the keyboard MIDI clock and draws
# the screen. Two builds of the same code, both no_flash binaries (they run
# from SRAM):
#
#   midihello   picotool loads it into SRAM and runs it once
#   psgsynth    the plugin: build/PSGSYNTH.CVP, for the card's /PLUGINS
#               folder. The console's RESET goes back to the menu
#               (PSG_SYNTH_CHAINED, handoff.asm)
#
#   cmake -B build -G Ninja -DPICO_SDK_PATH=$HOME/pico-sdk
#   ninja -C build                      -> build/PSGSYNTH.CVP
#
# PSG Synth was the cart's first plugin, and its screen kernel became the
# SDK's: midihello.asm, handoff.asm, vdp.c, pad.c and their headers are the
# SDK's copies (sdk/firmware/revc/midihello). src/ is the rest. Its USB is
# the pico-sdk's own TinyUSB (0.18). Needs sjasmplus (the kernel) and
# python3 (the pin order, from sdk/hw3/pins.py).
cmake_minimum_required(VERSION 3.13)

# the cart's SDK: the copy in sdk/ (sdk/README.md)
set(PARROTCART_SDK ${CMAKE_CURRENT_LIST_DIR}/sdk/firmware/revc)
set(PICO_BOARD_HEADER_DIRS ${PARROTCART_SDK}/cvcart_revc/boards)
set(PICO_BOARD cvcart_revc)
set(PICO_PLATFORM rp2350-arm-s)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# TinyUSB's log level (CFG_TUSB_DEBUG): 1 = errors and asserts; 2 adds
# enumeration and every transfer (too much once the MIDI clock runs)
set(LOG 1)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(midihello C CXX ASM)
pico_sdk_init()

# The same source makes the same file, whenever it's built: the compiler's
# __DATE__ and __TIME__ (the build date in the binary info) come from
# SOURCE_DATE_EPOCH, fixed here at the tested build's, 2026-11-15 19:28:57 UTC.
set(BUILD_EPOCH 1794770937)
set(CMAKE_C_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})
set(CMAKE_CXX_COMPILER_LAUNCHER ${CMAKE_COMMAND} -E env SOURCE_DATE_EPOCH=${BUILD_EPOCH})

set(MH ${PARROTCART_SDK}/midihello)
set(GEN ${CMAKE_CURRENT_BINARY_DIR})
add_custom_command(
    OUTPUT ${GEN}/mhkernel.bin ${GEN}/mhkernel.sym
    COMMAND sjasmplus --nologo --sym=${GEN}/mhkernel.sym ${MH}/midihello.asm
    WORKING_DIRECTORY ${GEN}                # SAVEBIN writes to the cwd
    DEPENDS ${MH}/midihello.asm)
add_custom_command(
    OUTPUT ${GEN}/rom_image.h
    COMMAND python3 ${PARROTCART_SDK}/bin2h_revc.py ${GEN}/mhkernel.bin ${GEN}/rom_image.h
    DEPENDS ${GEN}/mhkernel.bin ${PARROTCART_SDK}/bin2h_revc.py
            ${PARROTCART_SDK}/../../hw3/pins.py)

add_custom_command(
    OUTPUT ${GEN}/handoff.bin
    COMMAND sjasmplus --nologo ${MH}/handoff.asm
    WORKING_DIRECTORY ${GEN}
    DEPENDS ${MH}/handoff.asm)
add_custom_command(
    OUTPUT ${GEN}/handoff.h
    COMMAND python3 ${PARROTCART_SDK}/sdplayer/bin2c.py ${GEN}/handoff.bin handoff_bin ${GEN}/handoff.h
    DEPENDS ${GEN}/handoff.bin ${PARROTCART_SDK}/sdplayer/bin2c.py)

function(psg_synth TARGET)
  add_executable(${TARGET} src/midihello.c src/midi_host.c src/usbmidi.c src/usblog.c src/synth.c ${MH}/pad.c src/screen.c src/ui.c ${MH}/vdp.c
                 ${GEN}/rom_image.h ${GEN}/handoff.h)
  # src/ first (its tusb_config.h), then the SDK's midihello: the kernel's
  # headers, and midihello.c's "../sdplayer/htrace.h"
  target_include_directories(${TARGET} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src ${MH} ${GEN})
  target_link_libraries(${TARGET} pico_stdlib pico_multicore hardware_flash hardware_watchdog tinyusb_host)
  # TinyUSB 0.18's hub driver asks for the hub descriptor with wValue 0, which a
  # VIA VL817 hub stalls: build tinyusb_fix/hub.c (the same file, fixed) instead.
  target_sources(${TARGET} PRIVATE ${MH}/tinyusb_fix/hub.c)
  pico_set_binary_type(${TARGET} no_flash)
  pico_enable_stdio_usb(${TARGET} 0)        # the USB port is a host now
  pico_enable_stdio_uart(${TARGET} 0)
endfunction()

psg_synth(midihello)
pico_add_extra_outputs(midihello)

psg_synth(psgsynth)
target_compile_definitions(psgsynth PRIVATE PSG_SYNTH_CHAINED=1)
# the bootrom copies it from flash into SRAM when the menu starts it; the
# UF2 says 0x10E00000, and the load map is relative
pico_package_uf2_output(psgsynth 0x10E00000)
pico_add_extra_outputs(psgsynth)

# build/PSGSYNTH.CVP, the plugin file for the card's /PLUGINS folder:
# psgsynth.uf2 through mkcvp.py, with CART-README.md as the README its
# details page on the cart shows (sdk/cvreadme.py converts it).
set(SYNTH_README ${CMAKE_CURRENT_LIST_DIR}/CART-README.md)
set(SYNTH_ICON ${CMAKE_CURRENT_LIST_DIR}/src/psgsynth_icon.txt)
set(MKCVP ${PARROTCART_SDK}/sdplayer/mkcvp.py)
add_custom_command(
    OUTPUT ${GEN}/PSGSYNTH.CVP
    COMMAND python3 ${MKCVP} ${GEN}/psgsynth.uf2 ${GEN}/PSGSYNTH.CVP
            --id parrotcart.psgsynth --name "PSG Synth" --version 1.0.0 --by "Parrot Cart"
            --uses usb-host --icon ${SYNTH_ICON} --readme ${SYNTH_README}
    DEPENDS psgsynth ${MKCVP} ${PARROTCART_SDK}/sdk/cvreadme.py ${SYNTH_ICON} ${SYNTH_README}
    COMMENT "Plugin file PSGSYNTH.CVP"
    VERBATIM)
add_custom_target(psgsynth_cvp ALL DEPENDS ${GEN}/PSGSYNTH.CVP)

# CFG_TUH_MIDI (tusb_config.h) makes usbh.c name two Audio class constants
# that this TinyUSB (0.18) only declares in audio.h, which a host build never
# includes; give usbh.c that header.
set_source_files_properties(${PICO_TINYUSB_PATH}/src/host/usbh.c
    TARGET_DIRECTORY midihello psgsynth PROPERTIES COMPILE_OPTIONS "-include;class/audio/audio.h")
set_source_files_properties(${PICO_TINYUSB_PATH}/src/host/hub.c
    TARGET_DIRECTORY midihello psgsynth PROPERTIES HEADER_FILE_ONLY TRUE)
