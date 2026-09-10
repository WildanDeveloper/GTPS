#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace WildanDev
{

constexpr int kWorldWidth = 100;
constexpr int kWorldHeight = 60;
constexpr std::size_t kMaxLoadedWorlds = 512; // Soft cap against world-name floods.

// Real Growtopia item ids (verified against the official items.dat).
constexpr int16_t kDirtItemId = 2;
constexpr int16_t kLavaItemId = 4;
constexpr int16_t kMainDoorItemId = 6;
constexpr int16_t kBedrockItemId = 8;
constexpr int16_t kCaveBackgroundItemId = 14;
constexpr int16_t kGrassItemId = 16;
constexpr int16_t kGemsItemId = 112;
constexpr int16_t kWorldLockItemId = 242;

// A placed door (id 12) with optional destination ("WORLD" or "WORLD:ID").
struct WorldDoor
{
    int x{0};
    int y{0};
    std::string label;
    std::string dest; // empty = goes back to spawn
    std::string id; // optional target id for other doors
};

// A placed sign (id 20) with editable text.
struct WorldSign
{
    int x{0};
    int y{0};
    std::string text;
};

// A planted seed tree (tile.fg = the seed item id).
struct WorldTree
{
    int x{0};
    int y{0};
    uint64_t plantedAt{0}; // unix seconds
    uint8_t fruit{1};
};

// A dropped item lying in the world.
struct WorldObject
{
    int id{0};
    int count{0};
    float x{0.0f};
    float y{0.0f};
    uint32_t uid{0};
};

// One map cell: foreground/background item ids plus 4 state bytes.
struct Tile
{
    int16_t fg{0};
    int16_t bg{0};
    uint8_t state[4] = {0, 0, 0, 0};
};

struct World
{
    std::string name;
    int ownerId{0};
    std::vector<Tile> tiles{kWorldWidth * kWorldHeight};
    int spawnTileX{50};
    int spawnTileY{10};
    int visitorCount{0};
    int nextNetId{0};
    bool dirty{false};
    bool isPublic{false}; // world lock: anyone may build

    // Dropped objects, placed doors/signs + in-memory punch damage.
    std::vector<WorldObject> objects;
    std::vector<WorldDoor> doors;
    std::vector<WorldSign> signs;
    std::vector<WorldTree> trees;
    uint32_t lastObjectId{0};
    std::unordered_map<int, std::pair<int, long long>> damage; // tile idx -> (hits, last hit ms)

    Tile& at(int x, int y) { return tiles[y * kWorldWidth + x]; }
    const Tile& at(int x, int y) const { return tiles[y * kWorldWidth + x]; }
    bool inside(int x, int y) const
    {
        return x >= 0 && y >= 0 && x < kWorldWidth && y < kWorldHeight;
    }
};

class Database;

// Owns every loaded world, generates new ones and persists dirty maps.
class WorldManager
{
public:
    explicit WorldManager(Database& database) : m_database(database) {}

    World& getOrCreate(const std::string& name);
    void replace(const std::string& name, World&& world);
    void saveDirty();
    void saveAll();
    // Drops worlds nobody is visiting and without unsaved changes, so a
    // flood of unique world names cannot grow memory/DB without bound.
    void pruneEmpty();
    std::vector<std::pair<std::string, int>> listWorlds() const;

private:
    void generate(World& world);
    void applyTiles(World& world, const std::vector<uint8_t>& blob);

    Database& m_database;
    std::map<std::string, World> m_worlds;
};

// Serializes a world into the client map-data layout.
std::vector<uint8_t> serializeWorld(const World& world);

// Dropped-object persistence (bounds-checked on load). parseObjects reports
// how many bytes it consumed so the extras section can follow it.
std::vector<uint8_t> serializeObjects(const World& world);
void parseObjects(World& world, const std::vector<uint8_t>& blob, std::size_t& consumed);

// Doors + signs + world flags (stored alongside the objects blob).
std::vector<uint8_t> serializeWorldExtras(const World& world);
void parseWorldExtras(World& world, const std::vector<uint8_t>& blob, std::size_t offset);

// Appends the per-tile extra data (door destination/label, sign text) the
// client expects after the 8-byte tile core in map data and tile updates.
void appendTileExtras(const World& world, int x, int y, std::vector<uint8_t>& out);

} // namespace WildanDev
