#!/usr/bin/env python3
"""Convert Hendo sprite sheets to RGB565 binary files for ESP32 LittleFS."""

import struct
import os
import sys
from PIL import Image
import numpy as np

COLS = 8
ROWS = 9
FRAME_W = 192
FRAME_H = 208

ANIM_NAMES = ['idle', 'run_r', 'run_l', 'wave', 'jump', 'sad', 'wait', 'work', 'think']
ANIM_PREFIXES = ['i', 'rr', 'rl', 'w', 'j', 's', 'wt', 'wk', 'th']


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def convert_sheet(input_path, output_dir, prefix):
    img = Image.open(input_path).convert('RGBA')
    arr = np.array(img)

    os.makedirs(output_dir, exist_ok=True)
    total_frames = 0
    manifest = []

    for row in range(ROWS):
        frame_count = 0
        for col in range(COLS):
            x, y = col * FRAME_W, row * FRAME_H
            frame = arr[y:y + FRAME_H, x:x + FRAME_W]

            opaque_count = np.sum(frame[:, :, 3] > 128)
            if opaque_count < 50:
                break

            r = frame[:, :, 0].astype(np.uint16)
            g = frame[:, :, 1].astype(np.uint16)
            b = frame[:, :, 2].astype(np.uint16)
            a = frame[:, :, 3]

            is_magenta = (r > 200) & (g < 80) & (b > 200)
            transparent = (a < 128) | is_magenta

            rgb = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            rgb[transparent] = 0x0000

            data = rgb.astype(np.uint16).tobytes()

            filename = f"{ANIM_PREFIXES[row]}{frame_count}.bin"
            filepath = os.path.join(output_dir, filename)
            with open(filepath, 'wb') as f:
                f.write(data)

            frame_count += 1

        manifest.append(frame_count)
        if frame_count > 0:
            print(f"  {ANIM_NAMES[row]}: {frame_count} frames")
        total_frames += frame_count

    manifest_path = os.path.join(output_dir, "m.txt")
    with open(manifest_path, 'w') as f:
        for count in manifest:
            f.write(f"{count}\n")

    total_kb = total_frames * FRAME_W * FRAME_H * 2 / 1024
    print(f"  Total: {total_frames} frames ({total_kb:.0f} KB)")
    return total_frames


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_dir = os.path.dirname(script_dir)
    data_dir = os.path.join(project_dir, "data")

    gemini_path = os.path.expanduser("~/Desktop/hendo_gemini_v3.png")
    duo_path = os.path.expanduser("~/Desktop/hendo_duo_v1.png")

    if not os.path.exists(gemini_path) or not os.path.exists(duo_path):
        print("Sprite sheets not found on Desktop.")
        print(f"  Expected: {gemini_path}")
        print(f"  Expected: {duo_path}")
        sys.exit(1)

    print(f"Converting gemini sprites...")
    convert_sheet(gemini_path, os.path.join(data_dir, "g"), "gemini")

    print(f"\nConverting duo sprites...")
    convert_sheet(duo_path, os.path.join(data_dir, "d"), "duo")

    print(f"\nSprite data written to {data_dir}")
    print("Upload to ESP32 with: pio run -t uploadfs")


if __name__ == '__main__':
    main()
