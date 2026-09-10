#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace WildanDev
{

// WildanDev item database version. Must stay in sync with encodeItemsDat().
constexpr uint16_t kItemsDatVersion = 0x1A; // 26

enum class ItemType : uint8_t
{
    Fist = 0,
    Wrench = 1,
    Door = 2,
    Lock = 3,
    Foreground = 0x11,
    Background = 0x12,
    Seed = 0x13,
    Clothing = 0x14,
    MainDoor = 0x0D
};

enum class ItemCollision : uint8_t
{
    None = 0,
    Full = 1,
    Platform = 2
};

struct ItemDef
{
    uint16_t id{0};
    uint8_t property{0};
    uint8_t category{0};
    ItemType type{ItemType::Foreground};
    std::string name;
    std::string texture;
    int32_t ingredient{0};
    ItemCollision collision{ItemCollision::Full};
    uint8_t hits{4};
    int32_t hitResetSeconds{0};
    uint8_t clothType{0};
    int16_t rarity{1};
    std::string info;
    uint16_t spliceA{0};
    uint16_t spliceB{0};
};

// The built-in WildanDev catalog (our own game data).
const std::vector<ItemDef>& builtinCatalog();

// Finds a catalog entry by id (nullptr when unknown).
const ItemDef* findItemById(int id);

// Encodes the full items.dat file bytes (header + every item).
std::vector<uint8_t> encodeItemsDat(const std::vector<ItemDef>& catalog);

// Decodes one obfuscated item name (used by tests and tools).
std::string decodeItemName(const std::vector<uint8_t>& blob, std::size_t& pos, uint16_t itemId);

// Lists every item name in an encoded file (empty on corrupt input).
std::vector<std::string> decodeAllNames(const std::vector<uint8_t>& blob);

// FNV-1a 32-bit digest (generic helper).
uint32_t fnv1a32(const std::vector<uint8_t>& data);

// CRC-32 IEEE of a binary payload — what the client expects as the
// items.dat hash identity.
uint32_t crc32IEEE(const std::vector<uint8_t>& data);

} // namespace WildanDev
