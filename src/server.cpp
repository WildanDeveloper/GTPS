#include "server.hpp"
#include "base64.hpp"
#include "items.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <random>
#include <sstream>

namespace WildanDev
{

namespace
{

int32_t readInt32(const uint8_t* data, std::size_t offset)
{
    int32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

float readFloat(const uint8_t* data, std::size_t offset)
{
    float value = 0.0f;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

// Pack an ENet IPv4 address into a comparable 32-bit value.
uint32_t peerIpv4(const ENetPeer* peer)
{
    uint32_t value = 0;
    std::memcpy(&value, peer->address.host.v4, sizeof(value));
    return value;
}

constexpr int kTankHeaderSize = 60;
constexpr int kPacketState = 0;
constexpr int kPacketTileChange = 3;
constexpr int kPacketMapData = 4;
constexpr int kFistItemId = 18;

} // namespace

GameServer::~GameServer()
{
    m_login.stop();
    if (m_host != nullptr)
    {
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    if (m_enetInitialized)
        enet_deinitialize();
}

namespace
{

// Full-string numeric parse (rejects "17091abc" and negatives).
std::optional<int> parseInt(const char* text)
{
    if (text == nullptr || text[0] == '\0')
        return std::nullopt;
    int value = 0;
    auto [ptr, ec] = std::from_chars(text, text + std::strlen(text), value);
    if (ec != std::errc() || *ptr != '\0')
        return std::nullopt;
    return value;
}

} // namespace

bool GameServer::configure(const std::string& configDir)
{
    Config gameConf;
    if (!gameConf.load(configDir + "/gameserver.conf"))
    {
        logError("Cannot load " + configDir + "/gameserver.conf");
        return false;
    }
    m_config.bindHost = gameConf.get("bind_host", "0.0.0.0");
    m_config.bindPort = static_cast<uint16_t>(gameConf.getInt("bind_port", 17091));
    if (const char* portEnv = std::getenv("WILDANDEV_PORT"); portEnv != nullptr && portEnv[0] != '\0')
    {
        if (auto port = parseInt(portEnv); port.has_value() && *port > 0 && *port < 65536)
            m_config.bindPort = static_cast<uint16_t>(*port);
    }
    int maxPeers = gameConf.getInt("max_peers", 64);
    m_config.maxPeers = maxPeers > 0 && maxPeers <= 4096 ? static_cast<std::size_t>(maxPeers) : 64;
    m_config.publicHost = gameConf.get("public_host", "127.0.0.1");
    m_config.loginHost = gameConf.get("login_host", "0.0.0.0");
    if (auto port = parseInt(gameConf.get("login_port", "8092").c_str());
        port.has_value() && *port > 0 && *port < 65536)
        m_config.loginPort = *port;
    m_config.resourcesDir = configDir;

    Config dbConf;
    if (!dbConf.load(configDir + "/db.conf"))
    {
        logError("Cannot load " + configDir + "/db.conf");
        return false;
    }
    m_config.db.host = dbConf.get("host", "127.0.0.1");
    m_config.db.user = dbConf.get("user", "root");
    m_config.db.password = dbConf.get("password", "");
    m_config.db.name = dbConf.get("name", "wildandev");
    if (auto port = parseInt(dbConf.get("db_port", "3306").c_str()); port.has_value())
        m_config.db.port = *port;

    m_config.rolesPath = configDir + "/roles.conf";
    if (!m_roles.load(m_config.rolesPath))
        return false;
    if (!m_database.connect(m_config.db))
        return false;

    // Serve the official Growtopia items.dat (v26) so real clients can parse
    // it. The encoded builtin catalog remains the server-side metadata only.
    {
        std::ifstream itemsFile(configDir + "/items.dat", std::ios::binary);
        if (itemsFile.is_open())
        {
            m_itemsDat.assign(std::istreambuf_iterator<char>(itemsFile), std::istreambuf_iterator<char>());
            logInfo("Loaded items.dat (" + std::to_string(m_itemsDat.size()) + " bytes, crc32 " +
                    std::to_string(crc32IEEE(m_itemsDat)) + ")");
        }
        else
        {
            m_itemsDat = encodeItemsDat(builtinCatalog());
            logWarn("resources/items.dat missing; falling back to the minimal builtin catalog "
                    "(real clients will likely reject it)");
        }
    }
    // Advertise the CRC-32 of the items.dat we actually serve — the client
    // verifies the received dat against this hash and rejects it on
    // mismatch (which used to trap clients on the "updating items" screen).
    // Override with WILDANDEV_ITEMS_HASH when serving a modified database.
    m_itemsHash = crc32IEEE(m_itemsDat);
    if (const char* hashEnv = std::getenv("WILDANDEV_ITEMS_HASH"); hashEnv != nullptr && hashEnv[0] != '\0')
    {
        if (auto parsed = parseInt(hashEnv); parsed.has_value() && *parsed >= 0)
            m_itemsHash = static_cast<uint32_t>(*parsed);
    }

    registerBuiltinCommands();
    return true;
}

bool GameServer::start()
{
    if (enet_initialize() != 0)
    {
        logError("enet_initialize failed");
        return false;
    }
    m_enetInitialized = true;

    ENetAddress address{};
    address.type = ENET_ADDRESS_TYPE_IPV4;
    address.port = m_config.bindPort;

    m_host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, &address, m_config.maxPeers, 2, 0, 0);
    if (m_host == nullptr)
    {
        logError("enet_host_create failed on port " + std::to_string(m_config.bindPort));
        return false;
    }

    m_host->usingNewPacketForServer = true;
    m_host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(m_host);

    logInfo("Listening for game clients on port " + std::to_string(m_config.bindPort));

    // In-process HTTPS login service (replaces the old Python backend).
    LoginConfig login;
    login.resourcesDir = m_config.resourcesDir;
    login.certFile = m_config.resourcesDir + "/certs/server.crt";
    login.keyFile = m_config.resourcesDir + "/certs/server.key";
    login.host = m_config.loginHost;
    login.port = m_config.loginPort;
    login.db = m_config.db;
    const Role* defaultRole = m_roles.getDefaultRole();
    login.defaultRoleId = defaultRole != nullptr ? defaultRole->id : 4;

    // Optional Resend.com settings for password recovery emails.
    Config resendConf;
    if (resendConf.load(m_config.resourcesDir + "/resend.conf"))
    {
        login.resendApiKey = resendConf.get("api_key", "");
        login.resendFrom = resendConf.get("from", login.resendFrom);
        if (!login.resendApiKey.empty())
            logInfo("Password recovery email enabled via Resend");
    }

    m_login.start(login);
    return true;
}

void GameServer::runOnce()
{
    ENetEvent event{};
    while (enet_host_service(m_host, &event, 250) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            handleConnect(event);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            handleDisconnect(event);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            handleReceive(event);
            enet_packet_destroy(event.packet);
            break;
        default:
            break;
        }
    }

    auto now = std::chrono::steady_clock::now();
    if (now - m_lastSave >= std::chrono::seconds(30))
    {
        m_lastSave = now;
        m_worlds.saveDirty();
        m_worlds.pruneEmpty();
    }

    for (auto it = m_pending.begin(); it != m_pending.end();)
    {
        if (it->second.expires < now)
            it = m_pending.erase(it);
        else
            ++it;
    }

    // Expire brute-force counters so a single old failure cannot lock an
    // account forever.
    for (auto it = m_loginFailures.begin(); it != m_loginFailures.end();)
    {
        if (now - it->second.windowStart >= std::chrono::seconds(60))
            it = m_loginFailures.erase(it);
        else
            ++it;
    }
}

void GameServer::shutdown()
{
    m_worlds.saveAll();
    for (auto& [peer, session] : m_sessions)
    {
        if (session.authenticated && session.playerId != 0)
            m_database.setGems(session.playerId, session.gems);
        enet_peer_disconnect(peer, 0);
    }
    enet_host_flush(m_host);
    m_sessions.clear();
}

void GameServer::sendAction(ENetPeer* peer, const std::string& action, const std::string& payload)
{
    std::string body = "action|" + action + "\n" + payload;
    std::vector<uint8_t> data(sizeof(int32_t) + body.size(), 0x00);
    data[0] = 3; // Game message.
    std::memcpy(data.data() + sizeof(int32_t), body.data(), body.size());

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
}

void GameServer::sendVariant(ENetPeer* peer, const std::vector<VariantValue>& args, int netId, int delayMs)
{
    std::vector<uint8_t> body = encodeVariantList(args);

    TankHeader header;
    header.type = 1; // Call function.
    header.netId = netId;
    header.state = 8; // Extended flag.
    header.id = delayMs; // Client applies the variant after this many ms.
    header.dataSize = static_cast<uint32_t>(body.size());

    std::vector<uint8_t> data = encodeTankHeader(header);
    data.insert(data.end(), body.begin(), body.end());

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
}

void GameServer::sendConsoleMessage(ENetPeer* peer, const std::string& text)
{
    sendVariant(peer, {VariantValue::makeString("OnConsoleMessage"), VariantValue::makeString(text)});
}

void GameServer::registerCommand(const std::string& name, CommandEntry entry)
{
    m_commands[name] = std::move(entry);
}

Session* GameServer::findSession(const std::string& growId)
{
    // Exact case-insensitive match only. Prefix matching made /kick a
    // resolve to an arbitrary player.
    std::string wanted = growId;
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    for (auto& [peer, session] : m_sessions)
    {
        if (!session.authenticated)
            continue;
        std::string candidate = session.growId;
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (candidate == wanted)
            return &session;
    }
    return nullptr;
}

bool GameServer::canModerate(Session& session, Session& target, std::string* reason)
{
    const Role* actor = m_roles.getRole(session.roleId);
    const Role* subject = m_roles.getRole(target.roleId);
    int actorRank = actor != nullptr ? actor->rank : 0;
    int subjectRank = subject != nullptr ? subject->rank : 0;
    if (&session == &target)
        return true;
    if (subjectRank >= actorRank)
    {
        if (reason != nullptr)
            *reason = "You cannot target a player with an equal or higher rank.";
        return false;
    }
    return true;
}

const Role* GameServer::findRoleByName(const std::string& name) const
{
    for (auto& [id, role] : m_roles.all())
    {
        (void)id;
        if (role.name == name)
            return &role;
    }
    return nullptr;
}

void GameServer::executeCommand(Session& session, const std::string& text)
{
    // Split "/name arg1 arg2 ..." into tokens.
    std::vector<std::string> args;
    std::string current;
    for (char c : text.substr(1))
    {
        if (std::isspace(static_cast<unsigned char>(c)))
        {
            if (!current.empty())
            {
                args.push_back(current);
                current.clear();
            }
        }
        else
        {
            current.push_back(c);
        }
    }
    if (!current.empty())
        args.push_back(current);
    if (args.empty())
        return;

    auto it = m_commands.find(args[0]);
    if (it == m_commands.end())
    {
        sendConsoleMessage(session.peer, "Unknown command. Type /help for the list.");
        return;
    }

    const Role* role = m_roles.getRole(session.roleId);
    if (role == nullptr || !role->hasPermission(it->second.requiredPermission))
    {
        sendConsoleMessage(session.peer, "Unknown command. Type /help for the list.");
        return;
    }

    std::vector<std::string> callArgs(args.begin() + 1, args.end());
    it->second.handler(session, callArgs);
}

void GameServer::handleConnect(ENetEvent& event)
{
    Session session;
    session.peer = event.peer;
    m_sessions[event.peer] = std::move(session);

    // Server hello: 5-byte reliable packet carrying int32(1).
    uint8_t hello[5] = {1, 0, 0, 0, 0};
    ENetPacket* packet = enet_packet_create(hello, sizeof(hello), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(event.peer, 0, packet) != 0)
        enet_packet_destroy(packet);

    logInfo("Client connected");
}

void GameServer::handleDisconnect(ENetEvent& event)
{
    auto it = m_sessions.find(event.peer);
    if (it != m_sessions.end())
    {
        if (!it->second.growId.empty())
            logInfo("Client disconnected: " + it->second.growId);
        if (!it->second.worldName.empty())
        {
            World& world = m_worlds.getOrCreate(it->second.worldName);
            if (world.visitorCount > 0)
                --world.visitorCount;
        }
        m_sessions.erase(it);
    }
    event.peer->data = nullptr;
}

void GameServer::handleReceive(ENetEvent& event)
{
    auto it = m_sessions.find(event.peer);
    if (it == m_sessions.end())
        return;

    const uint8_t* data = event.packet->data;
    std::size_t length = event.packet->dataLength;
    if (length == 0)
        return;

    if (data[0] == 2 || data[0] == 3)
    {
        auto parsed = parseTextPacket(data, length);
        if (parsed.has_value())
        {
            handleTextPacket(it->second, parsed.value());
        }
        else
        {
            // Malformed text packet: dump the header so the cause is visible.
            std::ostringstream hex;
            for (std::size_t i = 0; i < length && i < 24; ++i)
                hex << (i ? " " : "") << std::hex << std::setw(2) << std::setfill('0')
                    << static_cast<int>(data[i]);
            logWarn("Unparsable text packet (" + std::to_string(length) + " bytes): " + hex.str());
        }
        return;
    }

    if (data[0] == 4)
    {
        if (length >= static_cast<std::size_t>(kTankHeaderSize))
            handleTankPacket(it->second, data, length);
        else
            logWarn("Short tank packet dropped (" + std::to_string(length) + " bytes)");
        return;
    }

    std::ostringstream hex;
    for (std::size_t i = 0; i < length && i < 24; ++i)
        hex << (i ? " " : "") << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(data[i]);
    logWarn("Dropping packet with unknown kind " + std::to_string(data[0]) + " (" + std::to_string(length) +
            " bytes): " + hex.str());
}

void GameServer::handleTextPacket(Session& session, const TextPacket& packet)
{
    if (packet.lines.empty())
        return;

    const std::string& first = packet.lines[0];
    if (first != "protocol" && first.rfind("protocol|", 0) != 0)
    {
        // Full dump (bounded) so identity/handshake issues are diagnosable.
        std::string dump;
        for (const auto& line : packet.lines)
        {
            if (!dump.empty())
                dump += " \\n ";
            dump += line.substr(0, 60);
            if (dump.size() > 400)
            {
                dump += " ...";
                break;
            }
        }
        logInfo("Text packet from " + (session.growId.empty() ? "?" : session.growId) + ": " + dump);
    }
    if (first == "protocol" || first.rfind("protocol|", 0) == 0)
    {
        handleLoginProtocol(session, packet);
        return;
    }
    if (first == "tankIDName" || first.rfind("tankIDName|", 0) == 0 || first.rfind("requestedName|", 0) == 0 ||
        !lineValue(packet.lines, "tankIDName").empty() || !lineValue(packet.lines, "requestedName").empty())
    {
        handleLoginIdentity(session, packet);
        return;
    }
    if (first.rfind("action|", 0) == 0)
    {
        std::string action = first.substr(7);
        if (action == "input")
        {
            std::string text = lineValue(packet.lines, "text");
            if (text.empty())
                return;
            if (text[0] == '/')
                executeCommand(session, text);
            else if (session.authenticated)
                broadcastChat(session, text);
        }
        else if (action == "join_request")
        {
            if (!session.authenticated)
            {
                sendAction(session.peer, "logon_fail", "");
                return;
            }
            std::string worldName = lineValue(packet.lines, "name");
            joinWorld(session, worldName);
        }
        else if (action == "enter_game")
        {
            if (session.authenticated)
                handleEnterGame(session);
        }
        else if (action == "respawn")
        {
            if (session.authenticated)
                respawnPlayer(session);
        }
        else if (action == "refresh_item_data")
        {
            if (session.authenticated)
            {
                logInfo("Serving items.dat to " + session.growId + " (" +
                        std::to_string(m_itemsDat.size()) + " bytes)");
                sendItemDatabase(session.peer);
            }
        }
        return;
    }

    logWarn("Dropping text packet with unknown action: " + first.substr(0, 120));
}

void GameServer::handleLoginProtocol(Session& session, const TextPacket& packet)
{
    std::string token = lineValue(packet.lines, "ltoken");
    if (token.empty())
    {
        sendAction(session.peer, "logon_fail", "");
        return;
    }

    auto parsed = parseLoginToken<Base64>(token);
    if (!parsed.has_value())
    {
        sendAction(session.peer, "logon_fail", "");
        logWarn("Rejected protocol packet with invalid token");
        return;
    }

    // Brute-force guard: 5 failures per 60s per growId.
    auto now = std::chrono::steady_clock::now();
    auto failure = m_loginFailures.find(parsed->growId);
    if (failure != m_loginFailures.end() && failure->second.count >= 5 &&
        now - failure->second.windowStart < std::chrono::seconds(60))
    {
        sendConsoleMessage(session.peer, "Too many failed attempts. Try again later.");
        sendAction(session.peer, "logon_fail", "");
        logWarn("Login rate limit hit for " + parsed->growId);
        return;
    }

    const Role* fallback = m_roles.getDefaultRole();
    auto record = m_database.loginOrRegister(parsed->growId, parsed->password,
                                             fallback != nullptr ? fallback->id : 4);
    if (!record.has_value())
    {
        auto& entry = m_loginFailures[parsed->growId];
        if (now - entry.windowStart >= std::chrono::seconds(60))
        {
            entry.windowStart = now;
            entry.count = 1;
        }
        else
        {
            ++entry.count;
        }
        sendAction(session.peer, "logon_fail", "");
        return;
    }
    m_loginFailures.erase(parsed->growId);

    session.playerId = record->id;
    session.growId = record->growId;
    session.roleId = record->roleId;
    session.muted = record->muted;
    session.gems = record->gems;
    session.authenticated = true;
    m_database.loadInventory(session.playerId, session.inventory);
    if (record->isNew)
        grantStarterKit(session);

    // Remember this login across the client's reconnect (it drops the
    // connection after OnSendToServer and returns with tankIDName). The
    // pending entry is bound to the client IP so another host cannot resume
    // it by only knowing the growId.
    PendingAuth pending;
    pending.playerId = record->id;
    pending.roleId = record->roleId;
    pending.muted = record->muted;
    pending.isNew = record->isNew;
    pending.gems = record->gems;
    pending.ip = peerIpv4(session.peer);
    pending.expires = now + std::chrono::seconds(120);
    m_pending[record->growId] = pending;

    sendVariant(session.peer, {VariantValue::makeString("OnSendToServer"),
                               VariantValue::makeInt(m_config.bindPort), VariantValue::makeInt(0),
                               VariantValue::makeInt(static_cast<int>(session.playerId)),
                               VariantValue::makeString(m_config.publicHost + "|0|0"),
                               VariantValue::makeInt(1),
                               VariantValue::makeString(session.growId)});
    sendConsoleMessage(session.peer, "Welcome, " + session.growId + ".");
    logInfo("Authenticated " + session.growId);
}

void GameServer::handleLoginIdentity(Session& session, const TextPacket& packet)
{
    std::string name = lineValue(packet.lines, "tankIDName");
    if (name.empty())
        name = lineValue(packet.lines, "requestedName");
    std::string country = lineValue(packet.lines, "country");
    if (!country.empty())
    {
        if (country.size() > 32)
            country.resize(32);
        session.country = country;
    }

    if (!session.authenticated)
    {
        // Reconnecting client: resume the login accepted during protocol.
        // Modern clients (proto 226+) identify themselves with the "user" id
        // echoed from OnSendToServer and may send an EMPTY requestedName —
        // so resolve the pending login by name, then by user id, then by
        // (single) pending login from the same IP.
        std::string user = lineValue(packet.lines, "user");
        PendingAuth* pending = nullptr;
        if (!name.empty())
        {
            auto it = m_pending.find(name);
            if (it != m_pending.end())
                pending = &it->second;
        }
        if (pending == nullptr && !user.empty())
        {
            uint32_t userId = 0;
            if (auto [ptr, ec] = std::from_chars(user.data(), user.data() + user.size(), userId);
                ec == std::errc() && ptr == user.data() + user.size())
            {
                for (auto& [key, entry] : m_pending)
                {
                    if (entry.playerId == userId)
                    {
                        pending = &entry;
                        break;
                    }
                }
            }
        }
        if (pending == nullptr)
        {
            // Fallback: exactly one fresh pending login from this IP.
            PendingAuth* candidate = nullptr;
            for (auto& [key, entry] : m_pending)
            {
                if (entry.ip == peerIpv4(session.peer) && entry.expires >= std::chrono::steady_clock::now())
                {
                    if (candidate != nullptr)
                    {
                        candidate = nullptr; // Ambiguous — do not guess.
                        break;
                    }
                    candidate = &entry;
                }
            }
            pending = candidate;
        }

        if (pending == nullptr || pending->expires < std::chrono::steady_clock::now() ||
            pending->ip != peerIpv4(session.peer))
        {
            sendAction(session.peer, "logon_fail", "");
            logWarn("Identity resume failed (name='" + name + "', user='" + user + "')");
            return;
        }
        PendingAuth resolved = *pending;
        // Remove the matched pending entry (find by playerId+ip).
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it)
        {
            if (it->second.playerId == resolved.playerId && it->second.ip == resolved.ip)
            {
                m_pending.erase(it);
                break;
            }
        }
        session.playerId = resolved.playerId;
        session.growId = !name.empty() ? name : session.growId;
        if (session.growId.empty())
        {
            // Name was empty: recover the growId from the players table.
            auto recordId = resolved.playerId;
            auto grow = m_database.findPlayerById(recordId);
            session.growId = grow.has_value() ? grow.value() : std::to_string(recordId);
        }
        session.roleId = resolved.roleId;
        session.muted = resolved.muted;
        session.gems = resolved.gems;
        session.authenticated = true;
        m_database.loadInventory(session.playerId, session.inventory);
        if (resolved.isNew)
            grantStarterKit(session);
    }
    else
    {
        // Already authenticated on this connection: finish the pending login
        // and never let the client rename itself (that would enable
        // impersonation in chat and spawn data).
        m_pending.erase(session.growId);
        if (!name.empty() && name != session.growId)
        {
            sendConsoleMessage(session.peer, "Identity mismatch; keeping account " + session.growId + ".");
        }
    }

