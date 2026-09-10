#include "items.hpp"

#include <cstring>
#include <unordered_map>

namespace WildanDev
{

namespace
{

constexpr char kNameToken[] = "PBG892FXX982ABC*";
constexpr std::size_t kNameTokenLength = 16;

void appendU8(std::vector<uint8_t>& out, uint8_t value)
{
    out.push_back(value);
}

void appendU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void appendI16(std::vector<uint8_t>& out, int16_t value)
{
    appendU16(out, static_cast<uint16_t>(value));
}

void appendI32(std::vector<uint8_t>& out, int32_t value)
{
    appendU32(out, static_cast<uint32_t>(value));
}

void appendCountedString(std::vector<uint8_t>& out, const std::string& text)
{
    appendI16(out, static_cast<int16_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

void appendZeroes(std::vector<uint8_t>& out, std::size_t count)
{
    out.insert(out.end(), count, 0);
}

} // namespace

const std::vector<ItemDef>& builtinCatalog()
{
    // Server-side metadata only (the client uses resources/items.dat). IDs
    // and behaviour mirror the official database: seeds are item+1, Gems are
    // 112, World Lock is 242. hits = punches needed to break (0 = never).
    static const std::vector<ItemDef> catalog = {
        {0, 0, 0, ItemType::Foreground, "Blank", "tiles/blank.png", 0, ItemCollision::None, 0, 0, 0, 0,
         "Empty space.", 0, 0},
        {2, 0, 0, ItemType::Foreground, "Dirt", "tiles/dirt.png", 0, ItemCollision::Full, 4, 0, 0, 1,
         "Plain dirt.", 0, 0},
        {4, 0, 0, ItemType::Foreground, "Lava", "tiles/lava.png", 0, ItemCollision::Full, 3, 0, 0, 77,
         "Hot! Don't touch.", 0, 0},
        {6, 0, 0, ItemType::MainDoor, "Main Door", "tiles/main_door.png", 0, ItemCollision::Full, 0, 0,
         0, 1, "World entrance.", 0, 0},
        {8, 0, 0, ItemType::Foreground, "Bedrock", "tiles/bedrock.png", 0, ItemCollision::Full, 0, 0,
         0, 1, "Unbreakable.", 0, 0},
        {10, 0, 0, ItemType::Foreground, "Rock", "tiles/rock.png", 0, ItemCollision::Full, 4, 0, 0, 2,
         "A hard rock.", 0, 0},
        {14, 0, 0, ItemType::Background, "Cave Background", "tiles/cave_background.png", 0,
         ItemCollision::None, 4, 0, 0, 1, "Cavey.", 0, 0},
        {16, 0, 0, ItemType::Foreground, "Grass", "tiles/grass.png", 0, ItemCollision::Full, 4, 0, 0, 1,
         "Green grass.", 0, 0},
        {18, 0, 0, ItemType::Fist, "Fist", "hands/fist.png", 0, ItemCollision::None, 0, 0, 0, 0,
         "Your bare hands.", 0, 0},
        {32, 0, 0, ItemType::Wrench, "Wrench", "hands/wrench.png", 0, ItemCollision::None, 0, 0, 0, 1,
         "Interact with the world.", 0, 0},
        {112, 0, 0, ItemType::Foreground, "Gems", "tiles/gems.png", 0, ItemCollision::None, 0, 0, 0, 0,
         "Shiny!", 0, 0},
        {242, 0, 0, ItemType::Lock, "World Lock", "locks/world_lock.png", 0, ItemCollision::Full, 4, 0,
         0, 1, "Locks a world to you.", 0, 0},
    };
    return catalog;
}

// Active catalog (official items.dat parsed at startup), falling back to the
// builtin set until/unless it loads.
static std::vector<ItemDef> g_activeCatalog;
static std::unordered_map<int, const ItemDef*> g_activeIndex;

const std::vector<ItemDef>& activeCatalog()
{
    return g_activeCatalog;
}

void setActiveCatalog(const std::vector<ItemDef>& catalog)
{
    g_activeCatalog = catalog;
    g_activeIndex.clear();
    for (const ItemDef& item : g_activeCatalog)
        g_activeIndex[item.id] = &item;
}

const ItemDef* findItemById(int id)
{
    if (!g_activeIndex.empty())
    {
        auto it = g_activeIndex.find(id);
        return it != g_activeIndex.end() ? it->second : nullptr;
    }
    for (const ItemDef& item : builtinCatalog())
    {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

bool parseItemsDat(const std::vector<uint8_t>& blob, std::vector<ItemDef>& out)
{
    out.clear();
    if (blob.size() < 6)
        return false;
    uint32_t count = 0;
    std::memcpy(&count, blob.data() + 2, 4);
    if (count == 0 || count > 100000)
        return false;

    // Pass 1: item boundaries. ids are sequential (0,1,2,...) stored as
    // little-endian u32 at each item start. Candidates are validated by a
    // plausible name length (1-128) and the next id appearing within 64 KiB.
    std::vector<std::size_t> starts;
    starts.reserve(count);
    std::size_t scan = 6;
    for (uint32_t id = 0; id < count; ++id)
    {
        bool found = false;
        std::size_t limit = scan + 2097152 < blob.size() ? scan + 2097152 : blob.size();
        std::size_t p = scan;
        while (p + 12 <= limit)
        {
            // Fast-skip to the next byte matching the id's low byte.
            const void* hit = std::memchr(blob.data() + p, id & 0xFF, limit - p);
            if (hit == nullptr)
                break;
            p = static_cast<std::size_t>(static_cast<const uint8_t*>(hit) - blob.data());
            if (blob[p + 1] != ((id >> 8) & 0xFF) || blob[p + 2] != 0 || blob[p + 3] != 0)
            {
                ++p;
                continue;
            }
            uint16_t nameLen = static_cast<uint16_t>(blob[p + 8] | (blob[p + 9] << 8));
            if (nameLen == 0 || nameLen > 128)
            {
                ++p;
                continue;
            }
            if (id + 1 < count)
            {
                bool nextFound = false;
                std::size_t nextLimit = p + 4 + 65536 < blob.size() ? p + 4 + 65536 : blob.size();
                for (std::size_t q = p + 4; q + 4 <= nextLimit; ++q)
                {
                    if (blob[q] == ((id + 1) & 0xFF) && blob[q + 1] == (((id + 1) >> 8) & 0xFF) &&
                        blob[q + 2] == 0 && blob[q + 3] == 0)
                    {
                        nextFound = true;
                        break;
                    }
                }
                if (!nextFound)
                {
                    ++p;
                    continue;
                }
            }
            starts.push_back(p);
            scan = p + 4;
            found = true;
            break;
        }
        if (!found)
            return false;
    }

    // Pass 2: per item the head layout is deterministic (verified against
    // known landmarks): id u32, prop/cat/type/pad, name (u16 + xor), texture,
    // hash u32, u8, ingredient u32, 4 u8s, collision u8, hits u8 (/6),
    // reset u32, cloth u8, rarity u16, u8, audio, u32, 4 u8s, 4 strings,
    // 16 u8s, growTime u32. The tail layout varies per revision, so splice
    // pairs (seeds) are scanned backwards from the next item boundary.
    out.resize(count);
    for (uint32_t id = 0; id < count; ++id)
    {
        std::size_t pos = starts[id];
        std::size_t end = id + 1 < count ? starts[id + 1] : blob.size();
        ItemDef& item = out[id];
        item.id = static_cast<uint16_t>(id);
        pos += 4;
        if (pos + 4 > end)
            continue;
        item.property = blob[pos];
        item.category = blob[pos + 1];
        item.type = static_cast<ItemType>(blob[pos + 2]);
        pos += 4;
        auto readString = [&](std::string& value, std::size_t maxLen) {
            if (pos + 2 > end)
                return false;
            uint16_t length = static_cast<uint16_t>(blob[pos] | (blob[pos + 1] << 8));
            pos += 2;
            if (pos + length > end || length > maxLen)
                return false;
            value.assign(reinterpret_cast<const char*>(blob.data() + pos), length);
            pos += length;
            return true;
        };
        std::string name;
        if (!readString(name, 512))
            continue;
        for (std::size_t c = 0; c < name.size(); ++c)
            name[c] = static_cast<char>(name[c] ^ kNameToken[(c + item.id) % kNameTokenLength]);
        item.name = name;
        if (!readString(item.texture, 256))
            continue;
        pos += 4 + 1; // texture hash + u8
        pos += 4 + 4; // ingredient + 4 unknown bytes
        if (pos + 2 > end)
            continue;
        item.collision = static_cast<ItemCollision>(blob[pos]);
        item.hits = blob[pos + 1] != 0 ? static_cast<uint8_t>(blob[pos + 1] / 6) : 0;
        pos += 2 + 4 + 1 + 2 + 1; // reset, cloth, rarity, u8
        std::string audio;
        if (!readString(audio, 256))
            continue;
        pos += 4 + 4;
        for (int k = 0; k < 4; ++k)
        {
            std::string s;
            if (!readString(s, 256))
                break;
        }
        if (pos + 16 + 4 > end)
            continue;
        pos += 16;
        std::memcpy(&item.growTimeSeconds, blob.data() + pos, 4);
        pos += 4;

        if (item.type == ItemType::Seed)
        {
            // Splice pair sits near the item end; scan backwards for the last
            // plausible (a,b) pair before the fixed postlude.
            std::size_t scanEnd = end >= 12 ? end - 12 : pos;
            for (std::size_t p = scanEnd; p > pos && p >= starts[id] + 4; --p)
            {
                uint16_t a = static_cast<uint16_t>(blob[p] | (blob[p + 1] << 8));
                uint16_t b = static_cast<uint16_t>(blob[p + 2] | (blob[p + 3] << 8));
                if ((a > 0 || b > 0) && a < count && b < count)
                {
                    item.spliceA = a;
                    item.spliceB = b;
                    break;
                }
            }
        }
    }
    return true;
}

std::vector<uint8_t> encodeItemsDat(const std::vector<ItemDef>& catalog)
{
    std::vector<uint8_t> out;
    appendU16(out, kItemsDatVersion);
    appendU32(out, static_cast<uint32_t>(catalog.size()));

    for (const ItemDef& item : catalog)
    {
        appendU16(out, item.id);
        appendU16(out, 0);
        appendU8(out, item.property);
        appendU8(out, item.category);
        appendU8(out, static_cast<uint8_t>(item.type));
        appendU8(out, 0); // Material slot.

        appendI16(out, static_cast<int16_t>(item.name.size()));
        for (std::size_t i = 0; i < item.name.size(); ++i)
            appendU8(out, static_cast<uint8_t>(item.name[i] ^ kNameToken[(i + item.id) % kNameTokenLength]));

        appendCountedString(out, item.texture);
        appendI32(out, 0);
        appendU8(out, 0);
        appendI32(out, item.ingredient);
        appendZeroes(out, 1);
        appendU8(out, static_cast<uint8_t>(item.collision));
        appendU8(out, item.hits);
        appendI32(out, item.hitResetSeconds);
        appendU8(out, item.clothType);
        appendI16(out, item.rarity);
        appendU8(out, 0);
        appendCountedString(out, "");
        appendI32(out, 0);
        appendZeroes(out, 4);
        for (int i = 0; i < 4; ++i)
            appendCountedString(out, "");
        appendZeroes(out, 16);
        appendI32(out, 0); // Growth tick.
        appendI16(out, 0);
        appendI16(out, 0);
        for (int i = 0; i < 3; ++i)
            appendCountedString(out, "");
        appendZeroes(out, 80);

        // Version-gated tail fields (numeric unknowns stay zero, strings empty).
        appendCountedString(out, ""); // v11+
        appendI32(out, 0); // v12+
        appendZeroes(out, 9);
        appendI32(out, 0); // v13+
        appendI32(out, 0); // v14+
        appendZeroes(out, 25); // v15+
        appendCountedString(out, "");
        appendCountedString(out, ""); // v16+
        appendI32(out, 0); // v17+
        appendI32(out, 0); // v18+
        appendZeroes(out, 9); // v19+
        appendI16(out, 0); // v21+
        appendCountedString(out, item.info); // v22+ info text
        appendU16(out, item.spliceA); // v23+
        appendU16(out, item.spliceB);
        appendU8(out, 0); // v24+
        appendI16(out, 0); // v25+
        appendI32(out, 0);
        appendU8(out, 0); // v26+
    }
    return out;
}

std::string decodeItemName(const std::vector<uint8_t>& blob, std::size_t& pos, uint16_t itemId)
{
    if (pos + 2 > blob.size())
        return "";
    int16_t length = static_cast<int16_t>(blob[pos] | (blob[pos + 1] << 8));
    pos += 2;

    std::string name;
    for (int16_t i = 0; i < length && pos < blob.size(); ++i, ++pos)
        name.push_back(static_cast<char>(blob[pos] ^ kNameToken[(i + itemId) % kNameTokenLength]));
    return name;
}

uint32_t fnv1a32(const std::vector<uint8_t>& data)
{
    uint32_t hash = 2166136261u;
    for (uint8_t byte : data)
    {
        hash ^= byte;
        hash *= 16777619u;
    }
    return hash;
}

// CRC-32 (IEEE 802.3, reflected) — the hash the Growtopia client associates
// with an items.dat payload.
uint32_t crc32IEEE(const std::vector<uint8_t>& data)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint8_t byte : data)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::vector<std::string> decodeAllNames(const std::vector<uint8_t>& blob)
{
    std::vector<std::string> names;
    auto need = [&](std::size_t pos, std::size_t count) { return pos + count <= blob.size(); };
    auto skip = [&](std::size_t& pos, std::size_t count) {
        if (!need(pos, count))
            return false;
        pos += count;
        return true;
    };
    auto skipCounted = [&](std::size_t& pos) {
        if (!need(pos, 2))
            return false;
        uint16_t length = static_cast<uint16_t>(blob[pos] | (blob[pos + 1] << 8));
        return skip(pos, static_cast<std::size_t>(2 + length));
    };

    std::size_t pos = 0;
    if (!skip(pos, 6))
        return names;
    uint32_t count = static_cast<uint32_t>(blob[2] | (blob[3] << 8) | (blob[4] << 16) | (blob[5] << 24));

    for (uint32_t i = 0; i < count; ++i)
    {
        if (!need(pos, 2))
            break;
        uint16_t id = static_cast<uint16_t>(blob[pos] | (blob[pos + 1] << 8));
        if (!skip(pos, 8))
            break;
        names.push_back(decodeItemName(blob, pos, id));

        bool ok = skipCounted(pos); // Texture.
        ok = ok && skip(pos, 4 + 1 + 4 + 1 + 1 + 1 + 4 + 1 + 2 + 1);
        ok = ok && skipCounted(pos); // Audio.
        ok = ok && skip(pos, 4 + 2 + 2);
        for (int k = 0; k < 4 && ok; ++k)
            ok = skipCounted(pos);
        ok = ok && skip(pos, 16 + 4 + 2 + 2);
        for (int k = 0; k < 3 && ok; ++k)
            ok = skipCounted(pos);
        ok = ok && skip(pos, 80);
        ok = ok && skipCounted(pos); // v11+
        ok = ok && skip(pos, 4 + 9); // v12+
        ok = ok && skip(pos, 4); // v13+
        ok = ok && skip(pos, 4); // v14+
        ok = ok && skip(pos, 25); // v15+
        ok = ok && skipCounted(pos);
        ok = ok && skipCounted(pos); // v16+
        ok = ok && skip(pos, 4); // v17+
        ok = ok && skip(pos, 4); // v18+
        ok = ok && skip(pos, 9); // v19+
        ok = ok && skip(pos, 2); // v21+
        ok = ok && skipCounted(pos); // v22+
        ok = ok && skip(pos, 2 + 2 + 1 + 2 + 4 + 1); // v23-v26
        if (!ok)
            break;
    }
    return names;
}

} // namespace WildanDev
