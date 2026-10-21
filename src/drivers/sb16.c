#include "../include/sb16.h"
#include "../include/dma.h"
#include "../include/io.h"
#include "../include/utils.h"
#include "../include/serial.h"
#include "../include/spinlock.h"

// IDT registration
extern void register_interrupt_handler(uint8_t n, void* handler);

#define SB16_BASE       0x220
#define DSP_RESET       (SB16_BASE + 0x6)
#define DSP_READ        (SB16_BASE + 0xA)
#define DSP_WRITE       (SB16_BASE + 0xC)
#define DSP_READ_STAT   (SB16_BASE + 0xE)
#define DSP_INT_ACK_8   (SB16_BASE + 0xE)
#define DSP_INT_ACK_16  (SB16_BASE + 0xF)
#define DSP_MIXER_ADDR  (SB16_BASE + 0x4)
#define DSP_MIXER_DATA  (SB16_BASE + 0x5)

#define BUFFER_SIZE     32768
#define HALF_BUFFER     (BUFFER_SIZE / 2)

static int sb16_available = 0;

// DMA buffer — must be within first 16MB and not cross a 64KB boundary.
// We use a 32KB buffer aligned to 32KB which guarantees no 64KB crossing.
static uint8_t dma_buffer[BUFFER_SIZE] __attribute__((aligned(32768)));

static uint8_t* current_pcm_data = 0;
static uint32_t current_pcm_length = 0;
static uint32_t current_pcm_pos = 0;
static int current_half = 0;

// Extended playback state
static int playback_state = 0;  // SB16_STATE_STOPPED, PLAYING, PAUSED
static sb16_format_t current_format = {8, 1, 0, 22050};  // Default: 8-bit mono unsigned 22kHz

// sb16_lock serializes the PCM state above across its two contexts: the
// completion IRQ (fill_buffer + the half toggle) and task context
// (sb16_set_audio_buffer, and the prefill in sb16_start_playback). Process
// context takes it with spin_lock_irqsave, the IRQ handler with the plain
// spin_lock/unlock (the interrupt gate already cleared IF — house rule in
// spinlock.h); IRQ 5 is routed to one CPU, but the syscall can run on any.
// The scalars (sb16_available, current_volume) are lockless by design.
static spinlock_t sb16_lock = SPINLOCK_INIT;

static int dsp_write(uint8_t value) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(DSP_WRITE) & 0x80)) {
            outb(DSP_WRITE, value);
            return 1; // Success
        }
    }
    write_serial_string("[SB16] dsp_write timeout!\n");
    return 0; // Timeout
}

static int dsp_read_data(uint8_t* out) {
    for (int i = 0; i < 100000; i++) {
        if (inb(DSP_READ_STAT) & 0x80) {
            *out = inb(DSP_READ);
            return 1;
        }
    }
    return 0;
}

static int dsp_reset(void) {
    outb(DSP_RESET, 1);
    // Wait >3 microseconds
    for (volatile int i = 0; i < 1000; i++) __asm__ __volatile__ ("pause");
    outb(DSP_RESET, 0);
    
    // Wait for ready byte (0xAA) with timeout
    uint8_t val;
    for (int attempt = 0; attempt < 100; attempt++) {
        if (dsp_read_data(&val)) {
            if (val == 0xAA) return 1; // Success
        }
    }
    return 0; // Failed
}

// Fill half of the DMA buffer with PCM data
static void fill_buffer(int half) {
    uint8_t* dest = dma_buffer + (half * HALF_BUFFER);
    // Silence byte varies by format: 8-bit unsigned = 0x80, 16-bit unsigned =
    // 0x00 0x80, 16-bit signed = 0x00 0x00. Writing 0x80 into every byte of a
    // 16-bit sample yields 0x8080 — a full-scale DC buzz, not silence.
    uint8_t sil_lo, sil_hi;
    if (current_format.bits == 16) {
        if (current_format.is_signed) { sil_lo = 0x00; sil_hi = 0x00; }
        else                          { sil_lo = 0x00; sil_hi = 0x80; }
    } else {
        sil_lo = 0x80; sil_hi = 0x80;
    }
    for (int i = 0; i < HALF_BUFFER; i++) {
        if (current_pcm_data && current_pcm_length > 0) {
            dest[i] = current_pcm_data[current_pcm_pos++];
            if (current_pcm_pos >= current_pcm_length) {
                current_pcm_pos = 0; // Loop
            }
        } else if (current_format.bits == 16) {
            dest[i] = (i & 1) ? sil_hi : sil_lo; // high/low byte of 16-bit sample
        } else {
            dest[i] = sil_lo; // 8-bit
        }
    }
}