    // Real clients expect the GDPR override before the super-main payload
    // (mirrors Gurotopia/Vallen: 18, 1, 0, 1 as int32 args).
    sendVariant(session.peer, {VariantValue::makeString("OnOverrideGDPRFromServer"),
                               VariantValue::makeInt(18), VariantValue::makeInt(1),
                               VariantValue::makeInt(0), VariantValue::makeInt(1)});
    sendSuperMain(session.peer);
    // NOTE: the items.dat is NOT pushed here. Real clients use their cached
    // copy when the advertised hash matches and ask for an update via
    // action|refresh_item_data otherwise (answered in handleTextPacket).
    sendInventoryState(session);
    logInfo("Login completed for " + session.growId + " (supermain + inventory sent)");
    sendConsoleMessage(session.peer, "Identity confirmed. Use /warp <world> or join a world door.");
}

void GameServer::grantStarterKit(Session& session)
{
    static const std::pair<int, int> kStarter[] = {{18, 1}, {32, 1}, {242, 1}};
    session.inventory.clear();
    for (const auto& [itemId, count] : kStarter)
    {
        session.inventory.emplace_back(itemId, count);
        m_database.setInventoryItem(session.playerId, itemId, count);
    }
}

void GameServer::sendInventoryState(Session& session)
{
    std::vector<uint8_t> body;
    body.push_back(0x01); // Extended inventory flag.

    auto appendI16 = [](std::vector<uint8_t>& out, int16_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    };
    auto appendI32 = [](std::vector<uint8_t>& out, int32_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    };

    appendI32(body, 16); // Slot capacity.
    std::size_t shown = session.inventory.size() > 200 ? 200 : session.inventory.size();
    appendI16(body, static_cast<int16_t>(shown));
    for (std::size_t i = 0; i < shown; ++i)
    {
        int id = session.inventory[i].first;
        int count = session.inventory[i].second;
        if (id < 0)
            id = 0;
        if (id > 32767)
            id = 32767;
        if (count < 0)
            count = 0;
        if (count > 32767)
            count = 32767;
        appendI16(body, static_cast<int16_t>(id));
        appendI16(body, static_cast<int16_t>(count));
    }

    TankHeader header;
    header.type = 0x09; // Inventory state.
    header.netId = session.netId;
    header.state = 8; // Extended flag.
    header.dataSize = static_cast<uint32_t>(body.size());

    std::vector<uint8_t> data = encodeTankHeader(header);
    data.insert(data.end(), body.begin(), body.end());

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(session.peer, 0, packet) != 0)
        enet_packet_destroy(packet);
}

