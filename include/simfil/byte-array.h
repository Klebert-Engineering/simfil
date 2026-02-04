// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace simfil
{

struct ByteArray
{
    std::string_view bytes;

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

    [[nodiscard]] std::string toHex() const
    {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(bytes.size() * 2 + 2);
        out.append("0x");
        for (unsigned char byte : bytes) {
            out.push_back(kHex[(byte >> 4) & 0x0f]);
            out.push_back(kHex[byte & 0x0f]);
        }
        return out;
    }

    [[nodiscard]] std::string toDisplayString() const
    {
        if (auto decoded = decodeBigEndianI64())
            return std::to_string(*decoded);
        return toHex();
    }
};

}  // namespace simfil
