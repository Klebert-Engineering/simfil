// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.
#pragma once

#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

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

    [[nodiscard]] static std::optional<ByteArray> fromHex(std::string_view hex)
    {
        if (hex.size() % 2 != 0)
            return std::nullopt;

        std::string decoded;
        decoded.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            const auto upper = decodeHexNibble(hex[i]);
            const auto lower = decodeHexNibble(hex[i + 1]);
            if (upper < 0 || lower < 0)
                return std::nullopt;
            decoded.push_back(static_cast<char>((upper << 4) | lower));
        }

        return ByteArray{std::move(decoded)};
    }

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
        std::string out;
        out.reserve(bytes.size() * 2);

        if (uppercase) {
            for (unsigned char byte : bytes)
                fmt::format_to(std::back_inserter(out), FMT_STRING("{:02X}"), byte);
        } else {
            for (unsigned char byte : bytes)
                fmt::format_to(std::back_inserter(out), FMT_STRING("{:02x}"), byte);
        }

        return out;
    }

    [[nodiscard]] std::string toLiteral() const
    {
        std::string out;
        out.reserve(bytes.size() + 3);
        out += "b\"";

        for (unsigned char byte : bytes) {
            switch (byte) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (byte < 0x20 || byte >= 0x7f)
                    fmt::format_to(std::back_inserter(out), FMT_STRING("\\x{:02X}"), byte);
                else
                    out.push_back(static_cast<char>(byte));
                break;
            }
        }

        out.push_back('"');
        return out;
    }

    [[nodiscard]] static auto decodeHexNibble(char c) -> int
    {
        if ('0' <= c && c <= '9')
            return c - '0';
        if ('a' <= c && c <= 'f')
            return c - 'a' + 10;
        if ('A' <= c && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }
};

}  // namespace simfil