void GameServer::broadcastChat(Session& session, const std::string& text)
{
    if (session.muted)
    {
        sendConsoleMessage(session.peer, "You are muted.");
        return;
    }
    if (session.worldName.empty())
    {
        sendConsoleMessage(session.peer, "Join a world first: /warp <world>.");
        return;
    }
    if (text.size() > 256)
    {
        sendConsoleMessage(session.peer, "Message too long (256 characters max).");
        return;
    }

    World& world = m_worlds.getOrCreate(session.worldName);
    std::string bubble = "CP:0_PL:0_OID:_player_chat=" + text;
    std::string console = "[W] <" + session.growId + "> " + text;

    for (auto& [peer, candidate] : m_sessions)
    {
        if (candidate.worldName != world.name)
            continue;
        sendVariant(peer, {VariantValue::makeString("OnTalkBubble"), VariantValue::makeInt(session.netId),
                           VariantValue::makeString(bubble), VariantValue::makeUInt(0)});
        sendConsoleMessage(peer, console);
    }
}

void GameServer::sendSuperMain(ENetPeer* peer)
{
    // Argument layout mirrors what real 5.5x clients expect (proven by the
    // Gurotopia/Vallen references): items hash, CDN host, CDN cache folder,
    // cheat-engine blacklist, then the big meta config string.
    sendVariant(peer, {VariantValue::makeString("OnSuperMainStartAcceptLogonHrdxs47254722215a"),
                       VariantValue::makeUInt(m_itemsHash),
                       VariantValue::makeString("ubistatic-a.akamaihd.net"),
                       VariantValue::makeString("0098/150726456789/cache/"),
                       VariantValue::makeString("cc.cz.madkite.freedom org.aqua.gg idv.aqua.bulldog "
                                                "com.cih.gamecih2 com.cih.gamecih com.cih.game_cih "
                                                "cn.maocai.gamekiller com.gmd.speedtime org.dax.attack "
                                                "com.x0.strai.frep com.x0.strai.free org.cheatengine.cegui "
                                                "org.sbtools.gamehack com.skgames.traffikrider "
                                                "org.sbtoods.gamehaca com.skype.ralder org.cheatengine.cegui.xx.multi1458919170111 "
                                                "com.prohiro.macro me.autotouch.autotouch com.cygery.repetitouch.free "
                                                "com.cygery.repetitouch.pro com.proziro.zacro com.slash.gamebuster"),
                       VariantValue::makeString(
                           "proto=225|choosemusic=audio/mp3/about_theme.mp3|active_holiday=0|wing_week_day=0|"
                           "ubi_week_day=0|server_tick=0|game_theme=growtopia|clash_active=0|"
                           "drop_lavacheck_faster=1|isPayingUser=1|usingStoreNavigation=1|enableInventoryTab=1|"
                           "bigBackpack=1|seed_diary_hash=4266294761|m_clientBits=|eventButtons={\"EventButtonData\":"
                           "[{\"active\":false,\"buttonAction\":\"eventmenu\",\"buttonTemplate\":\"BaseEventButton\","
                           "\"counter\":0,\"counterMax\":0,\"itemIdIcon\":6244,\"name\":\"ClashEventButton\",\"order\":9,"
                           "\"rcssClass\":\"clash-event\",\"text\":\"\"},{\"active\":false,\"buttonAction\":"
                           "\"dailychallengemenu\",\"buttonTemplate\":\"BaseEventButton\",\"counter\":0,\"counterMax\":0,"
                           "\"itemIdIcon\":23,\"name\":\"DailyChallenge\",\"order\":10,\"rcssClass\":\"daily_challenge\","
                           "\"text\":\"\"},{\"active\":false,\"buttonAction\":\"openPiggyBank\",\"buttonTemplate\":"
                           "\"BaseEventButton\",\"counter\":0,\"counterMax\":0,\"name\":\"PiggyBankButton\",\"order\":20,"
                           "\"rcssClass\":\"\",\"text\":\"\"}]}")});
}

