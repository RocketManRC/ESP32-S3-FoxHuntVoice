#!/usr/bin/env python3
"""
Convert a WAV file to the speech.raw cache format used by FoxHuntVoice.

Format: 4 little-endian int32 header [SPEECH_VERSION, sample_rate, num_samples, num_channels]
        followed by raw int16 PCM samples.

Usage:
    python3 scripts/wav2speech.py input.wav [--version N] [--resample RATE]

Output is written to data/speech.raw, ready for SPIFFS upload via:
    pio run --target uploadfs
"""

import argparse
import struct
import sys

def read_wav(input_path):
    """Read WAV file, handling both PCM (format 1) and float (format 3)."""
    with open(input_path, 'rb') as f:
        # Parse RIFF header
        riff = f.read(12)
        if riff[:4] != b'RIFF' or riff[8:12] != b'WAVE':
            print("Error: not a valid WAV file")
            sys.exit(1)

        fmt_tag = None
        channels = None
        sample_rate = None
        bits_per_sample = None
        raw = None

        while True:
            chunk_hdr = f.read(8)
            if len(chunk_hdr) < 8:
                break
            chunk_id = chunk_hdr[:4]
            chunk_size = struct.unpack('<I', chunk_hdr[4:8])[0]

            if chunk_id == b'fmt ':
                fmt_data = f.read(chunk_size)
                fmt_tag = struct.unpack('<H', fmt_data[0:2])[0]
                channels = struct.unpack('<H', fmt_data[2:4])[0]
                sample_rate = struct.unpack('<I', fmt_data[4:8])[0]
                bits_per_sample = struct.unpack('<H', fmt_data[14:16])[0]
            elif chunk_id == b'data':
                raw = f.read(chunk_size)
            else:
                f.read(chunk_size)
            # Chunks are word-aligned
            if chunk_size % 2 == 1:
                f.read(1)

    if fmt_tag is None or raw is None:
        print("Error: missing fmt or data chunk")
        sys.exit(1)

    return fmt_tag, channels, sample_rate, bits_per_sample, raw

def convert(input_path, output_path, speech_version, target_rate):
    fmt_tag, channels, sample_rate, bits_per_sample, raw = read_wav(input_path)

    num_frames = len(raw) // (channels * (bits_per_sample // 8))
    print(f"Input: {input_path}")
    print(f"  Format: {fmt_tag} ({'PCM' if fmt_tag == 1 else 'float' if fmt_tag == 3 else 'unknown'})")
    print(f"  {channels} ch, {bits_per_sample}-bit, {sample_rate} Hz, {num_frames} frames")
    print(f"  Duration: {num_frames / sample_rate:.2f}s")

    # Convert to int16 mono
    if fmt_tag == 3:
        # IEEE float
        if bits_per_sample == 32:
            all_samples = struct.unpack(f'<{len(raw) // 4}f', raw)
        elif bits_per_sample == 64:
            all_samples = struct.unpack(f'<{len(raw) // 8}d', raw)
        else:
            print(f"Error: unsupported float bit depth {bits_per_sample}")
            sys.exit(1)
        if channels == 1:
            samples = [max(-32768, min(32767, int(s * 32767))) for s in all_samples]
        else:
            samples = []
            for i in range(0, len(all_samples), channels):
                mix = sum(all_samples[i:i+channels]) / channels
                samples.append(max(-32768, min(32767, int(mix * 32767))))
    elif fmt_tag == 1:
        # Integer PCM
        sample_width = bits_per_sample // 8
        if sample_width == 1:
            samples = [struct.unpack('B', raw[i:i+1])[0] for i in range(0, len(raw), channels)]
            samples = [(s - 128) * 256 for s in samples]
        elif sample_width == 2:
            all_samples = struct.unpack(f'<{num_frames * channels}h', raw)
            if channels == 1:
                samples = list(all_samples)
            else:
                samples = []
                for i in range(0, len(all_samples), channels):
                    mix = sum(all_samples[i:i+channels]) // channels
                    samples.append(max(-32768, min(32767, mix)))
        elif sample_width == 3:
            samples = []
            step = 3 * channels
            for i in range(0, len(raw), step):
                b = raw[i:i+3]
                val = int.from_bytes(b, 'little', signed=True)
                samples.append(max(-32768, min(32767, val >> 8)))
        elif sample_width == 4:
            all_samples = struct.unpack(f'<{num_frames * channels}i', raw)
            if channels == 1:
                samples = [max(-32768, min(32767, s >> 16)) for s in all_samples]
            else:
                samples = []
                for i in range(0, len(all_samples), channels):
                    mix = sum(all_samples[i:i+channels]) // channels
                    samples.append(max(-32768, min(32767, mix >> 16)))
        else:
            print(f"Error: unsupported sample width {sample_width} bytes")
            sys.exit(1)
    else:
        print(f"Error: unsupported WAV format tag {fmt_tag}")
        sys.exit(1)

    # Resample if needed
    if target_rate and target_rate != sample_rate:
        print(f"  Resampling {sample_rate} -> {target_rate} Hz")
        ratio = target_rate / sample_rate
        new_len = int(len(samples) * ratio)
        resampled = []
        for i in range(new_len):
            src = i / ratio
            idx = int(src)
            frac = src - idx
            if idx + 1 < len(samples):
                val = samples[idx] * (1 - frac) + samples[idx + 1] * frac
            else:
                val = samples[idx] if idx < len(samples) else 0
            resampled.append(max(-32768, min(32767, int(val))))
        samples = resampled
        sample_rate = target_rate

    num_samples = len(samples)
    num_channels = 1

    # Write output
    header = struct.pack('<4i', speech_version, sample_rate, num_samples, num_channels)
    pcm_data = struct.pack(f'<{num_samples}h', *samples)

    with open(output_path, 'wb') as f:
        f.write(header)
        f.write(pcm_data)

    file_size = len(header) + len(pcm_data)
    print(f"\nOutput: {output_path}")
    print(f"  SPEECH_VERSION={speech_version}, {sample_rate} Hz, {num_samples} samples, mono")
    print(f"  Duration: {num_samples / sample_rate:.2f}s")
    print(f"  File size: {file_size:,} bytes ({file_size / 1024:.1f} KB)")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Convert WAV to FoxHuntVoice speech.raw format')
    parser.add_argument('input', help='Input WAV file')
    parser.add_argument('-o', '--output', default='data/speech.raw', help='Output file (default: data/speech.raw)')
    parser.add_argument('-v', '--version', type=int, default=12, help='SPEECH_VERSION value (default: 12)')
    parser.add_argument('-r', '--resample', type=int, default=16000, help='Target sample rate in Hz (default: 16000)')
    args = parser.parse_args()

    convert(args.input, args.output, args.version, args.resample)
