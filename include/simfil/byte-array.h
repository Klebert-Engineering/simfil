// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace simfil
{

struct ByteArray
{
    std::string bytes;

    ByteArray() = default;

    explicit ByteArray(const char* data)
        : bytes(data)
    {}

    explicit ByteArray(std::string_view data)
        : bytes(data)
    {}

    explicit ByteArray(std::string data)
        : bytes(std::move(data))
    {}

    auto operator==(const ByteArray&) const -> bool = default;

    [[nodiscard]] std::optional<int64_t> decodeBigEndianI64() const
    {
        if (bytes.size() > 8) {
            for (size_t i = 8; i < bytes.size(); ++i) {
                if (static_cast<unsigned char>(bytes[i]) != 0)
                    return std::nullopt;
            }
        }

        const size_t count = bytes.size() <= 8 ? bytes.size() : 8;
        uint64_t value = 0;
        for (size_t i = 0; i < count; ++i) {
            value = (value << 8) | static_cast<unsigned char>(bytes[i]);
        }

        int64_t signedValue = 0;
        std::memcpy(&signedValue, &value, sizeof(signedValue));
        return signedValue;
    }

    [[nodiscard]] std::string toHex(bool uppercase = true) const
    {
        static constexpr char kHexLower[] = "0123456789abcdef";
        static constexpr char kHexUpper[] = "0123456789ABCDEF";
        const char* kHex = uppercase ? kHexUpper : kHexLower;

        std::string out;
        out.reserve(bytes.size() * 2);
        for (unsigned char byte : bytes) {
            out.push_back(kHex[(byte >> 4) & 0x0f]);
            out.push_back(kHex[byte & 0x0f]);
        }
        return out;
    }
};

}  // namespace simfil