static void sb16_irq_handler(void* r) {
    (void)r;

    // IRQ context: the gate already cleared IF, so the plain spin_lock is
    // the house rule (see spinlock.h). The lock is what makes fill_buffer
    // read one coherent {pointer, length, pos} triple: without it a
    // concurrent sb16_set_audio_buffer could publish the new pointer with
    // the OLD length (up to the 2 MB SYS_PLAY_WAV cap) and the fill would
    // walk off the end of the new — possibly much smaller — buffer.
    spin_lock(&sb16_lock);

    // Acknowledge the completion interrupt. The ack port differs by width:
    // 8-bit auto-init asserts on DSP_INT_ACK_8 (0xE), 16-bit on DSP_INT_ACK_16
    // (0xF). Acknowledging the wrong port leaves the IRQ asserted so the card
    // keeps re-firing and the fill runs out of phase with DMA.
    if (current_format.bits == 16) {
        inb(DSP_INT_ACK_16);
    } else {
        inb(DSP_INT_ACK_8);
    }

    // Fill the half that just finished playing
    fill_buffer(current_half);
    current_half ^= 1;

    spin_unlock(&sb16_lock);
}

void sb16_init(void) {
    write_serial_string("[SB16] Initializing DSP...\n");
    if (!dsp_reset()) {
        write_serial_string("[SB16] DSP Reset FAILED. No sound card found.\n");
        sb16_available = 0;
        return;
    }
    write_serial_string("[SB16] DSP Reset OK (0xAA received).\n");
    
    // Set IRQ 5 on the mixer
    outb(DSP_MIXER_ADDR, 0x80); // IRQ select register
    outb(DSP_MIXER_DATA, 0x02); // bit 1 = IRQ 5
    
    // Set DMA channel on the mixer
    outb(DSP_MIXER_ADDR, 0x81); // DMA select register
    outb(DSP_MIXER_DATA, 0x02); // bit 1 = DMA 1 (8-bit)
    
    // Register IRQ 5 handler (Interrupt 37)
    register_interrupt_handler(37, sb16_irq_handler);
    write_serial_string("[SB16] IRQ 5 handler registered.\n");
    
    // Turn on the DSP speaker output
    dsp_write(0xD1);
    // Publish "available" LAST (publish race, audit): anything that sees it
    // may immediately drive the mixer/DMA/IRQ paths configured above, so the
    // flag must not be visible while the card is still half-initialized.
    sb16_available = 1;
    write_serial_string("[SB16] Speaker ON. Ready.\n");
}

// Publish a new PCM source for the completion IRQ. Under sb16_lock so the
// fields cross the task/IRQ boundary as one unit — the syscall's ordering
// guarantee ("point the IRQ at the new copy first, then free the old one")
// only holds if the swap is atomic against the handler.
uint8_t* sb16_swap_audio_buffer(uint8_t* pcm_data, uint32_t length) {
    uint32_t nf = spin_lock_irqsave(&sb16_lock);
    uint8_t* old = current_pcm_data;
    current_pcm_data = pcm_data;
    current_pcm_length = length;
    current_pcm_pos = 0;
    spin_unlock_irqrestore(&sb16_lock, nf);
    return old;
}

void sb16_set_audio_buffer(uint8_t* pcm_data, uint32_t length) {
    (void)sb16_swap_audio_buffer(pcm_data, length);
}

void sb16_stop_playback() {
    if (!sb16_available) return;

    // Exit auto-init mode and stop DMA
    if (current_format.bits == 16) {
        dsp_write(0xD9);  // Exit 16-bit auto-init mode
    } else {
        dsp_write(0xDA);  // Exit 8-bit auto-init mode
    }

    playback_state = 0; // SB16_STATE_STOPPED
    write_serial_string("[SB16] Playback stopped.\n");
}

