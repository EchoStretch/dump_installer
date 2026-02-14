# Created by Gezine

import struct
import os
import sys

VERSION = 1

if len(sys.argv) == 5:
    TITLE_ID = sys.argv[1]
    SCE_SYS_PATH = sys.argv[2]
    INPUT_FILE = sys.argv[3]
    OUTPUT_FILE = sys.argv[4]
else:
    print("Usage: python ffpkg_create.py <TITLE_ID> <SCE_SYS_PATH> <INPUT_FILE> <OUTPUT_FILE>")
    print()
    print("Examples:")
    print("  python ffpkg_create.py PPSA10240 sce_sys snake.img snake.ffpkg")
    print("  python ffpkg_create.py PPSA12345 custom_sce game.img game.ffpkg")
    sys.exit(1)

if len(TITLE_ID) != 9:
    raise ValueError(f"TITLE_ID must be exactly 9 characters, got {len(TITLE_ID)}")

sce_sys_files = []
for root, dirs, files in os.walk(SCE_SYS_PATH):
    for name in files:
        filepath = os.path.join(root, name)
        full_path = filepath.replace("\\", "/")
        sce_sys_files.append(full_path)

sce_sys_files.sort()  # deterministic order

file_count = len(sce_sys_files)

sce_entries = b""
for full_path in sce_sys_files:
    path_bytes = full_path.encode("ascii")
    path_len   = len(path_bytes) + 1        # +1 extra

    with open(full_path, "rb") as fh:
        file_data = fh.read()

    file_size = len(file_data)

    # Written in reverse so reading from EOF gives:
    # path_len | path+null | file_size | file_data
    sce_entries += file_data                    # raw file data
    sce_entries += struct.pack("<Q", file_size) # uint64 LE — file size in bytes
    sce_entries += path_bytes + b"\x00"         # path string + null byte
    sce_entries += struct.pack("<H", path_len)  # uint16 LE — length of path + null

    print(f"  + {full_path} ({file_size} bytes)")

magic      = b"ffpkg"
version    = struct.pack("<H", VERSION)         # uint16 little-endian
title      = TITLE_ID.encode("ascii")           # 9 bytes, normal order
file_count_bytes = struct.pack("<I", file_count) # uint32 LE — placed before TITLE_ID
                                                 # so it's at a fixed offset from EOF

# Trailer layout (written forward, read from EOF):
#   ffpkg | version(2) | TITLE_ID(9) | file_count(4) | [entries...] | img_data
trailer = sce_entries + file_count_bytes + title + version + magic

with open(INPUT_FILE, "ab") as f:
    f.write(trailer)

os.rename(INPUT_FILE, OUTPUT_FILE)

print(f"\nDone — {OUTPUT_FILE} written")

