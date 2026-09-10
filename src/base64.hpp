#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace WildanDev
{

// Self-contained Base64 codec (RFC 4648, no external dependency).
class Base64
{
public:
    static std::string encode(const std::string& input)
    {
        static constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((input.size() + 2) / 3) * 4);

        for (std::size_t i = 0; i < input.size(); i += 3)
        {
            uint32_t group = static_cast<uint8_t>(input[i]) << 16;
            int padding = 0;
            if (i + 1 < input.size())
                group |= static_cast<uint8_t>(input[i + 1]) << 8;
            else
                ++padding;
            if (i + 2 < input.size())
                group |= static_cast<uint8_t>(input[i + 2]);
            else
                ++padding;

            out.push_back(alphabet[(group >> 18) & 0x3F]);
            out.push_back(alphabet[(group >> 12) & 0x3F]);
            out.push_back(padding >= 2 ? '=' : alphabet[(group >> 6) & 0x3F]);
            out.push_back(padding >= 1 ? '=' : alphabet[group & 0x3F]);
        }
        return out;
    }

    static std::optional<std::string> decode(const std::string& input)
    {
        if (input.size() % 4 != 0)
            return std::nullopt;

        std::string out;
        out.reserve((input.size() / 4) * 3);

        for (std::size_t i = 0; i < input.size(); i += 4)
        {
            int values[4];
            int padding = 0;
            for (int k = 0; k < 4; ++k)
            {
                char c = input[i + k];
                if (c == '=')
                {
                    // Padding is only valid at the very end, and only in the
                    // last one or two positions of the final quad.
                    if (i + 4 != input.size() || k < 2 || (k == 2 && input[i + 3] != '='))
                        return std::nullopt;
                    values[k] = 0;
                    ++padding;
                    continue;
                }
                int v = decodeChar(c);
                if (v < 0)
                    return std::nullopt;
                values[k] = v;
            }
            uint32_t group = (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
            out.push_back(static_cast<char>((group >> 16) & 0xFF));
            if (padding < 2)
                out.push_back(static_cast<char>((group >> 8) & 0xFF));
            if (padding < 1)
                out.push_back(static_cast<char>(group & 0xFF));
        }
        return out;
    }

private:
    static int decodeChar(char c)
    {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    }
};

} // namespace WildanDev