void sb16_start_playback(uint16_t sample_rate) {
    if (!sb16_available) {
        write_serial_string("[SB16] Cannot play: no sound card.\n");
        return;
    }
    write_serial_string("[SB16] Starting 8-bit mono playback...\n");

    // Keep the active format in sync: if a prior sb16_start_playback_ex left
    // current_format as 16-bit, pause/stop would emit the 16-bit exit command
    // (0xD9/0xD5) against an 8-bit transfer and fail to stop it.
    current_format.bits = 8;
    current_format.channels = 1;
    current_format.is_signed = 0;
    current_format.sample_rate = sample_rate;

    // Pre-fill both halves of the DMA buffer. Under sb16_lock: fill_buffer
    // advances current_pcm_pos, which a completion IRQ from a previous start
    // also advances — prefill and IRQ must not interleave their cursors.
    uint32_t nf = spin_lock_irqsave(&sb16_lock);
    fill_buffer(0);
    fill_buffer(1);
    current_half = 0;
    spin_unlock_irqrestore(&sb16_lock, nf);
    
    // Program DMA Channel 1 for auto-init transfer
    // Mode: single, auto-init, increment, read from memory
    // 01 0 1 10 01 = 0x59 (channel bits included)
    // But dma_set_channel adds channel bits, so mode base = 0x58
    uint32_t buf_addr = (uint32_t)dma_buffer;
    write_serial_string("[SB16] DMA buffer at: ");
    write_serial_hex(buf_addr);
    write_serial_string("\n");
    
    dma_set_channel(1, 0x58, buf_addr, BUFFER_SIZE);
    write_serial_string("[SB16] DMA channel 1 configured.\n");

    // Turn on DSP speaker output (required before playback)
    dsp_write(0xD1);

    // Set output sample rate (SB16 command 0x41)
    dsp_write(0x41);
    dsp_write((sample_rate >> 8) & 0xFF);
    dsp_write(sample_rate & 0xFF);
    
    // Start 8-bit auto-init DMA playback (SB16 command 0xC6)
    // Format: command, mode, length_lo, length_hi
    dsp_write(0xC6);            // 8-bit auto-init output
    dsp_write(0x00);            // mode: unsigned mono
    uint16_t blk_len = HALF_BUFFER - 1;
    dsp_write(blk_len & 0xFF);
    dsp_write((blk_len >> 8) & 0xFF);
    
    playback_state = 1; // SB16_STATE_PLAYING

    write_serial_string("[SB16] Playback started at ");
    write_serial_hex(sample_rate);
    write_serial_string(" Hz.\n");
}

// --- Volume Control ---
// SB16 master volume: register 0x22, format: left[7:4] | right[3:0] (each 0-15)
static uint8_t current_volume = 80; // Default 80%
static sb16_mixer_t current_mixer = {80, 50, 50, 80}; // Default mixer settings

void sb16_set_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    current_volume = vol;
    // Keep the mixer cache coherent with the master-volume view so
    // sb16_get_mixer() does not report a stale master after set_volume().
    current_mixer.master = vol;

    if (!sb16_available) return;
    
    // Map 0-100 to 0-15 for SB16 mixer
    uint8_t hw_vol = (vol * 15) / 100;
    uint8_t mixer_val = (hw_vol << 4) | hw_vol; // Same for L+R
    
    outb(DSP_MIXER_ADDR, 0x22); // Master volume register
    outb(DSP_MIXER_DATA, mixer_val);
    
    write_serial_string("[SB16] Volume set to ");
    write_serial_hex(vol);
    write_serial_string("%\n");
}

uint8_t sb16_get_volume(void) {
    return current_volume;
}

int sb16_is_available(void) {
    return sb16_available;
}

// --- Extended Playback Functions ---

void sb16_pause_playback(void) {
    if (!sb16_available || playback_state != 1) return;

    // Pause DMA transfer (0xD0 for 8-bit, 0xD5 for 16-bit)
    if (current_format.bits == 16) {
        dsp_write(0xD5);
    } else {
        dsp_write(0xD0);
    }

    playback_state = 2; // SB16_STATE_PAUSED
    write_serial_string("[SB16] Playback paused.\n");
}

void sb16_resume_playback(void) {
    if (!sb16_available || playback_state != 2) return;

    // Resume DMA transfer (0xD4 for 8-bit, 0xD6 for 16-bit)
    if (current_format.bits == 16) {
        dsp_write(0xD6);
    } else {
        dsp_write(0xD4);
    }

    playback_state = 1; // SB16_STATE_PLAYING
    write_serial_string("[SB16] Playback resumed.\n");
}

int sb16_get_state(void) {
    return playback_state;
}

uint32_t sb16_get_position(void) {
    return current_pcm_pos;
}