void GameServer::sendItemDatabase(ENetPeer* peer)
{
    TankHeader header;
    header.type = 0x10; // Item database payload.
    header.state = 8; // Extended flag.
    header.dataSize = static_cast<uint32_t>(m_itemsDat.size());

    std::vector<uint8_t> data = encodeTankHeader(header);
    data.insert(data.end(), m_itemsDat.begin(), m_itemsDat.end());

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
}

void GameServer::handleEnterGame(Session& session)
{
    sendInventoryState(session);
    sendVariant(session.peer, {VariantValue::makeString("OnSetBux"), VariantValue::makeInt(session.gems),
                               VariantValue::makeInt(1), VariantValue::makeInt(1)});
    sendVariant(session.peer, {VariantValue::makeString("SetHasGrowID"), VariantValue::makeInt(1),
                               VariantValue::makeString(session.growId), VariantValue::makeString("")});

    std::string popular;
    for (const auto& [name, visitors] : m_worlds.listWorlds())
        popular += "add_floater|" + name + "|" + std::to_string(visitors) + "|0.5|3529161471\n";
    sendVariant(session.peer, {VariantValue::makeString("OnRequestWorldSelectMenu"),
                               VariantValue::makeString("add_filter|\nadd_heading|Top Worlds<ROW2>|\n" +
                                                        popular),
                               VariantValue::makeInt(1)});

    std::size_t online = 0;
    for (const auto& [peer, candidate] : m_sessions)
    {
        (void)peer;
        if (candidate.authenticated)
            ++online;
    }
    sendConsoleMessage(session.peer,
                       "Where would you like to go? (" + std::to_string(online) + " online)");

    std::string gazette = "set_default_color|`o\nadd_label_with_icon|big|`wWildanDev Gazette``|left|5016|\n"
                          "add_spacer|small|\nadd_textbox|`wWelcome to WildanDev GTPS``|left|\n"
                          "add_spacer|small|\nadd_quick_exit|\nend_dialog|gazette|||";
    sendVariant(session.peer, {VariantValue::makeString("OnDialogRequest"), VariantValue::makeString(gazette)});

    TankHeader ping;
    ping.type = 0x16; // Ping request.
    std::vector<uint8_t> data = encodeTankHeader(ping);
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(session.peer, 0, packet) != 0)
        enet_packet_destroy(packet);

    sendVariant(session.peer, {VariantValue::makeString("OnSetFeatureEnableFlags"),
                               VariantValue::makeString("EA8DEAcGAgEOBQgKCQ0MEQQ=")});
    logInfo(session.growId + " entered the game menu");
}

