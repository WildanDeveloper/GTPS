#pragma once

#include <enet/enet.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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
