import wave
import struct
import math

# WAV settings
SAMPLE_RATE = 11025
DURATION = 5  # seconds
NUM_SAMPLES = SAMPLE_RATE * DURATION

# Simple melody (Super Mario theme like notes: E E E C E G G)
# Notes in Hz
notes = [
    (329.63, 0.2), # E
    (0, 0.05),
    (329.63, 0.2), # E
    (0, 0.05),
    (329.63, 0.2), # E
    (0, 0.1),
    (261.63, 0.2), # C
    (0, 0.05),
    (329.63, 0.2), # E
    (0, 0.1),
    (392.00, 0.4), # G
    (0, 0.4),
    (196.00, 0.4), # G (low)
]

def generate_melody():
    pcm_data = []
    
    # 8-bit unsigned PCM is 0-255, center is 128
    for freq, duration in notes:
        num_note_samples = int(SAMPLE_RATE * duration)
        for i in range(num_note_samples):
            if freq == 0:
                pcm_data.append(128)
            else:
                # generate sine wave
                val = int(128 + 127 * math.sin(2 * math.pi * freq * i / SAMPLE_RATE))
                pcm_data.append(val)
                
    # Pad the rest with silence (128)
    while len(pcm_data) < NUM_SAMPLES:
        pcm_data.append(128)
        
    return pcm_data

with wave.open("apps/music.wav", "wb") as wav_file:
    # 1 channel (mono), 1 byte per sample (8-bit), 11025 Hz
    wav_file.setnchannels(1)
    wav_file.setsampwidth(1)
    wav_file.setframerate(SAMPLE_RATE)
    
    pcm_data = generate_melody()
    
    # pack into bytes
    byte_data = struct.pack(f'{len(pcm_data)}B', *pcm_data)
    wav_file.writeframes(byte_data)

print("apps/music.wav generated successfully!")
