# PSG Synth

This plugin turns the ColecoVision's sound chip into a small synth that you play from a USB MIDI keyboard. It has nine sounds and plays three notes at a time, plus noise. It doesn't open any files.

## Setup

Copy `PSGSYNTH.CVP` into the `/PLUGINS` folder on the card. Start PSG Synth from the Plugins page in the menu.

The cart can't power a keyboard. Plug the keyboard into a USB-C hub with its charger plugged in, or into a powered USB-A hub on an adapter. Then plug the hub into the cart.

## Controls

On the ColecoVision controller:

- Keypad 1 to 9 picks the sound. The sounds are lead, organ, pluck, pad, major arpeggio, minor arpeggio, buzz bass, zap and drums.
- Keypad * silences every note.
- Keypad 0 or left fire starts and stops the MIDI clock.
- Keypad # opens the info page, with the USB details and the controls. Press it again to go back.
- Push the joystick up or down to change the tempo by 1. Hold it to keep going.
- Push the joystick right or left to change the tempo by 10.
- Press RESET on the console to go back to the menu.

On the MIDI keyboard:

- The keys play notes, and how hard you play sets the volume.
- The pitch bend wheel bends notes by up to 2 semitones.
- Program change 0 to 8 picks a sound.
- With the drums sound, each key plays a drum. C is the kick, D is the snare and F# is the closed hi-hat, in every octave.

## Notes

- The cart sends MIDI clock to the keyboard. The clock starts at 120 BPM when the keyboard is found, and the tempo goes from 30 to 300 BPM.
- The beat light at the top of the screen blinks on each beat while the clock runs. It's red on the first beat of each bar.
- For the keyboard's arpeggiator or sequencer to follow the cart, set its sync source to USB.
- Notes below A2 play an octave up.
- The second line of the screen shows the keyboard's name, or what to plug in. It also says whether the clock is playing.

## Credits

- TinyUSB by Ha Thach, under the MIT license. The source is at https://github.com/hathach/tinyusb
- The plugin itself is by Parrot Cart.
