#include "../include/sb16.h"
#include "../include/dma.h"
#include "../include/io.h"
#include "../include/utils.h"
#include "../include/serial.h"

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
    for (int i = 0; i < HALF_BUFFER; i++) {
        if (current_pcm_data && current_pcm_length > 0) {
            dest[i] = current_pcm_data[current_pcm_pos++];
            if (current_pcm_pos >= current_pcm_length) {
                current_pcm_pos = 0; // Loop
            }
        } else {
            dest[i] = 128; // Silence (8-bit unsigned center)
        }
    }
}

static void sb16_irq_handler(void* r) {
    (void)r;
    
    // Acknowledge 8-bit interrupt
    inb(DSP_INT_ACK_8);
    
    // Fill the half that just finished playing
    fill_buffer(current_half);
    current_half ^= 1;
}

void sb16_init(void) {
    write_serial_string("[SB16] Initializing DSP...\n");
    if (!dsp_reset()) {
        write_serial_string("[SB16] DSP Reset FAILED. No sound card found.\n");
        sb16_available = 0;
        return;
    }
    sb16_available = 1;
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
    write_serial_string("[SB16] Speaker ON. Ready.\n");
}

void sb16_set_audio_buffer(uint8_t* pcm_data, uint32_t length) {
    current_pcm_data = pcm_data;
    current_pcm_length = length;
    current_pcm_pos = 0;
}

void sb16_stop_playback() {
    if (!sb16_available) return;
    // Halt 8-bit DMA
    dsp_write(0xD0);
    // Unmask DMA channel just to be safe
    // Actually D0 just pauses it
    write_serial_string("[SB16] Playback stopped/paused.\n");
}

void sb16_start_playback(uint16_t sample_rate) {
    if (!sb16_available) {
        write_serial_string("[SB16] Cannot play: no sound card.\n");
        return;
    }
    write_serial_string("[SB16] Starting 8-bit mono playback...\n");
    
    // Pre-fill both halves of the DMA buffer
    fill_buffer(0);
    fill_buffer(1);
    current_half = 0;
    
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
    
    write_serial_string("[SB16] Playback started at ");
    write_serial_hex(sample_rate);
    write_serial_string(" Hz.\n");
}

// --- Volume Control ---
// SB16 master volume: register 0x22, format: left[7:4] | right[3:0] (each 0-15)
static uint8_t current_volume = 80; // Default 80%

void sb16_set_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    current_volume = vol;
    
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
