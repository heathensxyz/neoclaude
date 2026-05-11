#!/usr/bin/env python3
"""Convert sprite sheets to RGB565 binary files for ESP32 LittleFS.

Works with any sprite sheet that uses a standard 8-column x 9-row grid
with 192x208 pixel frames. Any sprite sheet matching this layout will work.

Usage:
    python3 convert_sprites.py <spritesheet.png> <output_dir> [--name label]

Example:
    python3 convert_sprites.py ~/Downloads/pumpy/spritesheet.webp data/p --name Pumpy
"""

import struct
import os
import sys
import argparse
from PIL import Image
import numpy as np

COLS = 8
ROWS = 9
FRAME_W = 192
FRAME_H = 208

ANIM_NAMES = ['idle', 'run_r', 'run_l', 'wave', 'jump', 'sad', 'wait', 'work', 'think']
ANIM_PREFIXES = ['i', 'rr', 'rl', 'w', 'j', 's', 'wt', 'wk', 'th']


def convert_sheet(input_path, output_dir):
    img = Image.open(input_path).convert('RGBA')
    w, h = img.size

    expected_w = COLS * FRAME_W
    expected_h = ROWS * FRAME_H
    if w != expected_w or h != expected_h:
        print(f"Warning: expected {expected_w}x{expected_h}, got {w}x{h}")
        print(f"Sprite sheet should be an {COLS}x{ROWS} grid of {FRAME_W}x{FRAME_H} frames.")
        sys.exit(1)

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
    parser = argparse.ArgumentParser(
        description='Convert sprite sheets to RGB565 binary files for ESP32')
    parser.add_argument('spritesheet', help='Path to sprite sheet image (PNG or WebP)')
    parser.add_argument('output_dir', help='Output directory for binary frames (e.g., data/p)')
    parser.add_argument('--name', default=None, help='Display name for the pet (for logging)')
    args = parser.parse_args()

    if not os.path.exists(args.spritesheet):
        print(f"File not found: {args.spritesheet}")
        sys.exit(1)

    label = args.name or os.path.basename(os.path.dirname(args.spritesheet)) or "pet"
    print(f"Converting {label} sprites from {args.spritesheet}...")
    convert_sheet(args.spritesheet, args.output_dir)
    print(f"\nBinary frames written to {args.output_dir}")
    print("Upload to ESP32 with: pio run -t uploadfs")


if __name__ == '__main__':
    main()
