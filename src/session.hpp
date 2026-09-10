#pragma once

#include <cstdint>
#include <enet/enet.h>

#include <string>
#include <utility>
#include <vector>

namespace WildanDev
{

// Live state of one connected client.
struct Session
{
    ENetPeer* peer{nullptr};
    uint32_t playerId{0};
    std::string growId;
    std::string country;
    int roleId{0};
    bool ghost{false};
    bool authenticated{false};

    // World presence.
    std::string worldName;
    int netId{0};
    float posX{0.0f};
    float posY{0.0f};

    // Gameplay state.
    bool muted{false};
    int gems{0};
    int hp{10}; // Lava touches drain this; at 0 the player respawns.
    uint32_t skinColor{0}; // RGBA skin tint applied through OnSetClothing.
    std::vector<std::pair<int, int>> inventory; // (item_id, count)
};

} // namespace WildanDev