// Start playback with extended format support (16-bit, stereo)
void sb16_start_playback_ex(sb16_format_t* format) {
    if (!sb16_available) {
        write_serial_string("[SB16] Cannot play: no sound card.\n");
        return;
    }

    if (!format) {
        // Fall back to default format
        sb16_start_playback(22050);
        return;
    }

    // Validate into a local copy: the DSP state machine (current_format) and
    // the caller's struct are kept distinct, so a malformed format never
    // mutates the caller's data as a side effect.
    sb16_format_t fmt = *format;
    if (fmt.bits != 8 && fmt.bits != 16) {
        write_serial_string("[SB16] Invalid bit depth, using 8-bit.\n");
        fmt.bits = 8;
    }
    if (fmt.channels != 1 && fmt.channels != 2) {
        write_serial_string("[SB16] Invalid channels, using mono.\n");
        fmt.channels = 1;
    }

    // Save current format
    current_format = fmt;

    write_serial_string("[SB16] Starting playback: ");
    write_serial_hex(fmt.bits);
    write_serial_string("-bit, ");
    write_serial_string(fmt.channels == 2 ? "stereo" : "mono");
    write_serial_string(", ");
    write_serial_hex(fmt.sample_rate);
    write_serial_string(" Hz\n");

    // Pre-fill both halves of the DMA buffer
    uint32_t nf = spin_lock_irqsave(&sb16_lock);
    fill_buffer(0);
    fill_buffer(1);
    current_half = 0;
    spin_unlock_irqrestore(&sb16_lock, nf);

    uint32_t buf_addr = (uint32_t)dma_buffer;

    if (fmt.bits == 16) {
        // 16-bit playback uses DMA channel 5
        write_serial_string("[SB16] Using 16-bit DMA (channel 5)\n");
        dma_set_channel(5, 0x58, buf_addr, BUFFER_SIZE);

        // Turn on DSP speaker
        dsp_write(0xD1);

        // Set sample rate (command 0x41)
        dsp_write(0x41);
        dsp_write((fmt.sample_rate >> 8) & 0xFF);
        dsp_write(fmt.sample_rate & 0xFF);

        // Start 16-bit auto-init DMA playback (command 0xB6). The mode byte
        // bits per the SB16 spec: 0x08 = signed, 0x10 = 16-bit (required),
        // 0x20 = stereo. Setting 0x10 for the signed flag would be a no-op
        // (the 16-bit bit is already set) — the dead flag bug.
        dsp_write(0xB6);
        uint8_t mode = 0x10; // 16-bit
        if (fmt.channels == 2) mode |= 0x20; // Stereo
        if (fmt.is_signed) mode |= 0x08;     // Signed
        dsp_write(mode);

        uint16_t blk_len = (HALF_BUFFER / 2) - 1; // 16-bit transfers
        dsp_write(blk_len & 0xFF);
        dsp_write((blk_len >> 8) & 0xFF);
    } else {
        // 8-bit playback (existing code path)
        dma_set_channel(1, 0x58, buf_addr, BUFFER_SIZE);

        // Turn on DSP speaker
        dsp_write(0xD1);

        // Set sample rate
        dsp_write(0x41);
        dsp_write((fmt.sample_rate >> 8) & 0xFF);
        dsp_write(fmt.sample_rate & 0xFF);

        // Start 8-bit auto-init DMA playback
        dsp_write(0xC6);
        uint8_t mode = 0x00; // 8-bit mono unsigned
        if (fmt.channels == 2) mode |= 0x20; // Stereo
        dsp_write(mode);

        uint16_t blk_len = HALF_BUFFER - 1;
        dsp_write(blk_len & 0xFF);
        dsp_write((blk_len >> 8) & 0xFF);
    }

    playback_state = 1; // SB16_STATE_PLAYING
    write_serial_string("[SB16] Playback started.\n");
}

// --- Mixer Control Functions ---

void sb16_set_mixer(sb16_mixer_t* mixer) {
    if (!sb16_available || !mixer) return;

    current_mixer = *mixer;
    // Keep the master-volume cache coherent: sb16_get_volume() must reflect
    // the mixer master it just applied, not a stale value.
    current_volume = (mixer->master > 100) ? 100 : mixer->master;

    // Master volume (0x22)
    uint8_t master_hw = (mixer->master * 15) / 100;
    uint8_t master_val = (master_hw << 4) | master_hw;
    outb(DSP_MIXER_ADDR, 0x22);
    outb(DSP_MIXER_DATA, master_val);

    // Bass (0x46)
    uint8_t bass_hw = (mixer->bass * 15) / 100;
    uint8_t bass_val = (bass_hw << 4) | bass_hw;
    outb(DSP_MIXER_ADDR, 0x46);
    outb(DSP_MIXER_DATA, bass_val);

    // Treble (0x44)
    uint8_t treble_hw = (mixer->treble * 15) / 100;
    uint8_t treble_val = (treble_hw << 4) | treble_hw;
    outb(DSP_MIXER_ADDR, 0x44);
    outb(DSP_MIXER_DATA, treble_val);

    // PCM volume (0x04)
    uint8_t pcm_hw = (mixer->pcm * 15) / 100;
    uint8_t pcm_val = (pcm_hw << 4) | pcm_hw;
    outb(DSP_MIXER_ADDR, 0x04);
    outb(DSP_MIXER_DATA, pcm_val);

    write_serial_string("[SB16] Mixer updated: Master=");
    write_serial_hex(mixer->master);
    write_serial_string(", Bass=");
    write_serial_hex(mixer->bass);
    write_serial_string(", Treble=");
    write_serial_hex(mixer->treble);
    write_serial_string(", PCM=");
    write_serial_hex(mixer->pcm);
    write_serial_string("\n");
}

void sb16_get_mixer(sb16_mixer_t* mixer) {
    if (!mixer) return;
    *mixer = current_mixer;
}
