#!/usr/bin/env python3
"""Send a mono 16 kHz PCM WAV to the Pico in denoise, unity or echo mode."""

import argparse
import math
import sys
import time
import wave
from pathlib import Path

try:
    import serial
except ImportError:
    print("Missing dependency: install it with 'python -m pip install pyserial'.")
    raise SystemExit(2)

SAMPLE_RATE = 16000
HOP_SAMPLES = 256
BYTES_PER_HOP = HOP_SAMPLES * 2


def read_line_until(port, expected_prefix, timeout_seconds=20):
    """Read text lines until one starts with the requested marker."""
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        raw = port.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        print(f"Pico: {line}")
        if line.startswith(expected_prefix):
            return line
    raise TimeoutError(f"Timed out waiting for Pico message: {expected_prefix}")


def read_exact(port, count):
    """Read exactly count bytes; serial reads may return partial data."""
    result = bytearray()
    while len(result) < count:
        part = port.read(count - len(result))
        if not part:
            raise TimeoutError("Timed out while receiving audio from the Pico")
        result.extend(part)
    return bytes(result)


def open_port(port_name):
    # CDC uses USB packets; the baud value is a conventional serial setting.
    port = serial.Serial(port_name, baudrate=115200, timeout=5, write_timeout=10)
    time.sleep(0.2)

    # READY may have been printed before this script opened the port.
    # A ping makes startup/reconnection independent of that old message.
    port.reset_input_buffer()
    port.write(b"P\n")
    read_line_until(port, "PONG", timeout_seconds=20)
    read_line_until(port, "READY", timeout_seconds=10)
    return port


def run_benchmark(port):
    port.write(b"B\n")
    read_line_until(port, "BENCHMARK_DONE", timeout_seconds=60)
    read_line_until(port, "READY", timeout_seconds=10)


def process_wav(port, input_path, output_path, mode):
    with wave.open(str(input_path), "rb") as wav_in:
        if wav_in.getcomptype() != "NONE":
            raise ValueError("Input WAV must be uncompressed PCM.")
        if wav_in.getnchannels() != 1:
            raise ValueError("Input WAV must be mono. Convert stereo to mono first.")
        if wav_in.getsampwidth() != 2:
            raise ValueError("Input WAV must use 16-bit samples (PCM s16le).")
        if wav_in.getframerate() != SAMPLE_RATE:
            raise ValueError("Input WAV must be 16000 Hz. Resample it first.")
        original_frames = wav_in.getnframes()
        audio = wav_in.readframes(original_frames)
        original_audio = audio

    if original_frames == 0:
        raise ValueError("Input WAV contains no samples.")

    hops = math.ceil(original_frames / HOP_SAMPLES)
    padded_length = hops * BYTES_PER_HOP
    audio = audio.ljust(padded_length, b"\x00")

    command = {"denoise": "S", "unity": "U", "echo": "E"}[mode]
    port.write(f"{command} {hops}\n".encode("ascii"))
    read_line_until(port, "START")

    output = bytearray()
    for hop in range(hops):
        start = hop * BYTES_PER_HOP
        block = audio[start : start + BYTES_PER_HOP]
        written = port.write(block)
        if written != BYTES_PER_HOP:
            raise IOError(f"Only sent {written} bytes for hop {hop}")
        output.extend(read_exact(port, BYTES_PER_HOP))

    done = read_line_until(port, "DONE", timeout_seconds=30)
    # The FFT/overlap-add modes emit one startup hop before the first input hop.
    # Remove that 16 ms delay and pad the end to retain the original WAV length.
    if mode in ("denoise", "unity"):
        output = output[BYTES_PER_HOP:] + (b"\x00" * BYTES_PER_HOP)

    # Keep only the samples that were present in the original file.
    output = output[: original_frames * 2]

    with wave.open(str(output_path), "wb") as wav_out:
        wav_out.setnchannels(1)
        wav_out.setsampwidth(2)
        wav_out.setframerate(SAMPLE_RATE)
        wav_out.writeframes(output)

    print(f"Saved: {output_path}")
    print(f"Mode: {mode}")
    if mode == "echo":
        matches = output == original_audio
        print(f"USB byte-for-byte check: {'PASS' if matches else 'FAIL'}")
    print(f"Processed {original_frames} samples ({original_frames / SAMPLE_RATE:.2f} s).")
    print(f"Pico timing: {done}")


def main():
    parser = argparse.ArgumentParser(
        description="USB audio test tool for the Raspberry Pi Pico denoiser"
    )
    parser.add_argument("--port", required=True, help="Serial port, e.g. COM5 or /dev/ttyACM0")
    parser.add_argument("--benchmark", action="store_true", help="Run the Pico's synthetic DSP benchmark")
    parser.add_argument("--input", type=Path, help="Input mono 16 kHz, 16-bit PCM WAV")
    parser.add_argument("--unity", action="store_true", help="Test FFT/IFFT reconstruction without noise reduction")
    parser.add_argument("--echo", action="store_true", help="Test USB transfer by echoing audio without DSP")
    parser.add_argument("--output", type=Path, default=Path("denoised.wav"), help="Output WAV path")
    args = parser.parse_args()

    if args.benchmark == (args.input is not None):
        parser.error("Choose exactly one mode: --benchmark OR --input input.wav")
    if args.unity and args.echo:
        parser.error("Choose at most one extra mode: --unity OR --echo")

    mode = "unity" if args.unity else "echo" if args.echo else "denoise"

    try:
        with open_port(args.port) as port:
            if args.benchmark:
                run_benchmark(port)
            else:
                process_wav(port, args.input, args.output, mode)
    except (OSError, ValueError, TimeoutError) as error:
        print(f"Error: {error}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
