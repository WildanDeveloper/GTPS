// WildanDev unit tests: base64, roles, variant encoding, items database.
#include <cstdio>
#include <string>

#include "base64.hpp"
#include "items.hpp"
#include "roles.hpp"
#include "variant.hpp"

namespace
{

int g_failures = 0;

void check(bool condition, const char* name)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition)
        ++g_failures;
}

} // namespace

int main()
{
    using namespace WildanDev;

    // Base64 round-trips.
    check(Base64::encode("hello") == "aGVsbG8=", "base64 encode");
    auto decoded = Base64::decode("aGVsbG8=");
    check(decoded.has_value() && decoded.value() == "hello", "base64 decode");
    check(!Base64::decode("!!!").has_value(), "base64 rejects garbage");
    check(!Base64::decode("=AAA").has_value(), "base64 rejects misplaced padding");
    check(!Base64::decode("AB=C").has_value(), "base64 rejects interior padding");
    auto token = Base64::decode("X3Rva2VuPXgmZ3Jvd0lkPUJvYiZwYXNzd29yZD1wd2Q=");
    check(token.has_value() && token.value().find("growId=Bob") != std::string::npos, "base64 token shape");

    // Roles table (works from repo root or tests/ directory).
    RoleManager roles;
    const char* rolesPaths[] = {"resources/roles.conf", "../resources/roles.conf"};
    bool rolesLoaded = false;
    for (const char* path : rolesPaths)
    {
        if (roles.load(path))
        {
            rolesLoaded = true;
            break;
        }
    }
    check(rolesLoaded, "roles load");
    const Role* owner = roles.getRole(1);
    const Role* player = roles.getDefaultRole();
    check(owner != nullptr && owner->hasPermission("anything.at.all"), "owner wildcard");
    check(player != nullptr && player->name == "Player", "default role is Player");
    check(player != nullptr && !player->hasPermission("command.mod"), "player lacks mod perm");
    check(player != nullptr && player->hasPermission("command.basic"), "player has basic perm");

    // Variant encoding shape: count byte then index/type/data.
    std::vector<uint8_t> encoded =
        encodeVariantList({VariantValue::makeString("OnConsoleMessage"), VariantValue::makeString("Hi")});
    check(encoded.size() > 3 && encoded[0] == 2 && encoded[1] == 0 && encoded[2] == 2, "variant header");
    check(encoded.size() == 1 + (1 + 1 + 4 + 16) + (1 + 1 + 4 + 2), "variant length");

    // Items database round-trip.
    std::vector<uint8_t> blob = encodeItemsDat(builtinCatalog());
    check(blob.size() > 6 && blob[0] == 0x1A && blob[1] == 0x00, "items version header");
    uint32_t count = static_cast<uint32_t>(blob[2] | (blob[3] << 8) | (blob[4] << 16) | (blob[5] << 24));
    check(count == builtinCatalog().size(), "items count header");

    std::vector<std::string> names = decodeAllNames(blob);
    bool namesMatch = names.size() == builtinCatalog().size();
    for (std::size_t i = 0; namesMatch && i < names.size(); ++i)
        namesMatch = names[i] == builtinCatalog()[i].name;
    check(namesMatch, "items name round-trip");

    check(fnv1a32(blob) == fnv1a32(encodeItemsDat(builtinCatalog())), "items hash deterministic");
    check(decodeAllNames(std::vector<uint8_t>{1, 2, 3}).empty(), "items decoder rejects garbage");

    if (g_failures == 0)
        std::printf("ALL UNIT TESTS PASSED\n");
    else
        std::printf("UNIT FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
