// WildanDev GTPS integration test client (own test harness).
//
// Speaks the same ENet dialect as the game server, runs the full login
// pipeline and asserts on every server reply:
//   1. connect -> server hello packet
//   2. "protocol" + ltoken -> account created/authenticated, welcome variant
//   3. "tankIDName" identity -> confirmation message
//   4. "action|input" /who -> online counter reply
// Exit code is 0 only when every assertion passes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <enet/enet.h>

namespace
{

uint16_t testPort()
{
    const char* env = std::getenv("WILDANDEV_TEST_PORT");
    if (env == nullptr || env[0] == '\0')
        return 17091;
    return static_cast<uint16_t>(std::atoi(env));
}

void sendText(ENetPeer* peer, const std::string& body)
{
    std::vector<uint8_t> data(4 + body.size() + 1, 0x00);
    data[0] = 2;
    std::memcpy(data.data() + 4, body.data(), body.size());
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
    enet_host_flush(peer->host);
}

bool g_sawTankType[32] = {};

std::string base64Encode(const std::string& input)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < input.size(); i += 3)
    {
        uint32_t group = static_cast<uint8_t>(input[i]) << 16;
        int padding = 0;
        if (i + 1 < input.size())
            group |= static_cast<uint8_t>(input[i + 1]) << 8;
        else
            ++padding;
        if (i + 2 < input.size())
            group |= static_cast<uint8_t>(input[i + 2]);
        else
            ++padding;
        out.push_back(alphabet[(group >> 18) & 0x3F]);
        out.push_back(alphabet[(group >> 12) & 0x3F]);
        out.push_back(padding >= 2 ? '=' : alphabet[(group >> 6) & 0x3F]);
        out.push_back(padding >= 1 ? '=' : alphabet[group & 0x3F]);
    }
    return out;
}

bool waitFor(ENetHost* host, ENetPeer* peer, const std::string& needle, int timeoutMs, bool* connected,
             bool* sawHello = nullptr)
{
    (void)peer;
    ENetEvent event{};
    int waited = 0;
    while (waited < timeoutMs)
    {
        while (enet_host_service(host, &event, 100) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_CONNECT && connected != nullptr)
                *connected = true;
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                if (sawHello != nullptr && event.packet->dataLength >= 4 && event.packet->data[0] == 1)
                    *sawHello = true;
                if (event.packet->dataLength >= 60 && event.packet->data[0] == 4)
                {
                    int32_t tankType = 0;
                    std::memcpy(&tankType, event.packet->data + 4, 4);
                    if (tankType >= 0 && tankType < 32)
                        g_sawTankType[tankType] = true;
                }
                std::string payload(reinterpret_cast<char*>(event.packet->data), event.packet->dataLength);
                if (payload.find(needle) != std::string::npos)
                {
                    enet_packet_destroy(event.packet);
                    return true;
                }
                enet_packet_destroy(event.packet);
            }
            if (event.type == ENET_EVENT_TYPE_DISCONNECT && connected != nullptr)
                *connected = false;
        }
        waited += 100;
    }
    return false;
}

int g_failures = 0;

void check(bool condition, const char* name)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
        ++g_failures;
}

void finish(ENetHost* host)
{
    if (host != nullptr)
        enet_host_destroy(host);
    enet_deinitialize();
    if (g_failures == 0)
        std::printf("ALL TESTS PASSED\n");
    else
        std::printf("FAILURES: %d\n", g_failures);
}

bool waitForTank(ENetHost* host, int wantedType, int timeoutMs)
{
    ENetEvent event{};
    int waited = 0;
    while (waited < timeoutMs)
    {
        while (enet_host_service(host, &event, 100) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet->dataLength >= 60 &&
                event.packet->data[0] == 4)
            {
                int32_t type = 0;
                std::memcpy(&type, event.packet->data + 4, 4);
                if (type == wantedType)
                {
                    enet_packet_destroy(event.packet);
                    return true;
                }
            }
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
        }
        waited += 100;
    }
    return false;
}

bool waitForTankMask(ENetHost* host, int32_t mask, int32_t wanted, int timeoutMs)
{
    ENetEvent event{};
    int waited = 0;
    while (waited < timeoutMs)
    {
        while (enet_host_service(host, &event, 100) > 0)
        {
            if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet->dataLength >= 60 &&
                event.packet->data[0] == 4)
            {
                int32_t type = 0;
                std::memcpy(&type, event.packet->data + 4, 4);
                if ((type & mask) == wanted)
                {
                    enet_packet_destroy(event.packet);
                    return true;
                }
            }
            if (event.type == ENET_EVENT_TYPE_RECEIVE)
                enet_packet_destroy(event.packet);
        }
        waited += 100;
    }
    return false;
}