void GameServer::sendMapData(ENetPeer* peer, const World& world)
{
    std::vector<uint8_t> body = serializeWorld(world);

    TankHeader header;
    header.type = kPacketMapData;
    header.state = 8; // Extended flag.
    header.dataSize = static_cast<uint32_t>(body.size());

    std::vector<uint8_t> data = encodeTankHeader(header);
    data.insert(data.end(), body.begin(), body.end());

    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
}

void GameServer::sendSpawn(ENetPeer* peer, const Session& subject, bool local)
{
    const Role* role = m_roles.getRole(subject.roleId);
    int rank = role != nullptr ? role->rank : 0;

    int tileX = static_cast<int>(subject.posX / 32.0f);
    int tileY = static_cast<int>(subject.posY / 32.0f);

    std::string text = "spawn|avatar\nnetID|" + std::to_string(subject.netId) + "\nuserID|" +
                       std::to_string(subject.playerId) + "\ncolrect|0|0|20|30\nposXY|" +
                       std::to_string(tileX) + "|" + std::to_string(tileY) + "\nname|" + subject.growId +
                       "``\ncountry|" + subject.country + "\ninvis|" + (subject.ghost ? "1" : "0") +
                       "\nmstate|" + (rank >= 50 ? "1" : "0") + "\nsmstate|" + (rank >= 80 ? "1" : "0") +
                       "\nonlineID|\n";
    if (local)
        text += "type|local\n";

    sendVariant(peer, {VariantValue::makeString("OnSpawn"), VariantValue::makeString(text)});
}

