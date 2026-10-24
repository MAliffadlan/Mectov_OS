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
   - `nada()` remains the blocking path used by shell builtins. Ring 3 sound effects use `SYS_PLAY_SOUND` (34) instead: EBX is frequency in Hz (20–20000), ECX is duration in milliseconds (0–5000), and EAX returns 0 when accepted or -1 when an argument is out of range. A zero-length tone is accepted as immediate silence. `sys_play_sound()` returns this verdict so callers can distinguish acceptance from refusal.
   - `speaker_tone()` starts the pitch without waiting for the note to finish. The BSP timer interrupt calls `speaker_tick()` after advancing the millisecond clock; that tick clears the speaker gate when the deadline has passed. This keeps a game loop responsive during a long sound effect. A signed deadline difference handles the 32-bit counter wrap in both directions.
   - The non-blocking tone's port programming and deadline publication are serialized by `tone_lock`: task context takes `spin_lock_irqsave()`, and the BSP timer IRQ takes the plain `spin_lock()` under the house IRQ-locking rule. This prevents a tick from retiring a replacement pitch using the previous note's deadline. The lock is never held while waiting for a note. Concurrent notes are not queued: a later `speaker_tone()` replaces the pending note.
   - `nada()`/`beep()` still use their blocking `play_sound()` → `delay()` → `nosound()` sequence, but call `speaker_cancel()` first so an older non-blocking deadline cannot fire after they take ownership of the shared PIT/gate. The cancellation helper clears only deadline state; it does not change the gate and does not hold the lock across the busy wait.
   - The frequency floor is not just cosmetic: `play_sound()` divides 1193180 by the requested frequency, and frequencies below about 19 Hz would exceed the PIT's 16-bit divisor. The ABI accepts 20 Hz and above.
   - Two gates cover different evidence. `scripts/spk_test.py` boots `apps/spk_test.mct` and checks the Ring 3 argument verdicts and non-blocking call timing; it does **not** prove audible output. `scripts/spk_deadline_test.py` compiles the shipped speaker driver against a fake PIT and controllable clock, checking the programmed divisor, both deadline-wrap directions, the replacement/IRQ interleaving, and legacy deadline cancellation. Its RED recipes exercise a naive unsigned comparison and a lock-free staged driver. It runs on the host without QEMU; its IRQ model is deterministic single-CPU preemption, **not** an SMP stress test or an audio-device emulation.

2. **Sound Blaster 16 (`sb16.c`)**:
   - Initializes SB16 DSP via I/O base port `0x220`.
   - Supports 8-bit / 16-bit PCM audio playback via ISA DMA controller (`src/drivers/dma.c`).
