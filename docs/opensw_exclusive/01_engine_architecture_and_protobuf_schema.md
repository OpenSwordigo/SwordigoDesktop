# Swordigo OpenSwordigo Research: Engine Core Architecture & Binary Protobuf Schemas

## 1. System Overview

The **Caver Engine** (reconstructed in OpenSwordigo) utilizes a custom binary asset serialization system built on Google Protocol Buffer semantics. Rather than relying on heavy third-party runtime libraries, the engine implements a zero-dependency C++ binary decoder (`Caver::ProtobufDecoder`) that processes low-level wire formats for `.scene`, `.template`, and configuration files.

---

## 2. Low-Level Wire Format & Tag Encoding

Binary asset streams encode fields as key-value pairs where the field key packs both the Field Number (tag identifier) and the Wire Type into a variable-length integer (Varint).

```cpp
namespace Caver {

enum class WireType : uint32_t {
    Varint          = 0, // int32, int64, uint32, uint64, bool, enum
    Bit64           = 1, // fixed64, sfixed64, double
    LengthDelimited = 2, // string, bytes, embedded messages, packed repeated fields
    StartGroup      = 3, // Deprecated group start tag
    EndGroup        = 4, // Deprecated group end tag
    Bit32           = 5  // fixed32, sfixed32, float
};

struct ProtobufTag {
    uint32_t field_number;
    WireType wire_type;
};

inline ProtobufTag DecodeTag(uint32_t key) {
    return { key >> 3, static_cast<WireType>(key & 0x07) };
}

} // namespace Caver
```

---

## 3. C++ Binary Protobuf Stream Parser Implementation

The following C++ engine component implements deterministic reading of unsigned varints, IEEE 754 floating point numbers, and length-delimited byte arrays from raw memory buffers.

```cpp
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

namespace Caver {

class BinaryStreamReader {
public:
    BinaryStreamReader(const uint8_t* data, size_t size)
        : m_buffer(data), m_size(size), m_offset(0) {}

    bool HasMore() const { return m_offset < m_size; }
    size_t GetOffset() const { return m_offset; }

    uint64_t ReadVarint() {
        uint64_t result = 0;
        int shift = 0;
        while (m_offset < m_size) {
            uint8_t byte = m_buffer[m_offset++];
            result |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return result;
            shift += 7;
            if (shift >= 64) throw std::runtime_error("Varint overflow in binary asset reader");
        }
        throw std::runtime_error("Unexpected EOF while reading varint");
    }

    float ReadFloat32() {
        if (m_offset + 4 > m_size) throw std::runtime_error("Unexpected EOF while reading float32");
        float val;
        std::memcpy(&val, m_buffer + m_offset, 4);
        m_offset += 4;
        return val;
    }

    double ReadFloat64() {
        if (m_offset + 8 > m_size) throw std::runtime_error("Unexpected EOF while reading float64");
        double val;
        std::memcpy(&val, m_buffer + m_offset, 8);
        m_offset += 8;
        return val;
    }

    std::string ReadString() {
        uint64_t len = ReadVarint();
        if (m_offset + len > m_size) throw std::runtime_error("String length exceeds buffer boundaries");
        std::string str(reinterpret_cast<const char*>(m_buffer + m_offset), len);
        m_offset += len;
        return str;
    }

    std::vector<uint8_t> ReadBytes() {
        uint64_t len = ReadVarint();
        if (m_offset + len > m_size) throw std::runtime_error("Byte array length exceeds buffer boundaries");
        std::vector<uint8_t> bytes(m_buffer + m_offset, m_buffer + m_offset + len);
        m_offset += len;
        return bytes;
    }

private:
    const uint8_t* m_buffer;
    size_t m_size;
    size_t m_offset;
};

} // namespace Caver
```

---

## 4. Protobuf Schema Mappings for Engine Assets

### 4.1 Asset Header & Texture Conversion Schema

```cpp
namespace Caver {

enum class TexturePixelFormat : uint32_t {
    FormatNone            = 0,
    FormatRGBA8888        = 1,
    FormatRGBA4444        = 2,
    FormatRGBA5551        = 3,
    FormatRGB888          = 4,
    FormatRGB565          = 5,
    FormatLuminance8      = 6,
    FormatAlpha8          = 7,
    FormatLuminanceAlpha88 = 8
};

enum class TextureImageType : uint32_t {
    TypeUnknown = 0,
    TypePNG     = 1,
    TypePVR     = 2,
    TypeTEX     = 3
};

struct TextureConversionInfo {
    uint32_t width        = 0;  // Tag 0x08
    uint32_t height       = 0;  // Tag 0x10
    TexturePixelFormat format = TexturePixelFormat::FormatNone; // Tag 0x28
    TextureImageType image_type = TextureImageType::TypeUnknown; // Tag 0x30
};

} // namespace Caver
```

---

## 5. Virtual File System (`Caver::VFS`) Data Streaming

```cpp
#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace Caver {

class VFS {
public:
    static VFS& Instance() {
        static VFS instance;
        return instance;
    }

    void MountDirectory(const std::string& mount_point, const std::string& physical_path) {
        m_mounts[mount_point] = physical_path;
    }

    std::vector<uint8_t> ReadAsset(const std::string& asset_path) {
        // Read file contents into native memory stream for Protobuf parsing
        return {};
    }

private:
    VFS() = default;
    std::unordered_map<std::string, std::string> m_mounts;
};

} // namespace Caver
```
