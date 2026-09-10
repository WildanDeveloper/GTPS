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

    // Dropped objects + in-memory punch damage (damage is not persisted).
    std::vector<WorldObject> objects;
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

// Dropped-object persistence (bounds-checked on load).
std::vector<uint8_t> serializeObjects(const World& world);
void parseObjects(World& world, const std::vector<uint8_t>& blob);

} // namespace WildanDev