void GameServer::broadcastToWorld(const World& world, const std::vector<uint8_t>& raw, ENetPeer* except)
{
    for (auto& [peer, candidate] : m_sessions)
    {
        if (peer == except || candidate.worldName != world.name)
            continue;
        ENetPacket* packet = enet_packet_create(raw.data(), raw.size(), ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(peer, 0, packet) != 0)
            enet_packet_destroy(packet);
    }
}

bool GameServer::takeItem(Session& session, int itemId, int count)
{
    if (count <= 0)
        return false;
    for (auto it = session.inventory.begin(); it != session.inventory.end(); ++it)
    {
        if (it->first != itemId)
            continue;
        if (it->second < count)
            return false;
        it->second -= count;
        int remaining = it->second;
        if (remaining == 0)
            session.inventory.erase(it);
        m_database.setInventoryItem(session.playerId, itemId, remaining);
        sendInventoryDelta(session, itemId, -count);
        return true;
    }
    return false;
}

void GameServer::sendInventoryDelta(const Session& session, int itemId, int delta)
{
    if (delta == 0)
        return;
    uint32_t amount = static_cast<uint32_t>(delta < 0 ? -delta : delta);
    TankHeader header;
    header.type = delta > 0 ? static_cast<int32_t>((amount << 24) | 0x0D)
                            : static_cast<int32_t>((amount << 16) | 0x0D);
    header.netId = session.netId;
    header.id = itemId;

    std::vector<uint8_t> data = encodeTankHeader(header);
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(session.peer, 0, packet) != 0)
        enet_packet_destroy(packet);
}

void GameServer::giveItem(Session& session, int itemId, int count)
{
    if (count <= 0)
        return;
    bool found = false;
    for (auto& slot : session.inventory)
    {
        if (slot.first == itemId)
        {
            slot.second = std::min(200, slot.second + count);
            m_database.setInventoryItem(session.playerId, itemId, slot.second);
            found = true;
            break;
        }
    }
    if (!found)
    {
        session.inventory.emplace_back(itemId, count);
        m_database.setInventoryItem(session.playerId, itemId, count);
    }
    sendInventoryDelta(session, itemId, count);
}

void GameServer::respawnPlayer(Session& session)
{
    if (session.worldName.empty())
        return;
    World& world = m_worlds.getOrCreate(session.worldName);
    float restX = static_cast<float>(world.spawnTileX * 32);
    float restY = static_cast<float>(world.spawnTileY * 32);
    session.posX = restX;
    session.posY = restY;
    session.hp = 10;

    // Death sequence, mirroring real Growtopia: freeze, kill, then teleport
    // back to the main door 1.9s later and unfreeze.
    sendVariant(session.peer, {VariantValue::makeString("OnSetFreezeState"), VariantValue::makeInt(2)},
                session.netId);
    sendVariant(session.peer, {VariantValue::makeString("OnKilled")}, session.netId);
    sendVariant(session.peer,
                {VariantValue::makeString("OnSetPos"), VariantValue::makeVec2(restX, restY)}, session.netId,
                1900);
    sendVariant(session.peer, {VariantValue::makeString("OnSetFreezeState")}, session.netId, 1900);
}

void GameServer::leaveWorld(Session& session)
{
    if (session.worldName.empty())
        return;
    World& world = m_worlds.getOrCreate(session.worldName);
    if (world.visitorCount > 0)
        --world.visitorCount;
    sendVariant(session.peer, {VariantValue::makeString("OnRemove"), VariantValue::makeInt(session.netId)});
    for (auto& [peer, other] : m_sessions)
    {
        if (peer == session.peer || other.worldName != world.name)
            continue;
        sendVariant(peer, {VariantValue::makeString("OnRemove"), VariantValue::makeInt(session.netId)});
    }
    session.worldName.clear();
    session.netId = 0;
    sendConsoleMessage(session.peer, "Where would you like to go? Use /warp <world>.");
}

