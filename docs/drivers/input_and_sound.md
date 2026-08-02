# Input Devices & Audio Subsystem

Mectov OS supports standard PS/2 user input hardware and legacy PC audio interfaces.

---

## ⌨️ PS/2 Keyboard Driver (`src/drivers/keyboard.c`)

1. **Interrupt Vector 33 (IRQ 1)**:
   - Captures scan codes from I/O port `0x60`.
   - Translates US QWERTY scan codes to ASCII characters.
   - Manages Shift, Ctrl, Alt, and Caps Lock modifier key states.
   - Pushes processed characters into circular ring buffer for shell and GUI applications.

---

## 🖱️ PS/2 Mouse Driver (`src/drivers/mouse.c`)

1. **Interrupt Vector 44 (IRQ 12)**:
   - Configures PS/2 mouse via Auxiliary Device Command on I/O port `0x64` / `0x60`.
   - Processes 3-byte movement packets (X delta, Y delta, Left/Right/Middle button states).
   - Enforces screen boundary constraints (`0` to `1023` X, `0` to `767` Y).

---

## 🔊 Audio Subsystem (`src/drivers/speaker.c` & `src/drivers/sb16.c`)

1. **PC Speaker (`speaker.c`)**:
   - Uses Programmable Interval Timer (PIT) Channel 2 connected to I/O ports `0x42` and `0x61`.
   - Generates square-wave audio frequencies for system sound effects (`nada()`) and startup chime.

2. **Sound Blaster 16 (`sb16.c`)**:
   - Initializes SB16 DSP via I/O base port `0x220`.
   - Supports 8-bit / 16-bit PCM audio playback via ISA DMA controller (`src/drivers/dma.c`).
