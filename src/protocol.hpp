#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace WildanDev
{

// Growtopia text packet: u8 kind (2 or 3) + 3 zero bytes, then a payload of
// newline-separated "key|value" lines, optionally null terminated.
struct TextPacket
{
    uint8_t kind{0};
    std::vector<std::string> lines;
};

inline std::optional<TextPacket> parseTextPacket(const uint8_t* data, std::size_t length)
{
    if (data == nullptr || length < 5)
        return std::nullopt;
    if (data[0] != 2 && data[0] != 3)
        return std::nullopt;

    std::size_t end = length;
    while (end > 4 && data[end - 1] == 0)
        --end;

    TextPacket packet;
    packet.kind = data[0];
    std::string current;
    for (std::size_t i = 4; i < end; ++i)
    {
        if (data[i] == '\n')
        {
            packet.lines.push_back(current);
            current.clear();
        }
        else
        {
            current.push_back(static_cast<char>(data[i]));
        }
    }
    packet.lines.push_back(current);
    return packet;
}

inline std::string lineValue(const std::vector<std::string>& lines, const std::string& key)
{
    std::string prefix = key + "|";
    for (const auto& line : lines)
    {
        if (line.rfind(prefix, 0) == 0)
            return line.substr(prefix.size());
    }
    return "";
}

struct LoginToken
{
    std::string growId;
    std::string password;
};

// Decodes the base64 ltoken sent with the "protocol" action and extracts the
// embedded "growId=" / "password=" fields. Both fields are validated: the
// growId must be [A-Za-z0-9_]{1,32} and the password printable ASCII without
// '&' (which would be ambiguous in the token format). This keeps client
// strings from leaking into spawn text, chat or log lines as protocol data.
template <typename Base64>
std::optional<LoginToken> parseLoginToken(const std::string& encoded)
{
    auto decoded = Base64::decode(encoded);
    if (!decoded.has_value())
        return std::nullopt;

    LoginToken token;
    const std::string& text = decoded.value();
    auto growPos = text.find("growId=");
    auto passPos = text.find("password=");
    if (growPos == std::string::npos || passPos == std::string::npos)
        return std::nullopt;

    growPos += 7;
    passPos += 9;
    auto growEnd = text.find('&', growPos);
    auto passEnd = text.find('&', passPos);
    token.growId = text.substr(growPos, growEnd == std::string::npos ? growEnd : growEnd - growPos);
    token.password = text.substr(passPos, passEnd == std::string::npos ? passEnd : passEnd - passPos);

    if (token.growId.empty() || token.growId.size() > 32)
        return std::nullopt;
    for (char c : token.growId)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return std::nullopt;
    }
    if (token.password.empty() || token.password.size() > 64)
        return std::nullopt;
    for (char c : token.password)
    {
        if (c == '&' || static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
            return std::nullopt;
    }
    return token;
}

// Fixed 60-byte tank packet header (15 x 32-bit slots, little endian).
struct TankHeader
{
    int32_t tag{4};
    int32_t type{0};
    int32_t netId{0};
    int32_t uid{0};
    int32_t state{0};
    float count{0.0f};
    int32_t id{0};
    float posX{0.0f};
    float posY{0.0f};
    float velX{0.0f};
    float velY{0.0f};
    float extra{0.0f};
    int32_t punchX{0};
    int32_t punchY{0};
    uint32_t dataSize{0};
};

inline void appendInt32(std::vector<uint8_t>& out, int32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

inline void appendUInt32(std::vector<uint8_t>& out, uint32_t value)
{
    appendInt32(out, static_cast<int32_t>(value));
}

inline void appendFloat(std::vector<uint8_t>& out, float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    __builtin_memcpy(&bits, &value, sizeof(bits));
    appendUInt32(out, bits);
}

inline std::vector<uint8_t> encodeTankHeader(const TankHeader& header)
{
    std::vector<uint8_t> out;
    out.reserve(60);
    appendInt32(out, header.tag);
    appendInt32(out, header.type);
    appendInt32(out, header.netId);
    appendInt32(out, header.uid);
    appendInt32(out, header.state);
    appendFloat(out, header.count);
    appendInt32(out, header.id);
    appendFloat(out, header.posX);
    appendFloat(out, header.posY);
    appendFloat(out, header.velX);
    appendFloat(out, header.velY);
    appendFloat(out, header.extra);
    appendInt32(out, header.punchX);
    appendInt32(out, header.punchY);
    appendUInt32(out, header.dataSize);
    return out;
}

} // namespace WildanDev