void GameServer::sendObjectState(const World& world, int32_t marker, uint32_t uid, int itemId, int count,
                                 float x, float y)
{
    TankHeader header;
    header.type = 0x0E; // PACKET_ITEM_CHANGE_OBJECT
    header.netId = marker; // 0xFFFFFFFF new object, 0xFFFFFFFD merged, player netId = removed.
    header.uid = uid;
    header.count = static_cast<float>(count);
    header.id = itemId;
    header.posX = x;
    header.posY = y;

    std::vector<uint8_t> data = encodeTankHeader(header);
    for (auto& [peer, candidate] : m_sessions)
    {
        if (candidate.worldName != world.name)
            continue;
        ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(peer, 0, packet) != 0)
            enet_packet_destroy(packet);
    }
}

void GameServer::dropObject(World& world, int itemId, int count, int tileX, int tileY)
{
    if (count <= 0)
        return;
    static std::mt19937 rng{std::random_device{}()};
    float x = static_cast<float>(tileX * 32 + static_cast<int>(rng() % 17));
    float y = static_cast<float>(tileY * 32 + static_cast<int>(rng() % 17));

    // Merge with an identical object on the same tile (up to 200).
    for (auto& object : world.objects)
    {
        if (object.id == itemId && static_cast<int>(object.x) / 32 == tileX &&
            static_cast<int>(object.y) / 32 == tileY && object.count < 200)
        {
            object.count = std::min(200, object.count + count);
            world.dirty = true;
            sendObjectState(world, static_cast<int32_t>(0xFFFFFFFD), object.uid, itemId, object.count,
                            object.x, object.y);
            return;
        }
    }

    WorldObject object;
    object.id = itemId;
    object.count = count;
    object.x = x;
    object.y = y;
    object.uid = ++world.lastObjectId;
    world.objects.push_back(object);
    world.dirty = true;
    sendObjectState(world, static_cast<int32_t>(0xFFFFFFFF), object.uid, itemId, count, x, y);
}

void GameServer::removeDropObject(World& world, Session& collector, uint32_t uid)
{
    for (auto it = world.objects.begin(); it != world.objects.end(); ++it)
    {
        if (it->uid != uid)
            continue;
        sendObjectState(world, collector.netId, uid, it->id, 0, it->x, it->y);
        world.objects.erase(it);
        world.dirty = true;
        return;
    }
}

bool GameServer::joinWorld(Session& session, const std::string& worldName)
{
    if (worldName.empty() || worldName.size() > 24)
    {
        sendConsoleMessage(session.peer, "Invalid world name.");
        return false;
    }
    for (char c : worldName)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
        {
            sendConsoleMessage(session.peer, "World names allow letters, digits and underscore only.");
            return false;
        }
    }

    World& world = m_worlds.getOrCreate(worldName);
    if (!session.worldName.empty() && session.worldName == world.name)
        return true;

    if (!session.worldName.empty())
    {
        World& previous = m_worlds.getOrCreate(session.worldName);
        if (previous.visitorCount > 0)
            --previous.visitorCount;
        sendVariant(session.peer, {VariantValue::makeString("OnRemove"),
                                   VariantValue::makeInt(session.netId)});
        session.worldName.clear();
        session.netId = 0;
    }

    session.worldName = world.name;
    session.netId = ++world.nextNetId;
    ++world.visitorCount;
    session.posX = static_cast<float>(world.spawnTileX * 32);
    session.posY = static_cast<float>(world.spawnTileY * 32);

    sendMapData(session.peer, world);

    for (auto& [peer, other] : m_sessions)
    {
        if (peer == session.peer || other.worldName != world.name)
            continue;
        if (!other.ghost)
            sendSpawn(session.peer, other, false);
        // Ghosted players still receive spawns; they are just not shown.
        sendSpawn(peer, session, false);
    }
    sendSpawn(session.peer, session, true);
    sendVariant(session.peer,
                {VariantValue::makeString("OnSetPos"),
                 VariantValue::makeVec2(static_cast<float>(world.spawnTileX * 32),
                                        static_cast<float>(world.spawnTileY * 32))},
                session.netId);

    sendConsoleMessage(session.peer, "World " + world.name + " entered.");
    logInfo(session.growId + " entered world " + world.name);
    return true;
}

