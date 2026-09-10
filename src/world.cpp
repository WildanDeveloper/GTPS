#include "world.hpp"
#include "database.hpp"
#include "items.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <random>

namespace WildanDev
{

namespace
{

void appendInt16(std::vector<uint8_t>& out, int16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void appendInt32(std::vector<uint8_t>& out, int32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

std::string upperCopy(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

} // namespace

World& WorldManager::getOrCreate(const std::string& name)
{
    std::string key = upperCopy(name);
    auto it = m_worlds.find(key);
    if (it != m_worlds.end())
        return it->second;

    // Soft cap: try to make room by evicting an idle, saved world.
    if (m_worlds.size() >= kMaxLoadedWorlds)
    {
        for (auto candidate = m_worlds.begin(); candidate != m_worlds.end(); ++candidate)
        {
            if (candidate->second.visitorCount == 0 && !candidate->second.dirty)
            {
                m_worlds.erase(candidate);
                break;
            }
        }
        if (m_worlds.size() >= kMaxLoadedWorlds)
            logWarn("World cache at cap (" + std::to_string(m_worlds.size()) + "); keeping '" + key + "' anyway");
    }

    World world;
    world.name = key;

    int ownerId = 0;
    std::vector<uint8_t> blob;
    std::vector<uint8_t> objectBlob;
    if (m_database.loadWorld(key, ownerId, blob, objectBlob) && blob.size() == world.tiles.size() * 8)
    {
        world.ownerId = ownerId;
        applyTiles(world, blob);
        std::size_t consumed = 0;
        parseObjects(world, objectBlob, consumed);
        parseWorldExtras(world, objectBlob, consumed);
        // Restore the spawn point from wherever the main door ended up.
        bool foundDoor = false;
        for (int y = 0; y < kWorldHeight && !foundDoor; ++y)
        {
            for (int x = 0; x < kWorldWidth && !foundDoor; ++x)
            {
                if (world.at(x, y).fg == kMainDoorItemId)
                {
                    world.spawnTileX = x;
                    world.spawnTileY = y;
                    foundDoor = true;
                }
            }
        }
        logInfo("Loaded world " + key + " from database");
    }
    else
    {
        generate(world);
        world.dirty = true;
        logInfo("Generated new world " + key);
    }

    auto inserted = m_worlds.emplace(key, std::move(world));
    return inserted.first->second;
}

void WorldManager::generate(World& world)
{
    // Real Growtopia START-world layout (mirrors the working Gurotopia
    // reference): cave background + dirt from y=37 down, grass on the
    // surface row, rock/lava speckles and caves, bedrock from y=54, and a
    // main door standing on a bedrock support near the surface.
    static std::mt19937 rng{std::random_device{}()};

    for (int y = 0; y < kWorldHeight; ++y)
    {
        for (int x = 0; x < kWorldWidth; ++x)
        {
            Tile& tile = world.at(x, y);
            if (y < 37)
                continue;
            tile.bg = kCaveBackgroundItemId;
            if (y >= 54)
            {
                tile.fg = kBedrockItemId;
            }
            else if (y == 37)
            {
                tile.fg = kGrassItemId;
            }
            else
            {
                tile.fg = kDirtItemId;
                if (y <= 47 && rng() % 29 == 0)
                    tile.fg = 0; // small cave pocket
                else if (y <= 49 && rng() % 39 < 2)
                    tile.fg = 10; // rock
                else if (y >= 51 && y <= 53 && rng() % 9 < 3)
                    tile.fg = kLavaItemId;
            }
        }
    }

    int doorX = 2 + static_cast<int>(rng() % (kWorldWidth - 4));
    world.at(doorX, 36).fg = kMainDoorItemId;
    world.at(doorX, 37).fg = kBedrockItemId; // support below the door
    world.spawnTileX = doorX;
    world.spawnTileY = 36;
}

void WorldManager::applyTiles(World& world, const std::vector<uint8_t>& blob)
{
    for (std::size_t i = 0; i < world.tiles.size(); ++i)
    {
        const uint8_t* raw = blob.data() + i * 8;
        world.tiles[i].fg = static_cast<int16_t>(raw[0] | (raw[1] << 8));
        world.tiles[i].bg = static_cast<int16_t>(raw[2] | (raw[3] << 8));
        world.tiles[i].state[0] = raw[4];
        world.tiles[i].state[1] = raw[5];
        world.tiles[i].state[2] = raw[6];
        world.tiles[i].state[3] = raw[7];
    }
}

void WorldManager::saveDirty()
{
    for (auto& [name, world] : m_worlds)
    {
        if (!world.dirty)
            continue;
        std::vector<uint8_t> blob;
        blob.reserve(world.tiles.size() * 8);
        for (const Tile& tile : world.tiles)
        {
            appendInt16(blob, tile.fg);
            appendInt16(blob, tile.bg);
            blob.push_back(tile.state[0]);
            blob.push_back(tile.state[1]);
            blob.push_back(tile.state[2]);
            blob.push_back(tile.state[3]);
        }
        std::vector<uint8_t> objects = serializeObjects(world);
        std::vector<uint8_t> extras = serializeWorldExtras(world);
        objects.insert(objects.end(), extras.begin(), extras.end());
        if (m_database.upsertWorld(name, world.ownerId, blob.data(), blob.size(), objects.data(), objects.size()))
            world.dirty = false;
    }
}

void WorldManager::saveAll()
{
    for (auto& [name, world] : m_worlds)
        world.dirty = true;
    saveDirty();
}

void WorldManager::pruneEmpty()
{
    for (auto it = m_worlds.begin(); it != m_worlds.end();)
    {
        if (it->second.visitorCount == 0 && !it->second.dirty)
            it = m_worlds.erase(it);
        else
            ++it;
    }
}

std::vector<std::pair<std::string, int>> WorldManager::listWorlds() const
{
    std::vector<std::pair<std::string, int>> result;
    for (const auto& [name, world] : m_worlds)
    {
        if (world.visitorCount > 0)
            result.emplace_back(name, world.visitorCount);
    }
    return result;
}

std::vector<uint8_t> serializeWorld(const World& world)
{
    std::vector<uint8_t> out;
    appendInt16(out, 0);
    appendInt32(out, 0);
    appendInt16(out, static_cast<int16_t>(world.name.size()));
    out.insert(out.end(), world.name.begin(), world.name.end());
    appendInt32(out, kWorldWidth);
    appendInt32(out, kWorldHeight);
    appendInt16(out, static_cast<int16_t>(world.tiles.size()));
    appendInt32(out, 0);
    appendInt16(out, 0);
    out.push_back(0);

    for (std::size_t i = 0; i < world.tiles.size(); ++i)
    {
        const Tile& tile = world.tiles[i];
        appendInt16(out, tile.fg);
        appendInt16(out, tile.bg);
        out.push_back(tile.state[0]);
        out.push_back(tile.state[1]);
        out.push_back(tile.state[2]);
        out.push_back(tile.state[3]);

        // Door-type tiles append extra data after the 8-byte core: a flag
        // byte, then (flag&2) u16 destination length + destination, then
        // (flag&1) u16 label length + label. Tiles without extra data emit
        // just the zero flag byte.
        appendTileExtras(world, static_cast<int>(i % kWorldWidth), static_cast<int>(i / kWorldWidth), out);
    }

    appendInt32(out, 0);
    appendInt32(out, 0);
    appendInt32(out, 0);
    appendInt32(out, 0);
    appendInt32(out, 0);
    return out;
}

std::vector<uint8_t> serializeObjects(const World& world)
{
    std::vector<uint8_t> out;
    appendInt32(out, static_cast<int32_t>(world.objects.size()));
    for (const WorldObject& object : world.objects)
    {
        appendInt32(out, static_cast<int32_t>(object.uid));
        appendInt32(out, object.id);
        appendInt32(out, object.count);
        appendInt32(out, static_cast<int32_t>(object.x));
        appendInt32(out, static_cast<int32_t>(object.y));
    }
    return out;
}

void parseObjects(World& world, const std::vector<uint8_t>& blob, std::size_t& consumed)
{
    world.objects.clear();
    std::size_t pos = 0;
    auto readU32 = [&](uint32_t& value) {
        if (pos + 4 > blob.size())
            return false;
        std::memcpy(&value, blob.data() + pos, 4);
        pos += 4;
        return true;
    };
    consumed = 0;
    uint32_t count = 0;
    if (!readU32(count) || count > 100000)
        return; // corrupt, empty, or extras-only blob
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t uid = 0, id = 0, count2 = 0, x = 0, y = 0;
        if (!readU32(uid) || !readU32(id) || !readU32(count2) || !readU32(x) || !readU32(y))
            return; // truncated: keep what parsed so far
        WorldObject object;
        object.uid = uid;
        object.id = static_cast<int>(id);
        object.count = static_cast<int>(count2);
        object.x = static_cast<float>(x);
        object.y = static_cast<float>(y);
        world.objects.push_back(object);
        if (uid > world.lastObjectId)
            world.lastObjectId = uid;
    }
    consumed = pos;
}

void appendTileExtras(const World& world, int x, int y, std::vector<uint8_t>& out)
{
    const Tile& tile = world.at(x, y);

    // Planted seed tiles carry the tree state: i32 ready-unix + u8 fruit.
    const ItemDef* tileItem = findItemById(tile.fg);
    if (tileItem != nullptr && tileItem->type == ItemType::Seed)
    {
        uint64_t ready = 0;
        uint8_t fruit = 0;
        for (const WorldTree& tree : world.trees)
        {
            if (tree.x == x && tree.y == y)
            {
                ready = tree.plantedAt + tileItem->growTimeSeconds;
                fruit = tree.fruit;
                break;
            }
        }
        appendInt32(out, static_cast<int32_t>(ready));
        out.push_back(fruit);
        return;
    }

    bool isDoor = tile.fg == 6 || tileItem != nullptr && tileItem->type == ItemType::Door;
    bool isSign = tile.fg == 20;
    if (!isDoor && !isSign)
        return;

    if (isDoor)
    {
        std::string label, dest;
        if (tile.fg == 6)
        {
            // Main door: label defaults to the world name, no destination.
            label = world.name;
        }
        else
        {
            for (const WorldDoor& door : world.doors)
            {
                if (door.x == x && door.y == y)
                {
                    label = door.label;
                    dest = door.dest;
                    break;
                }
            }
        }
        (void)0;
        uint8_t flags = 0;
        if (!dest.empty())
            flags |= 0x02;
        if (!label.empty())
            flags |= 0x01;
        out.push_back(flags);
        if (flags & 0x02)
        {
            out.push_back(static_cast<uint8_t>(dest.size() & 0xFF));
            out.push_back(static_cast<uint8_t>((dest.size() >> 8) & 0xFF));
            for (char c : dest)
                out.push_back(static_cast<uint8_t>(c));
        }
        if (flags & 0x01)
        {
            out.push_back(static_cast<uint8_t>(label.size() & 0xFF));
            out.push_back(static_cast<uint8_t>((label.size() >> 8) & 0xFF));
            for (char c : label)
                out.push_back(static_cast<uint8_t>(c));
        }
        return;
    }

    // Sign: u16 text length + text (no flag byte).
    std::string text;
    for (const WorldSign& sign : world.signs)
    {
        if (sign.x == x && sign.y == y)
        {
            text = sign.text;
            break;
        }
    }
    if (text.size() > 512)
        text.resize(512);
    out.push_back(static_cast<uint8_t>(text.size() & 0xFF));
    out.push_back(static_cast<uint8_t>((text.size() >> 8) & 0xFF));
    for (char c : text)
        out.push_back(static_cast<uint8_t>(c));
}

// Extras blob layout (appended after the objects section):
//   u32 marker 0x57443131, u8 isPublic, u32 doorCount,
//   per door: u16 x, u16 y, u16 labelLen + bytes, u16 destLen + bytes,
//             u16 idLen + bytes, u32 signCount,
//   per sign: u16 x, u16 y, u16 textLen + bytes
constexpr uint32_t kExtrasMarker = 0x57443131;

std::vector<uint8_t> serializeWorldExtras(const World& world)
{
    std::vector<uint8_t> out;
    appendInt32(out, static_cast<int32_t>(kExtrasMarker));
    out.push_back(world.isPublic ? 1 : 0);
    appendInt32(out, static_cast<int32_t>(world.doors.size()));
    auto appendString = [&out](const std::string& text) {
        appendInt16(out, static_cast<int16_t>(std::min<std::size_t>(text.size(), 512)));
        for (std::size_t i = 0; i < text.size() && i < 512; ++i)
            out.push_back(static_cast<uint8_t>(text[i]));
    };
    for (const WorldDoor& door : world.doors)
    {
        appendInt16(out, static_cast<int16_t>(door.x));
        appendInt16(out, static_cast<int16_t>(door.y));
        appendString(door.label);
        appendString(door.dest);
        appendString(door.id);
    }
    appendInt32(out, static_cast<int32_t>(world.signs.size()));
    for (const WorldSign& sign : world.signs)
    {
        appendInt16(out, static_cast<int16_t>(sign.x));
        appendInt16(out, static_cast<int16_t>(sign.y));
        appendString(sign.text);
    }
    appendInt32(out, static_cast<int32_t>(world.trees.size()));
    for (const WorldTree& tree : world.trees)
    {
        appendInt16(out, static_cast<int16_t>(tree.x));
        appendInt16(out, static_cast<int16_t>(tree.y));
        appendInt32(out, static_cast<int32_t>(tree.plantedAt));
        out.push_back(tree.fruit);
    }
    return out;
}

void parseWorldExtras(World& world, const std::vector<uint8_t>& blob, std::size_t offset)
{
    world.doors.clear();
    world.signs.clear();
    world.trees.clear();
    world.isPublic = false;
    std::size_t pos = offset;
    auto readU32 = [&](uint32_t& value) {
        if (pos + 4 > blob.size())
            return false;
        std::memcpy(&value, blob.data() + pos, 4);
        pos += 4;
        return true;
    };
    auto readU16 = [&](uint32_t& value) {
        if (pos + 2 > blob.size())
            return false;
        value = blob[pos] | (blob[pos + 1] << 8);
        pos += 2;
        return true;
    };
    auto readString = [&](std::string& value) {
        uint32_t length = 0;
        if (!readU16(length) || pos + length > blob.size())
            return false;
        value.assign(reinterpret_cast<const char*>(blob.data() + pos), length);
        pos += length;
        return true;
    };

    uint32_t marker = 0;
    if (!readU32(marker) || marker != kExtrasMarker)
        return; // legacy world without extras
    if (pos < blob.size())
        world.isPublic = blob[pos++] != 0;
    else
        return;
    uint32_t doorCount = 0;
    if (!readU32(doorCount) || doorCount > 10000)
        return;
    for (uint32_t i = 0; i < doorCount; ++i)
    {
        uint32_t x = 0, y = 0;
        WorldDoor door;
        if (!readU16(x) || !readU16(y) || !readString(door.label) || !readString(door.dest) ||
            !readString(door.id))
            return;
        door.x = static_cast<int>(x);
        door.y = static_cast<int>(y);
        world.doors.push_back(door);
    }
    uint32_t signCount = 0;
    if (!readU32(signCount) || signCount > 10000)
        return;
    for (uint32_t i = 0; i < signCount; ++i)
    {
        uint32_t x = 0, y = 0;
        WorldSign sign;
        if (!readU16(x) || !readU16(y) || !readString(sign.text))
            return;
        sign.x = static_cast<int>(x);
        sign.y = static_cast<int>(y);
        world.signs.push_back(sign);
    }
    uint32_t treeCount = 0;
    if (!readU32(treeCount) || treeCount > 10000)
        return;
    for (uint32_t i = 0; i < treeCount; ++i)
    {
        uint32_t x = 0, y = 0, planted = 0;
        WorldTree tree;
        if (!readU16(x) || !readU16(y) || !readU32(planted))
            return;
        if (pos >= blob.size())
            return;
        tree.fruit = blob[pos++];
        tree.x = static_cast<int>(x);
        tree.y = static_cast<int>(y);
        tree.plantedAt = planted;
        world.trees.push_back(tree);
    }
}

} // namespace WildanDev