void sendTank(ENetPeer* peer, int32_t type, int32_t heldId, int32_t punchX, int32_t punchY)
{
    std::vector<uint8_t> data(60, 0);
    auto put32 = [&](std::size_t off, int32_t value) {
        data[off] = static_cast<uint8_t>(value & 0xFF);
        data[off + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        data[off + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        data[off + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    };
    data[0] = 4;
    put32(4, type);
    put32(24, heldId);
    put32(48, punchX);
    put32(52, punchY);
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
    enet_host_flush(peer->host);
}

void sendMovement(ENetPeer* peer, float posX, float posY)
{
    std::vector<uint8_t> data(60, 0);
    auto put32 = [&](std::size_t off, int32_t value) {
        data[off] = static_cast<uint8_t>(value & 0xFF);
        data[off + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
        data[off + 2] = static_cast<uint8_t>((value >> 16) & 0xFF);
        data[off + 3] = static_cast<uint8_t>((value >> 24) & 0xFF);
    };
    data[0] = 4;
    put32(4, 0); // Movement/state packet.
    put32(8, 1); // netId (server rewrites it, but keep it plausible).
    float fpos[2] = {posX, posY};
    std::memcpy(data.data() + 28, fpos, sizeof(fpos));
    ENetPacket* packet = enet_packet_create(data.data(), data.size(), ENET_PACKET_FLAG_RELIABLE);
    if (enet_peer_send(peer, 0, packet) != 0)
        enet_packet_destroy(packet);
    enet_host_flush(peer->host);
}

} // namespace

int main(int argc, char** argv)
{
    std::string growId = argc > 1 ? argv[1] : "TestUser";
    std::string password = argc > 2 ? argv[2] : "secret123";
    std::string mode = argc > 3 ? argv[3] : "player"; // player | admin | badpass
    std::string token = base64Encode("_token=integration&growId=" + growId + "&password=" + password);
    if (enet_initialize() != 0)
    {
        std::printf("[FAIL] enet_initialize\n");
        return 1;
    }

    ENetHost* host = enet_host_create(ENET_ADDRESS_TYPE_IPV4, nullptr, 8, 2, 0, 0);
    if (host == nullptr)
    {
        std::printf("[FAIL] enet_host_create\n");
        enet_deinitialize();
        return 1;
    }
    host->usingNewPacket = 1;
    host->checksum = enet_crc32;
    enet_host_compress_with_range_coder(host);

    ENetAddress address{};
    address.type = ENET_ADDRESS_TYPE_IPV4;
    address.port = testPort();
    enet_address_set_host(&address, ENET_ADDRESS_TYPE_IPV4, "127.0.0.1");

    ENetPeer* peer = enet_host_connect(host, &address, 2, 0);
    if (peer == nullptr)
    {
        std::printf("[FAIL] enet_host_connect\n");
        finish(host);
        return 1;
    }

    bool connected = false;
    bool sawHello = false;
    check(waitFor(host, peer, "", 3000, &connected, &sawHello) || connected, "connect to game server");

    // Step 1: hello packet carries int32(1). It arrives together with connect,
    // so it is observed during the connect wait above.
    check(sawHello, "server hello received");

    // Step 2: protocol login (skipped in orphan mode on purpose).
    if (mode != "orphan")
        sendText(peer, "protocol|225\nltoken|" + token + "\nplatformID|0,1,1");
    if (mode == "badpass")
    {
        check(waitFor(host, peer, "logon_fail", 3000, nullptr), "bad password rejected");
        finish(host);
        return g_failures == 0 ? 0 : 1;
    }
    if (mode == "orphan")
    {
        // tankIDName with no prior protocol must be rejected.
        sendText(peer, "tankIDName|" + growId + "\ncountry|ID");
        check(waitFor(host, peer, "logon_fail", 3000, nullptr), "orphan identity rejected");
        finish(host);
        return g_failures == 0 ? 0 : 1;
    }
    if (mode == "reconnect")
    {
        // Real clients drop the connection after OnSendToServer and return
        // with tankIDName only; the pending login must resume.
        check(waitFor(host, peer, "Welcome, " + growId + ".", 3000, nullptr), "protocol login accepted");
        enet_peer_disconnect(peer, 0);
        enet_host_flush(host);

        ENetPeer* peer2 = enet_host_connect(host, &address, 2, 0);
        check(peer2 != nullptr, "reconnect issued");
        bool connected2 = false;
        waitFor(host, peer2, "", 3000, &connected2);
        check(connected2, "reconnected");
        // Real clients identify with requestedName on reconnect.
        sendText(peer2, "requestedName|" + growId + "\ncountry|ID");
        check(waitFor(host, peer2, "Identity confirmed", 3000, nullptr), "reconnect resumed");
        finish(host);
        return g_failures == 0 ? 0 : 1;
    }
    check(waitFor(host, peer, "Welcome, " + growId + ".", 3000, nullptr), "protocol login accepted");

    // Step 3: identity packet. Login completion delivers the GDPR override,
    // super-main and starter inventory (tank type 9). The items database
    // (tank 16) is only served on request, like real clients do.
    sendText(peer, "tankIDName|" + growId + "\ncountry|ID");
    check(waitFor(host, peer, "Identity confirmed", 3000, nullptr), "identity confirmed");
    check(g_sawTankType[9], "starter inventory received");
    sendText(peer, "action|refresh_item_data");
    check(waitForTank(host, 16, 30000), "item database received");

    // Step 3b: enter the game menu.
    sendText(peer, "action|enter_game");
    check(waitFor(host, peer, "Where would you like to go?", 3000, nullptr), "game menu shown");

    if (mode == "admin")
    {
        // Step A1: staff gate passes, unknown target reported.
        sendText(peer, "action|input\ntext|/kick NoSuchPlayer");
        check(waitFor(host, peer, "Player not found.", 3000, nullptr), "admin gate passes");

        // Step A2: mute self.
        sendText(peer, std::string("action|input\ntext|/mute ") + growId);
        check(waitFor(host, peer, "is muted.", 3000, nullptr), "mute confirmed");

        // Step A3: ban self disconnects the session.
        sendText(peer, std::string("action|input\ntext|/ban ") + growId);
        bool sawDisconnect = false;
        ENetEvent event{};
        int waited = 0;
        while (!sawDisconnect && waited < 3000)
        {
            while (enet_host_service(host, &event, 100) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_DISCONNECT)
                    sawDisconnect = true;
                if (event.type == ENET_EVENT_TYPE_RECEIVE)
                    enet_packet_destroy(event.packet);
            }
            waited += 100;
        }
        check(sawDisconnect, "ban disconnects");

        finish(host);
        return g_failures == 0 ? 0 : 1;
    }

    // Step 4: chat command through action|input (before joining a world).
    sendText(peer, "action|input\ntext|/who");
    check(waitFor(host, peer, "Join a world first.", 3000, nullptr), "command /who answered");

    // Step 4b: plain chat is broadcast back to the world-less client? No:
    // chat requires a world. Join first via warp below, then chat.
    sendText(peer, "action|input\ntext|/mod TestUser");
    check(waitFor(host, peer, "Unknown command.", 3000, nullptr), "player cannot use staff command");

    // Step 5: enter a world. The map payload (tank type 4) arrives before the
    // confirmation text, so it is observed during the text wait above.
    sendText(peer, "action|input\ntext|/warp TEST");
    check(waitFor(host, peer, "World TEST entered.", 3000, nullptr), "world TEST entered");
    check(g_sawTankType[4], "map data received");

    // Step 6: movement relay path (no reply expected for a lone client).
    // Position the avatar near tile row 45 (dirt zone in the new worldgen).
    sendMovement(peer, 50 * 32 + 16, 45 * 32 + 16);

    // Step 7: punch dirt/rock tiles — each hit relays a tile damage packet
    // ((damage << 24) | 0x08) unless the tile happens to be a cave pocket.
    bool sawDamage = false;
    for (int x = 48; x <= 51 && !sawDamage; ++x)
    {
        sendTank(peer, 3, 18, x, 45);
        sawDamage = waitForTankMask(host, 0xFF, 0x08, 3000);
    }
    check(sawDamage, "tile damage relayed");

    // Step 8: plain chat is broadcast to the world.
    sendText(peer, "action|input\ntext|hello world");
    check(waitFor(host, peer, "hello world", 3000, nullptr), "chat broadcast");

    enet_peer_disconnect(peer, 0);
    enet_host_flush(host);

    finish(host);
    return g_failures == 0 ? 0 : 1;
}