void GameServer::handleTankPacket(Session& session, const uint8_t* data, std::size_t length)
{
    int32_t type = readInt32(data, 4);
    if (!session.authenticated || session.worldName.empty())
    {
        // Movement/collect from unauthenticated or world-less sessions is
        // ignored; logging is limited to avoid log spam.
        return;
    }

    World& world = m_worlds.getOrCreate(session.worldName);

    auto relayPatched = [&](int32_t newType, bool includeSelf) {
        std::vector<uint8_t> raw(data, data + length);
        if (raw.size() >= 16)
        {
            auto writeI32 = [&raw](std::size_t offset, int32_t value) {
                raw[offset] = static_cast<uint8_t>(value & 0xFF);
                raw[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
                raw[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
                raw[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
            };
            writeI32(4, newType);
            writeI32(8, session.netId);
            writeI32(12, static_cast<int32_t>(session.playerId));
        }
        broadcastToWorld(world, raw, includeSelf ? nullptr : session.peer);
    };

    if (type == kPacketState)
    {
        session.posX = readFloat(data, 28);
        session.posY = readFloat(data, 32);
        int32_t state = readInt32(data, 16);

        // Ghosted players are not shown moving around the world.
        if (!session.ghost)
            relayPatched(type, false);

        // Walking into lava drains health; at 0 the player dies and
        // respawns at the main door.
        if (state & 0x40)
        {
            session.hp -= 2;
            if (session.hp <= 0)
                respawnPlayer(session);
        }
        return;
    }

    if (type == 0x0B)
    {
        // Object collect: the uid travels in the id field.
        uint32_t uid = static_cast<uint32_t>(readInt32(data, 24));
        for (auto it = world.objects.begin(); it != world.objects.end(); ++it)
        {
            if (it->uid != uid)
                continue;
            WorldObject object = *it;
            world.objects.erase(it);
            world.dirty = true;
            if (object.id == kGemsItemId)
            {
                session.gems += object.count;
                sendVariant(session.peer,
                            {VariantValue::makeString("OnSetBux"), VariantValue::makeInt(session.gems),
                             VariantValue::makeInt(1), VariantValue::makeInt(1)});
            }
            else
            {
                int space = 200;
                bool exists = false;
                for (const auto& slot : session.inventory)
                {
                    if (slot.first == object.id)
                    {
                        space = 200 - slot.second;
                        exists = true;
                        break;
                    }
                }
                if (exists && space <= 0)
                {
                    // Inventory full: leave the object on the ground.
                    world.objects.push_back(object);
                    sendConsoleMessage(session.peer, "Your inventory is full.");
                    return;
                }
                int collected = std::min(space, object.count);
                giveItem(session, object.id, collected);
                const ItemDef* def = findItemById(object.id);
                std::string name = def != nullptr ? def->name : std::to_string(object.id);
                sendConsoleMessage(session.peer,
                                   "Collected `w" + std::to_string(collected) + " " + name + "``.");
                if (collected < object.count)
                {
                    // Inventory filled up: keep the remainder on the ground.
                    WorldObject rest = object;
                    rest.count = object.count - collected;
                    rest.uid = ++world.lastObjectId;
                    world.objects.push_back(rest);
                    sendObjectState(world, static_cast<int32_t>(0xFFFFFFFF), rest.uid, rest.id, rest.count,
                                    rest.x, rest.y);
                }
            }
            // Removal is broadcast to the whole world.
            sendObjectState(world, session.netId, uid, object.id, 0, object.x, object.y);
            return;
        }
        return;
    }

    if (type == 0x07)
    {
        // Tile activate: walking into the main door exits the world.
        int punchX = readInt32(data, 48);
        int punchY = readInt32(data, 52);
        if (world.inside(punchX, punchY) && world.at(punchX, punchY).fg == kMainDoorItemId)
        {
            leaveWorld(session);
        }
        return;
    }

    if (type == kPacketTileChange)
    {
        int32_t heldId = readInt32(data, 24);
        int punchX = readInt32(data, 48);
        int punchY = readInt32(data, 52);
        if (!world.inside(punchX, punchY))
            return;

        // Punch reach: reject editing tiles far away from the avatar.
        int playerTileX = static_cast<int>(session.posX / 32.0f);
        int playerTileY = static_cast<int>(session.posY / 32.0f);
        if (std::abs(punchX - playerTileX) > 4 || std::abs(punchY - playerTileY) > 4)
        {
            sendConsoleMessage(session.peer, "Too far away.");
            return;
        }

        const Role* role = m_roles.getRole(session.roleId);
        bool staff = role != nullptr && role->hasPermission("world.bypass_lock");
        if (world.ownerId != 0 && session.playerId != static_cast<uint32_t>(world.ownerId) && !staff)
        {
            sendConsoleMessage(session.peer, "This world is locked.");
            return;
        }

        Tile& tile = world.at(punchX, punchY);

        if (heldId == kFistItemId)
        {
            // Punching the main door respawns the player at it (real GT
            // behaviour). Punching bedrock does nothing.
            if (tile.fg == kMainDoorItemId)
            {
                respawnPlayer(session);
                return;
            }
            if (tile.fg == 0)
                return;
            const ItemDef* target = findItemById(tile.fg);
            if (target != nullptr && target->hits == 0)
                return; // unbreakable (bedrock, doors...)

            // Track punch damage in memory (resets after 10 idle seconds).
            long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
            int tileIndex = punchY * kWorldWidth + punchX;
            auto dmg = world.damage.find(tileIndex);
            if (dmg == world.damage.end())
                dmg = world.damage.emplace(tileIndex, std::make_pair(0, nowMs)).first;
            if (nowMs - dmg->second.second > 10000)
                dmg->second.first = 0;
            dmg->second.second = nowMs;
            dmg->second.first += 1;
            int hits = dmg->second.first;
            int needed = target != nullptr ? target->hits : 4;

            // Broadcast the visual damage: (damage << 24) | 0x08.
            relayPatched(static_cast<int32_t>((hits << 24) | 0x08), true);

            if (hits < needed)
                return;
            world.damage.erase(dmg);
            int16_t broken = tile.fg;
            tile.fg = 0;
            world.dirty = true;

            // Real GT drops: gems always (small amounts), block and seed by
            // rarity-based chance.
            int rarity = target != nullptr ? target->rarity : 1;
            int blockChance = rarity > 1 ? 4 : 8;
            int seedChance = rarity > 1 ? 2 : 4;
            static std::mt19937 dropRng{std::random_device{}()};
            if (dropRng() % 20 == 0)
                dropObject(world, kGemsItemId, 10, punchX, punchY);
            else if (dropRng() % 4 == 0)
                dropObject(world, kGemsItemId, 5, punchX, punchY);
            else
                dropObject(world, kGemsItemId, 1, punchX, punchY);
            if (static_cast<int>(dropRng() % blockChance) == 0)
                dropObject(world, broken, 1, punchX, punchY);
            if (broken > 0 && static_cast<int>(dropRng() % seedChance) == 0)
                dropObject(world, broken + 1, 1, punchX, punchY);
            return;
        }

        // Placing.
        const ItemDef* item = heldId > 0 && heldId <= 20000 ? findItemById(heldId) : nullptr;
        if (tile.fg != 0 || item == nullptr)
            return;
        // Placing requires actually holding the item; locks are not consumed.
        if (item->type != ItemType::Lock && !takeItem(session, heldId, 1))
        {
            sendConsoleMessage(session.peer, "You do not have that item.");
            return;
        }
        tile.fg = static_cast<int16_t>(heldId);
        if (item->type == ItemType::Lock && world.ownerId == 0)
        {
            world.ownerId = static_cast<int>(session.playerId);
            sendConsoleMessage(session.peer, "World locked by you.");
        }
        world.dirty = true;
        relayPatched(type, true);
        return;
    }
}

} // namespace WildanDev
