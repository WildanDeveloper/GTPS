#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace WildanDev
{

// Variant value types of the Growtopia script call encoding.
enum class VariantType : uint8_t
{
    Unused = 0,
    Float = 1,
    String = 2,
    Vec2 = 3,
    Vec3 = 4,
    UInt32 = 5,
    Int32 = 9
};

struct VariantValue
{
    VariantType type{VariantType::Unused};
    std::string text; // String
    uint32_t number{0}; // UInt32
    int32_t integer{0}; // Int32
    float decimal{0.0f}; // Float
    float vec[3]{0.0f, 0.0f, 0.0f}; // Vec2 (xy) / Vec3 (xyz)
    uint8_t vecComponents{0};

    static VariantValue makeString(const std::string& value)
    {
        VariantValue variant;
        variant.type = VariantType::String;
        variant.text = value;
        return variant;
    }

    static VariantValue makeInt(int32_t value)
    {
        VariantValue variant;
        variant.type = VariantType::Int32;
        variant.integer = value;
        return variant;
    }

    static VariantValue makeUInt(uint32_t value)
    {
        VariantValue variant;
        variant.type = VariantType::UInt32;
        variant.number = value;
        return variant;
    }

    static VariantValue makeFloat(float value)
    {
        VariantValue variant;
        variant.type = VariantType::Float;
        variant.decimal = value;
        return variant;
    }

    static VariantValue makeVec2(float x, float y)
    {
        VariantValue variant;
        variant.type = VariantType::Vec2;
        variant.vec[0] = x;
        variant.vec[1] = y;
        variant.vecComponents = 2;
        return variant;
    }

    static VariantValue makeVec3(float x, float y, float z)
    {
        VariantValue variant;
        variant.type = VariantType::Vec3;
        variant.vec[0] = x;
        variant.vec[1] = y;
        variant.vec[2] = z;
        variant.vecComponents = 3;
        return variant;
    }
};

// Encodes a function-call argument list: u8 count, then per argument
// u8 index, u8 type, payload (string: u32 length + bytes, numeric: 4 bytes).
inline std::vector<uint8_t> encodeVariantList(const std::vector<VariantValue>& args)
{
    std::vector<uint8_t> body;
    uint8_t used = 0;
    for (const auto& arg : args)
    {
        if (arg.type != VariantType::Unused)
            ++used;
    }
    body.push_back(used);

    uint8_t index = 0;
    for (const auto& arg : args)
    {
        if (arg.type == VariantType::Unused)
        {
            ++index;
            continue;
        }
        body.push_back(index);
        body.push_back(static_cast<uint8_t>(arg.type));
        if (arg.type == VariantType::String)
        {
            uint32_t length = static_cast<uint32_t>(arg.text.size());
            body.push_back(static_cast<uint8_t>(length & 0xFF));
            body.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>((length >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((length >> 24) & 0xFF));
            body.insert(body.end(), arg.text.begin(), arg.text.end());
        }
        else if (arg.type == VariantType::Int32)
        {
            uint32_t bits = static_cast<uint32_t>(arg.integer);
            body.push_back(static_cast<uint8_t>(bits & 0xFF));
            body.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
        }
        else if (arg.type == VariantType::UInt32)
        {
            body.push_back(static_cast<uint8_t>(arg.number & 0xFF));
            body.push_back(static_cast<uint8_t>((arg.number >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>((arg.number >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((arg.number >> 24) & 0xFF));
        }
        else if (arg.type == VariantType::Vec2 || arg.type == VariantType::Vec3)
        {
            for (int i = 0; i < arg.vecComponents; ++i)
            {
                uint32_t bits = 0;
                __builtin_memcpy(&bits, &arg.vec[i], sizeof(bits));
                body.push_back(static_cast<uint8_t>(bits & 0xFF));
                body.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
                body.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
                body.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
            }
        }
        else // Float
        {
            uint32_t bits = 0;
            __builtin_memcpy(&bits, &arg.decimal, sizeof(bits));
            body.push_back(static_cast<uint8_t>(bits & 0xFF));
            body.push_back(static_cast<uint8_t>((bits >> 8) & 0xFF));
            body.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
            body.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
        }
        ++index;
    }
    return body;
}

} // namespace WildanDev
