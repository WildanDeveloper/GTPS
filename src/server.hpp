#pragma once

#include <enet/enet.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <unordered_map>

#include "config.hpp"
#include "database.hpp"
#include "login.hpp"
#include "protocol.hpp"
#include "roles.hpp"
#include "session.hpp"
#include "variant.hpp"
#include "world.hpp"

namespace WildanDev
{

// /<emote> command name -> glyph shown in the bubble (GT growmoji set).
inline const std::unordered_map<std::string, std::string> kEmoteGlyphs = {
    {"wl", "\u0101"}, {"yes", "\u0102"}, {"love", "\u0104"}, {"oops", "\u0105"},
    {"shy", "\u0106"}, {"wink", "\u0107"}, {"tongue", "\u0108"}, {"agree", "\u0109"},
    {"sleep", "\u010a"}, {"punch", "\u010b"}, {"music", "\u010c"}, {"build", "\u010d"},
    {"megaphone", "\u010e"}, {"sigh", "\u010f"}, {"mad", "\u0110"}, {"wow", "\u0111"},
    {"dance", "\u0112"}, {"see-no-evil", "\u0113"}, {"bheart", "\u0114"}, {"heart", "\u0115"},
    {"grow", "\u0116"}, {"gems", "\u0117"}, {"kiss", "\u0118"}, {"lol", "\u011a"},
    {"smile", "\u011b"}, {"cool", "\u011c"}, {"cry", "\u011d"}, {"vend", "\u011e"},
    {"bunny", "\u011f"}, {"cactus", "\u0120"}, {"pine", "\u0121"}, {"peace", "\u0122"},
    {"terror", "\u0123"}, {"troll", "\u0124"}, {"evil", "\u0125"}, {"fireworks", "\u0126"},
    {"football", "\u0127"}, {"alien", "\u0128"}, {"party", "\u0129"}, {"pizza", "\u012a"},
    {"clap", "\u012b"}, {"song", "\u012c"}, {"ghost", "\u012d"}, {"nuke", "\u012e"},
    {"halo", "\u012f"}, {"turkey", "\u0130"}, {"gift", "\u0131"}, {"cake", "\u0132"},
    {"heartarrow", "\u0133"}, {"lucky", "\u0134"}, {"shamrock", "\u0135"}, {"grin", "\u0136"},
    {"ill", "\u0137"}, {"eyes", "\u0138"}, {"weary", "\u0139"}, {"moyai", "\u013a"},
    {"plead", "\u013b"},
};

struct ServerConfig
{
    std::string bindHost{"0.0.0.0"};
    uint16_t bindPort{17091};
    std::size_t maxPeers{64};
    std::string publicHost{"127.0.0.1"};
    std::string loginHost{"0.0.0.0"};
    int loginPort{8092};
    std::string resourcesDir{"resources"};
    DbConfig db;
    std::string rolesPath{"resources/roles.conf"};
};

struct PendingAuth
{
    uint32_t playerId{0};
    int roleId{0};
    bool muted{false};
    bool isNew{false};
    int gems{0};
    uint32_t ip{0}; // Client IPv4 (host order); reconnect must match.
    std::chrono::steady_clock::time_point expires{};
};

struct LoginFailure
{
    int count{0};
    std::chrono::steady_clock::time_point windowStart{};
};

struct CommandEntry
{
    std::string requiredPermission;
    std::string usage;
    std::function<void(Session& session, const std::vector<std::string>& args)> handler;
};

class GameServer
{
public:
    GameServer() = default;
    ~GameServer();

    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    bool configure(const std::string& configDir);
    bool start();
    void runOnce();
    void shutdown();

    // Messaging primitives.
    void sendAction(ENetPeer* peer, const std::string& action, const std::string& payload);
    void sendConsoleMessage(ENetPeer* peer, const std::string& text);
    void sendVariant(ENetPeer* peer, const std::vector<VariantValue>& args, int netId = -1, int delayMs = 0);
    void sendMapData(ENetPeer* peer, const World& world);
    void sendSpawn(ENetPeer* peer, const Session& subject, bool local);
    void sendSuperMain(ENetPeer* peer);
    void sendItemDatabase(ENetPeer* peer);
    void sendInventoryState(Session& session);
    void broadcastChat(Session& session, const std::string& text);
    void grantStarterKit(Session& session);
    void broadcastToWorld(const World& world, const std::vector<uint8_t>& raw, ENetPeer* except = nullptr);
    bool joinWorld(Session& session, const std::string& worldName);

    // Inventory mutations (persisted immediately, state pushed to the client).
    bool takeItem(Session& session, int itemId, int count);
    void giveItem(Session& session, int itemId, int count);
    void sendInventoryDelta(const Session& session, int itemId, int delta);

    // World life cycle: death/respawn at the main door, dropped objects.
    void respawnPlayer(Session& session);
    void leaveWorld(Session& session);
    void dropObject(World& world, int itemId, int count, int tileX, int tileY);
    void removeDropObject(World& world, Session& collector, uint32_t uid);
    void sendObjectState(const World& world, int32_t marker, uint32_t uid, int itemId, int count, float x,
                         float y);
    void sendTileUpdate(World& world, int x, int y);

    // Client dialogs (wrench editing, drop/trash pickers, store).
    void handleDialogReturn(Session& session, const TextPacket& packet);
    void sendWrenchTileDialog(Session& session, World& world, int x, int y);
    void sendWrenchPlayerDialog(Session& session, int netId);
    void sendStoreDialog(Session& session);
    bool purchaseStoreItem(Session& session, const std::string& item);
    void sendSetClothing(const Session& subject, bool toWholeWorld);
    std::string roleColor(const Session& session) const;

    // Command framework.
    void registerCommand(const std::string& name, CommandEntry entry);
    void executeCommand(Session& session, const std::string& text);

    Session* findSession(const std::string& growId);

    // True when the session may moderate the target (strictly higher rank,
    // or the session itself).
    bool canModerate(Session& session, Session& target, std::string* reason);

    const Role* findRoleByName(const std::string& name) const;

    RoleManager& roles() { return m_roles; }
    Database& database() { return m_database; }
    const ServerConfig& config() const { return m_config; }

private:
    void handleConnect(ENetEvent& event);
    void handleDisconnect(ENetEvent& event);
    void handleReceive(ENetEvent& event);
    void handleTextPacket(Session& session, const TextPacket& packet);
    void handleTankPacket(Session& session, const uint8_t* data, std::size_t length);
    void handleLoginProtocol(Session& session, const TextPacket& packet);
    void handleLoginIdentity(Session& session, const TextPacket& packet);
    void handleEnterGame(Session& session);

    void registerBuiltinCommands();

    ServerConfig m_config;
    RoleManager m_roles;
    Database m_database;
    LoginService m_login;
    WorldManager m_worlds{m_database};
    std::vector<uint8_t> m_itemsDat;
    uint32_t m_itemsHash{0};
    ENetHost* m_host{nullptr};
    bool m_enetInitialized{false};
    std::unordered_map<ENetPeer*, Session> m_sessions;
    std::unordered_map<std::string, CommandEntry> m_commands;
    std::unordered_map<std::string, PendingAuth> m_pending;
    std::unordered_map<std::string, LoginFailure> m_loginFailures;
    std::chrono::steady_clock::time_point m_lastSave{std::chrono::steady_clock::now()};
};

} // namespace WildanDev
