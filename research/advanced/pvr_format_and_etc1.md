# PVR Format and ETC1 Texture Decoding

This document covers the PVR v2 texture container specification, ETC1 texture decompression, and a comparative analysis of the JavaScript and C++ parsing systems.

---

## 1. PVR v2 Container Header Layout

The PVR v2 header has a total footprint of **52 bytes** structured as follows:

| Field Name | Offset (Bytes) | Data Type | Value / Description |
| :--- | :--- | :--- | :--- |
| `header_size` | 0 | `uint32` | Typically `44` (header payload size excluding trail metadata) |
| `height` | 4 | `uint32` | Texture height in pixels |
| `width` | 8 | `uint32` | Texture width in pixels |
| `mip_count` | 12 | `uint32` | Number of mipmap levels |
| `flags` | 16 | `uint32` | Flags representing pixel format and features |
| `data_size` | 20 | `uint32` | Total byte size of compressed pixel data |
| `bpp` | 24 | `uint32` | Bits per pixel |
| `masks` | 28 | `uint32[4]` | Red, Green, Blue, Alpha masks (16 bytes) |
| `magic` | 44 | `uint32` | Magic number `0x21525650` (ASCII `"PVR!"` in little-endian) |
| `num_surfaces` | 48 | `uint32` | Number of surfaces (layers) |

### Format Enumeration (Lower 8 Bits of `flags` / `pixel_format`)
- **`18`** (`0x12`): RGBA8888 (Uncompressed 32-bit color).
- **`54`** (`0x36`): ETC1 (Ericsson Texture Compression 1).

---

## 2. ETC1 Decompression Algorithm

ETC1 compresses a 4x4 block of pixels into an 8-byte payload. The decoder reconstructs the block by computing color offsets from one of two base colors.

### Payload Layout
The 8-byte block is read as two 32-bit big-endian words `l` (bits 63–32) and `h` (bits 31–0):
- **`diff`** (`l >>> 1 & 1`): If 1, differential mode (Base 2 is Base 1 + delta). If 0, individual mode (independent base colors).
- **`flip`** (`l & 1`): If 0, block is split vertically (2x4 blocks). If 1, block is split horizontally (4x2 blocks).
- **`table1`** (`l >>> 5 & 7`), **`table2`** (`l >>> 2 & 7`): Intensity table indices.
- **Base colors**:
  - Individual Mode: Two RGB444 color structures decoded from `l` bits.
  - Differential Mode: One RGB555 base color and one RGB333 signed offset vector.

### Pixel Intensity Modulation
For each pixel $w \in [0, 15]$:
1. Identify the sub-block (0 or 1) based on the coordinate and the `flip` bit.
2. Read the control bits from `h`:
   - `msb` = bit $16 + w$ (sign of modulation).
   - `lsb` = bit $w$ (table index modifier).
3. The intensity modifier is fetched from the selected table at index `lsb`:
   $$\text{modifier} = \text{table}[\text{sub\_block}][\text{lsb}]$$
   If `msb` is 1, the modifier is negated.
4. Add the modifier to the sub-block base color, clamp the result to $[0, 255]$, and append an alpha value of 255 (ETC1 lacks alpha channel support).

---

## 3. Comparison of Parser Architectures

### JavaScript Implementation
- **Capability**: Supports PVR v2 headers only. Fails with a "not a PVR v2 file" exception if fed PVR v3.
- **Dependencies**: Uses web APIs (`Blob` / `FileReader` / `DataView`) to process memory buffers.
- **Decompressor**: Decodes ETC1 procedurally to `RGBA` arrays via JS math functions.

### C++ Engine Implementation (`exptsrc/platform/pvr_loader.cpp`)
- **Capability**: Dual support for PVR v3 (`0x03525650` signature) and legacy PVR v2 (`0x21525650` signature).
- **Performance**: Written in highly optimized C++ utilizing direct memory copies and pointer casting.
- **Texture Upload**: Directly interacts with OpenGL context, parsing and uploading the decompressed texture to the GPU (`glTexImage2D`).

### Verdict
The current C++ PVR texture loader in the engine is more robust than the decompiled JS equivalent, as it gracefully handles both legacy PVR v2 and newer PVR v3 assets. No changes are required for the C++ loader.
