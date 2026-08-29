#!/usr/bin/env python3
"""
make_shim_pck.py — Generate a minimal valid Godot 4 .pck (V2 format)
containing res://project.godot and res://main.tscn so that ruby_gg
boots cleanly out of the box without requiring CLI path overrides.
"""

import struct
import hashlib
import os

def create_pck(output_path):
    files = [
        (
            "res://project.godot",
            b'config_version=5\n\n[application]\nconfig/name="RubyGG"\nrun/main_scene="res://main.tscn"\nconfig/features=PackedStringArray("4.7")\n'
        ),
        (
            "res://main.tscn",
            b'[gd_scene format=3]\n\n[node name="Main" type="Node"]\n'
        )
    ]

    header_magic = 0x43504447 # "GDPC"
    format_version = 2
    ver_major = 4
    ver_minor = 7
    ver_patch = 2
    pack_flags = 0
    file_base = 0

    # Build directory entries and payloads
    dir_entries = []
    payloads = []
    current_ofs = 0

    for path, data in files:
        md5 = hashlib.md5(data).digest()
        path_bytes = path.encode('utf-8')
        sl = len(path_bytes)
        size = len(data)
        ofs = current_ofs
        current_ofs += size
        payloads.append(data)
        dir_entries.append((sl, path_bytes, ofs, size, md5, 0))

    # Calculate where file_base starts (after header and directory)
    # Header: magic(4) + ver(4) + maj(4) + min(4) + pat(4) + flags(4) + file_base(8) + 16*4 reserved(64) = 96 bytes
    # Directory: count(4) + for each: sl(4) + len(path) + ofs(8) + size(8) + md5(16) + flags(4) = 44 + len(path)
    header_size = 4 + 4 + 4 + 4 + 4 + 4 + 8 + 64
    dir_size = 4 + sum(4 + e[0] + 8 + 8 + 16 + 4 for e in dir_entries)
    actual_file_base = header_size + dir_size

    # Align file_base to 8 bytes if needed
    padding = (8 - (actual_file_base % 8)) % 8
    actual_file_base += padding

    with open(output_path, 'wb') as f:
        # Header
        f.write(struct.pack('<IIIIIIQ', header_magic, format_version, ver_major, ver_minor, ver_patch, pack_flags, actual_file_base))
        f.write(b'\x00' * 64) # 16 reserved uint32s

        # Directory
        f.write(struct.pack('<I', len(files)))
        for sl, path_bytes, ofs, size, md5, flags in dir_entries:
            f.write(struct.pack('<I', sl))
            f.write(path_bytes)
            f.write(struct.pack('<QQ', ofs, size))
            f.write(md5)
            f.write(struct.pack('<I', flags))

        if padding > 0:
            f.write(b'\x00' * padding)

        # Payloads
        for data in payloads:
            f.write(data)

    print(f"[make_shim_pck] Generated {output_path} ({os.path.getsize(output_path)} bytes)")

if __name__ == '__main__':
    out = os.path.expanduser('/home/quantumcreeper/SwordigoDesktop/bin/ruby_gg.pck')
    create_pck(out)
