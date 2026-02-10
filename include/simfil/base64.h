// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace simfil
{

inline auto base64Encode(std::string_view input) -> std::string
{
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
        const auto remaining = input.size() - i;
        const auto b0 = static_cast<uint8_t>(input[i]);
        const auto b1 = remaining > 1 ? static_cast<uint8_t>(input[i + 1]) : 0U;
        const auto b2 = remaining > 2 ? static_cast<uint8_t>(input[i + 2]) : 0U;

        out.push_back(kTable[(b0 >> 2) & 0x3F]);
        out.push_back(kTable[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)]);
        out.push_back(remaining > 1 ? kTable[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=');
        out.push_back(remaining > 2 ? kTable[b2 & 0x3F] : '=');
    }

    return out;
}

inline auto base64Decode(std::string_view input) -> std::optional<std::string>
{
    if (input.size() % 4 != 0)
        return std::nullopt;

    auto decodeChar = [](char c) -> int {
        if ('A' <= c && c <= 'Z')
            return c - 'A';
        if ('a' <= c && c <= 'z')
            return c - 'a' + 26;
        if ('0' <= c && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };

    std::string out;
    out.reserve((input.size() / 4) * 3);

    for (size_t i = 0; i < input.size(); i += 4) {
        const auto c0 = input[i];
        const auto c1 = input[i + 1];
        const auto c2 = input[i + 2];
        const auto c3 = input[i + 3];

        const auto v0 = decodeChar(c0);
        const auto v1 = decodeChar(c1);
        if (v0 < 0 || v1 < 0)
            return std::nullopt;

        const bool p2 = c2 == '=';
        const bool p3 = c3 == '=';

        if (p2 && !p3)
            return std::nullopt;

        const auto v2 = p2 ? 0 : decodeChar(c2);
        const auto v3 = p3 ? 0 : decodeChar(c3);
        if ((!p2 && v2 < 0) || (!p3 && v3 < 0))
            return std::nullopt;

        const auto b0 = static_cast<char>((v0 << 2) | (v1 >> 4));
        out.push_back(b0);

        if (!p2) {
            const auto b1 = static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2));
            out.push_back(b1);
        }

        if (!p3) {
            const auto b2 = static_cast<char>(((v2 & 0x03) << 6) | v3);
            out.push_back(b2);
        }
    }

    return out;
}

}  // namespace simfil
