#include "database.hpp"
#include "logger.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <sstream>

namespace WildanDev
{
namespace
{

bool validDbName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (char c : name)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    }
    return true;
}

// Constant-time comparison of two hex digests.
bool digestsEqual(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    volatile unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return diff == 0;
}

constexpr int kPbkdf2Iterations = 100000;

} // namespace

Database::~Database()
{
    if (m_handle != nullptr)
    {
        mysql_close(m_handle);
        m_handle = nullptr;
    }
}

bool Database::connect(const DbConfig& config)
{
    if (!validDbName(config.name))
    {
        logError("Invalid database name in configuration (letters, digits, underscore only)");
        return false;
    }
    m_config = config;

    m_handle = mysql_init(nullptr);
    if (m_handle == nullptr)
    {
        logError("mysql_init failed");
        return false;
    }

    // Reconnect transparently when MariaDB drops an idle connection.
    bool reconnect = true;
    mysql_options(m_handle, MYSQL_OPT_RECONNECT, &reconnect);

    if (mysql_real_connect(m_handle, config.host.c_str(), config.user.c_str(), config.password.c_str(), nullptr,
                           config.port, nullptr, 0) == nullptr)
    {
        logError(std::string("Database connect failed: ") + mysql_error(m_handle));
        mysql_close(m_handle);
        m_handle = nullptr;
        return false;
    }

    // Name is validated above, so concatenation is safe here.
    std::string createDb = "CREATE DATABASE IF NOT EXISTS `" + config.name + "`";
    if (mysql_query(m_handle, createDb.c_str()) != 0)
    {
        logError(std::string("CREATE DATABASE failed: ") + mysql_error(m_handle));
        return false;
    }
    if (mysql_select_db(m_handle, config.name.c_str()) != 0)
    {
        logError(std::string("USE database failed: ") + mysql_error(m_handle));
        return false;
    }
    return ensureSchema();
}

bool Database::ensureConnection()
{
    if (m_handle == nullptr)
        return false;
    if (mysql_ping(m_handle) == 0)
        return true;

    logWarn(std::string("Database connection lost (") + mysql_error(m_handle) + "), reconnecting");
    mysql_close(m_handle);
    m_handle = mysql_init(nullptr);
    if (m_handle == nullptr)
        return false;
    bool reconnect = true;
    mysql_options(m_handle, MYSQL_OPT_RECONNECT, &reconnect);
    if (mysql_real_connect(m_handle, m_config.host.c_str(), m_config.user.c_str(), m_config.password.c_str(), nullptr,
                           m_config.port, nullptr, 0) == nullptr ||
        mysql_select_db(m_handle, m_config.name.c_str()) != 0)
    {
        logError(std::string("Database reconnect failed: ") + mysql_error(m_handle));
        mysql_close(m_handle);
        m_handle = nullptr;
        return false;
    }
    return true;
}

bool Database::columnExists(const char* table, const char* column)
{
    // m_config.name is validated (alphanumeric/underscore) before use.
    std::string sql = "SELECT 1 FROM information_schema.COLUMNS WHERE TABLE_SCHEMA='" + m_config.name +
                      "' AND TABLE_NAME='" + table + "' AND COLUMN_NAME='" + column + "' LIMIT 1";
    if (mysql_query(m_handle, sql.c_str()) != 0)
        return false;
    MYSQL_RES* result = mysql_store_result(m_handle);
    if (result == nullptr)
        return false;
    bool found = mysql_fetch_row(result) != nullptr;
    mysql_free_result(result);
    return found;
}

bool Database::ensureSchema()
{
    if (!ensureConnection())
        return false;
    static constexpr char kSchema[] =
        "CREATE TABLE IF NOT EXISTS players ("
        "id INT UNSIGNED AUTO_INCREMENT PRIMARY KEY,"
        "growid VARCHAR(32) NOT NULL UNIQUE,"
        "pass_hash VARCHAR(255) NOT NULL,"
        "salt VARCHAR(255) NOT NULL,"
        "role_id INT NOT NULL DEFAULT 4,"
        "is_muted TINYINT NOT NULL DEFAULT 0,"
        "is_banned TINYINT NOT NULL DEFAULT 0,"
        "gems INT NOT NULL DEFAULT 0,"
        "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP"
        ")";
    if (mysql_query(m_handle, kSchema) != 0)
    {
        logError(std::string("CREATE TABLE failed: ") + mysql_error(m_handle));
        return false;
    }
    // Portable upgrades: only alter when the column is actually missing
    // (ADD COLUMN IF NOT EXISTS is MariaDB-specific).
    if (!columnExists("players", "is_muted") &&
        !exec("ALTER TABLE players ADD COLUMN is_muted TINYINT NOT NULL DEFAULT 0"))
        return false;
    if (!columnExists("players", "is_banned") &&
        !exec("ALTER TABLE players ADD COLUMN is_banned TINYINT NOT NULL DEFAULT 0"))
        return false;
    if (!columnExists("players", "gems") && !exec("ALTER TABLE players ADD COLUMN gems INT NOT NULL DEFAULT 0"))
        return false;
    if (!columnExists("players", "pass_hash"))
        return false;
    // Widen hash/salt columns for pre-PBKDF2 databases (no-op when already
    // wide).
    exec("ALTER TABLE players MODIFY pass_hash VARCHAR(255) NOT NULL");
    exec("ALTER TABLE players MODIFY salt VARCHAR(255) NOT NULL");
    return exec("CREATE TABLE IF NOT EXISTS inventory ("
                "player_id INT UNSIGNED NOT NULL,"
                "item_id INT NOT NULL,"
                "item_count INT NOT NULL DEFAULT 0,"
                "PRIMARY KEY (player_id, item_id))");
}

std::string Database::sha256Hex(const std::string& text)
{
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest);

    std::ostringstream out;
    out << std::hex;
    for (unsigned char byte : digest)
    {
        out.width(2);
        out.fill('0');
        out << static_cast<int>(byte);
    }
    return out.str();
}

// PBKDF2-HMAC-SHA256 over the raw salt bytes (decoded from the stored hex).
// Stored as "pbkdf2_sha256$iterations$salthex$hashhex".
std::string Database::hashPassword(const std::string& saltHex, const std::string& password) const
{
    // Strict, exception-free hex decode: a malformed stored hash must fail
    // the login, never crash the server.
    if (saltHex.size() % 2 != 0)
        return "";
    auto hexValue = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (char c : saltHex)
    {
        if (hexValue(c) < 0)
            return "";
    }
    std::string saltBytes;
    saltBytes.reserve(saltHex.size() / 2);
    for (std::size_t i = 0; i + 1 < saltHex.size(); i += 2)
        saltBytes.push_back(static_cast<char>((hexValue(saltHex[i]) << 4) | hexValue(saltHex[i + 1])));
    return hashPasswordRaw(saltBytes, password);
}

// Same scheme, but takes the salt bytes directly and hex-encodes them for
// storage. Used when upgrading legacy rows whose salt was plain text.
std::string Database::hashPasswordRaw(const std::string& saltBytes, const std::string& password) const
{
    unsigned char digest[SHA256_DIGEST_LENGTH] = {};
    if (PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                          reinterpret_cast<const unsigned char*>(saltBytes.data()),
                          static_cast<int>(saltBytes.size()), kPbkdf2Iterations, EVP_sha256(), sizeof(digest),
                          digest) != 1)
    {
        return "";
    }
    std::ostringstream saltHex;
    saltHex << std::hex;
    for (char byte : saltBytes)
    {
        saltHex.width(2);
        saltHex.fill('0');
        saltHex << static_cast<int>(static_cast<unsigned char>(byte));
    }
    std::ostringstream out;
    out << std::hex;
    for (unsigned char byte : digest)
    {
        out.width(2);
        out.fill('0');
        out << static_cast<int>(byte);
    }
    return "pbkdf2_sha256$" + std::to_string(kPbkdf2Iterations) + "$" + saltHex.str() + "$" + out.str();
}

namespace
{

// Binds a std::string parameter for the lifetime of the statement. The
// caller must keep the BoundString alive until after mysql_stmt_execute.
struct BoundString
{
    MYSQL_BIND bind{};
    unsigned long length{0};

    explicit BoundString(const std::string& value)
    {
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_STRING;
        bind.buffer = const_cast<char*>(value.data());
        bind.buffer_length = static_cast<unsigned long>(value.size());
        length = static_cast<unsigned long>(value.size());
        bind.length = &length;
    }
};

struct BoundUInt
{
    MYSQL_BIND bind{};
    uint32_t storage{0};

    explicit BoundUInt(uint32_t value) : storage(value)
    {
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_LONG;
        bind.buffer = &storage;
        bind.is_unsigned = 1;
    }
};

struct BoundInt
{
    MYSQL_BIND bind{};
    int storage{0};

    explicit BoundInt(int value) : storage(value)
    {
        std::memset(&bind, 0, sizeof(bind));
        bind.buffer_type = MYSQL_TYPE_LONG;
        bind.buffer = &storage;
    }
};

} // namespace

std::optional<PlayerRecord> Database::loginOrRegister(const std::string& growId, const std::string& password,
                                                      int defaultRoleId)
{
    if (!ensureConnection())
        return std::nullopt;

    // Register first: the INSERT doubles as the existence check, so two
    // concurrent registrations cannot race SELECT-then-INSERT.
    unsigned char saltBytes[16] = {};
    if (RAND_bytes(saltBytes, sizeof(saltBytes)) != 1)
    {
        logError("RAND_bytes failed");
        return std::nullopt;
    }
    std::ostringstream saltOut;
    saltOut << std::hex;
    for (unsigned char byte : saltBytes)
    {
        saltOut.width(2);
        saltOut.fill('0');
        saltOut << static_cast<int>(byte);
    }
    std::string salt = saltOut.str();
    std::string hash = hashPassword(salt, password);
    if (hash.empty())
    {
        logError("hashPassword failed");
        return std::nullopt;
    }

    {
        MYSQL_STMT* insertStmt = mysql_stmt_init(m_handle);
        if (insertStmt == nullptr)
            return std::nullopt;
        constexpr char kInsert[] = "INSERT INTO players (growid, pass_hash, salt, role_id) VALUES (?, ?, ?, ?)";
        if (mysql_stmt_prepare(insertStmt, kInsert, std::strlen(kInsert)) != 0)
        {
            mysql_stmt_close(insertStmt);
        }
        else
        {
            BoundString growValue(growId), hashValue(hash), saltValue(salt);
            BoundInt roleValue(defaultRoleId);
            MYSQL_BIND in[4] = {growValue.bind, hashValue.bind, saltValue.bind, roleValue.bind};
            if (mysql_stmt_bind_param(insertStmt, in) == 0 && mysql_stmt_execute(insertStmt) == 0)
            {
                uint32_t newId = static_cast<uint32_t>(mysql_stmt_insert_id(insertStmt));
                mysql_stmt_close(insertStmt);
                logInfo("Registered new account '" + growId + "'");
                return PlayerRecord{newId, growId, defaultRoleId, false, false, true, 0};
            }
            bool duplicate = mysql_stmt_errno(insertStmt) == 1062;
            mysql_stmt_close(insertStmt);
            if (!duplicate)
            {
                logError("INSERT register failed (account may exist but error was not a duplicate)");
                return std::nullopt;
            }
        }
    }

    // Account exists: verify the password.
    MYSQL_STMT* selectStmt = mysql_stmt_init(m_handle);
    if (selectStmt == nullptr)
        return std::nullopt;
    constexpr char kSelect[] =
        "SELECT id, pass_hash, salt, role_id, is_muted, is_banned, gems FROM players WHERE growid = ? LIMIT 1";
    if (mysql_stmt_prepare(selectStmt, kSelect, std::strlen(kSelect)) != 0)
    {
        logError(std::string("SELECT prepare failed: ") + mysql_stmt_error(selectStmt));
        mysql_stmt_close(selectStmt);
        return std::nullopt;
    }
    BoundString growParam(growId);
    if (mysql_stmt_bind_param(selectStmt, &growParam.bind) != 0 || mysql_stmt_execute(selectStmt) != 0)
    {
        logError(std::string("SELECT execute failed: ") + mysql_stmt_error(selectStmt));
        mysql_stmt_close(selectStmt);
        return std::nullopt;
    }

    uint32_t foundId = 0;
    char hashBuf[256] = {};
    char saltBuf[256] = {};
    int foundRole = 0;
    int foundGems = 0;
    char foundMuted = 0;
    char foundBanned = 0;
    unsigned long hashLen = 0, saltLen = 0;
    MYSQL_BIND out[7] = {};
    out[0].buffer_type = MYSQL_TYPE_LONG;
    out[0].buffer = &foundId;
    out[0].is_unsigned = 1;
    out[1].buffer_type = MYSQL_TYPE_STRING;
    out[1].buffer = hashBuf;
    out[1].buffer_length = sizeof(hashBuf) - 1;
    out[1].length = &hashLen;
    out[2].buffer_type = MYSQL_TYPE_STRING;
    out[2].buffer = saltBuf;
    out[2].buffer_length = sizeof(saltBuf) - 1;
    out[2].length = &saltLen;
    out[3].buffer_type = MYSQL_TYPE_LONG;
    out[3].buffer = &foundRole;
    out[4].buffer_type = MYSQL_TYPE_TINY;
    out[4].buffer = &foundMuted;
    out[5].buffer_type = MYSQL_TYPE_TINY;
    out[5].buffer = &foundBanned;
    out[6].buffer_type = MYSQL_TYPE_LONG;
    out[6].buffer = &foundGems;
    mysql_stmt_bind_result(selectStmt, out);

    int fetchStatus = mysql_stmt_fetch(selectStmt);
    mysql_stmt_close(selectStmt);

    if (fetchStatus == MYSQL_NO_DATA)
    {
        // Row vanished between the duplicate INSERT and the SELECT.
        logWarn("Account row disappeared during login");
        return std::nullopt;
    }
    if (fetchStatus != 0)
    {
        // Includes MYSQL_DATA_TRUNCATED: never trust a partially fetched row.
        logError("SELECT fetch failed or was truncated");
        return std::nullopt;
    }

    std::string storedHash(hashBuf, hashLen);
    std::string storedSalt(saltBuf, saltLen);
    if (storedHash.empty())
    {
        logError("Stored password hash is empty for account '" + growId + "'");
        return std::nullopt;
    }

    bool verified = false;
    if (storedHash.rfind("pbkdf2_sha256$", 0) == 0)
    {
        // pbkdf2_sha256$iterations$salthex$hashhex
        auto first = storedHash.find('$', 14);
        auto second = first == std::string::npos ? std::string::npos : storedHash.find('$', first + 1);
        if (first != std::string::npos && second != std::string::npos)
        {
            std::string saltHex = storedHash.substr(first + 1, second - first - 1);
            std::string expected = storedHash.substr(second + 1);
            std::string computed = hashPassword(saltHex, password);
            // computed ends with "$hashhex"; compare only the hex digest part.
            verified = !computed.empty() && computed.rfind('$') != std::string::npos &&
                       digestsEqual(computed.substr(computed.rfind('$') + 1), expected);
        }
    }
    else
    {
        // Legacy single-pass SHA-256(salt + password); upgrade on success.
        std::string computed = sha256Hex(storedSalt + password);
        verified = digestsEqual(computed, storedHash);
        if (verified)
        {
            // Re-derive under PBKDF2 using the legacy salt bytes verbatim.
            std::string upgraded = hashPasswordRaw(storedSalt, password);
            if (!upgraded.empty())
            {
                exec("UPDATE players SET pass_hash='" +
                     escape(reinterpret_cast<const uint8_t*>(upgraded.data()), upgraded.size()) +
                     "' WHERE id=" + std::to_string(foundId));
            }
        }
    }

    if (!verified)
    {
        logWarn("Rejected login for existing account (bad password)");
        return std::nullopt;
    }
    if (foundBanned != 0)
    {
        logWarn("Rejected login for banned account");
        return std::nullopt;
    }
    PlayerRecord record{foundId, growId, foundRole, foundMuted != 0, false, false, foundGems};
    return record;
}

bool Database::setRole(uint32_t playerId, int roleId)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return false;
    constexpr char kUpdate[] = "UPDATE players SET role_id = ? WHERE id = ?";
    if (mysql_stmt_prepare(stmt, kUpdate, std::strlen(kUpdate)) != 0)
    {
        logError(std::string("UPDATE prepare failed: ") + mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }
    BoundInt roleValue(roleId);
    BoundUInt idValue(playerId);
    MYSQL_BIND in[2] = {roleValue.bind, idValue.bind};
    bool ok = (mysql_stmt_bind_param(stmt, in) == 0) && (mysql_stmt_execute(stmt) == 0);
    if (!ok)
        logError(std::string("UPDATE execute failed: ") + mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ok;
}

bool Database::exec(const std::string& sql)
{
    if (!ensureConnection())
    {
        logError("Query skipped: database unreachable");
        return false;
    }
    if (mysql_query(m_handle, sql.c_str()) != 0)
    {
        logError(std::string("Query failed: ") + mysql_error(m_handle));
        return false;
    }
    return true;
}

bool Database::setMuted(uint32_t playerId, bool muted)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return false;
    constexpr char kUpdate[] = "UPDATE players SET is_muted = ? WHERE id = ?";
    if (mysql_stmt_prepare(stmt, kUpdate, std::strlen(kUpdate)) != 0)
    {
        logError(std::string("UPDATE prepare failed: ") + mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }
    BoundInt mutedValue(muted ? 1 : 0);
    BoundUInt idValue(playerId);
    MYSQL_BIND in[2] = {mutedValue.bind, idValue.bind};
    bool ok = (mysql_stmt_bind_param(stmt, in) == 0) && (mysql_stmt_execute(stmt) == 0);
    if (!ok)
        logError(std::string("UPDATE execute failed: ") + mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ok;
}

bool Database::setBanned(uint32_t playerId, bool banned)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return false;
    constexpr char kUpdate[] = "UPDATE players SET is_banned = ? WHERE id = ?";
    if (mysql_stmt_prepare(stmt, kUpdate, std::strlen(kUpdate)) != 0)
    {
        logError(std::string("UPDATE prepare failed: ") + mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }
    BoundInt bannedValue(banned ? 1 : 0);
    BoundUInt idValue(playerId);
    MYSQL_BIND in[2] = {bannedValue.bind, idValue.bind};
    bool ok = (mysql_stmt_bind_param(stmt, in) == 0) && (mysql_stmt_execute(stmt) == 0);
    if (!ok)
        logError(std::string("UPDATE execute failed: ") + mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ok;
}

bool Database::loadInventory(uint32_t playerId, std::vector<std::pair<int, int>>& slots)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return false;
    constexpr char kSelect[] = "SELECT item_id, item_count FROM inventory WHERE player_id = ?";
    if (mysql_stmt_prepare(stmt, kSelect, std::strlen(kSelect)) != 0)
    {
        mysql_stmt_close(stmt);
        return false;
    }
    BoundUInt idValue(playerId);
    if (mysql_stmt_bind_param(stmt, &idValue.bind) != 0 || mysql_stmt_execute(stmt) != 0)
    {
        mysql_stmt_close(stmt);
        return false;
    }

    int itemId = 0;
    int itemCount = 0;
    MYSQL_BIND out[2] = {};
    out[0].buffer_type = MYSQL_TYPE_LONG;
    out[0].buffer = &itemId;
    out[1].buffer_type = MYSQL_TYPE_LONG;
    out[1].buffer = &itemCount;
    mysql_stmt_bind_result(stmt, out);

    slots.clear();
    while (mysql_stmt_fetch(stmt) == 0)
        slots.emplace_back(itemId, itemCount);
    mysql_stmt_close(stmt);
    return true;
}

bool Database::setInventoryItem(uint32_t playerId, int itemId, int count)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return false;
    constexpr char kUpsert[] = "INSERT INTO inventory (player_id, item_id, item_count) VALUES (?, ?, ?) "
                               "ON DUPLICATE KEY UPDATE item_count=VALUES(item_count)";
    if (mysql_stmt_prepare(stmt, kUpsert, std::strlen(kUpsert)) != 0)
    {
        logError(std::string("UPSERT prepare failed: ") + mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return false;
    }
    BoundUInt playerValue(playerId);
    BoundInt itemValue(itemId);
    BoundInt countValue(count);
    MYSQL_BIND in[3] = {playerValue.bind, itemValue.bind, countValue.bind};
    bool ok = (mysql_stmt_bind_param(stmt, in) == 0) && (mysql_stmt_execute(stmt) == 0);
    if (!ok)
        logError(std::string("UPSERT execute failed: ") + mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ok;
}

std::optional<std::string> Database::findPlayerById(uint32_t playerId)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return std::nullopt;
    constexpr char kSelect[] = "SELECT growid FROM players WHERE id = ? LIMIT 1";
    if (mysql_stmt_prepare(stmt, kSelect, std::strlen(kSelect)) != 0)
    {
        mysql_stmt_close(stmt);
        return std::nullopt;
    }
    BoundUInt idValue(playerId);
    if (mysql_stmt_bind_param(stmt, &idValue.bind) != 0 || mysql_stmt_execute(stmt) != 0)
    {
        mysql_stmt_close(stmt);
        return std::nullopt;
    }
    char nameBuf[64] = {};
    unsigned long nameLen = 0;
    MYSQL_BIND out{};
    out.buffer_type = MYSQL_TYPE_STRING;
    out.buffer = nameBuf;
    out.buffer_length = sizeof(nameBuf) - 1;
    out.length = &nameLen;
    mysql_stmt_bind_result(stmt, &out);

    std::optional<std::string> result;
    if (mysql_stmt_fetch(stmt) == 0)
        result = std::string(nameBuf, nameLen);
    mysql_stmt_close(stmt);
    return result;
}

bool Database::setGems(uint32_t playerId, int gems)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return false;
    constexpr char kUpdate[] = "UPDATE players SET gems = ? WHERE id = ?";
    if (mysql_stmt_prepare(stmt, kUpdate, std::strlen(kUpdate)) != 0)
    {
        mysql_stmt_close(stmt);
        return false;
    }
    BoundInt gemsValue(gems);
    BoundUInt idValue(playerId);
    MYSQL_BIND in[2] = {gemsValue.bind, idValue.bind};
    bool ok = (mysql_stmt_bind_param(stmt, in) == 0) && (mysql_stmt_execute(stmt) == 0);
    if (!ok)
        logError(std::string("UPDATE gems failed: ") + mysql_stmt_error(stmt));
    mysql_stmt_close(stmt);
    return ok;
}

bool Database::ensureWorldsTable()
{
    if (!exec("CREATE TABLE IF NOT EXISTS worlds ("
              "name VARCHAR(24) NOT NULL PRIMARY KEY,"
              "owner INT NOT NULL DEFAULT 0,"
              "blocks MEDIUMBLOB,"
              "objects LONGBLOB)"))
        return false;
    if (!columnExists("worlds", "objects") &&
        !exec("ALTER TABLE worlds ADD COLUMN objects LONGBLOB"))
        return false;
    return true;
}

std::optional<uint32_t> Database::findPlayerId(const std::string& growId)
{
    MYSQL_STMT* stmt = mysql_stmt_init(m_handle);
    if (stmt == nullptr)
        return std::nullopt;
    constexpr char kSelect[] = "SELECT id FROM players WHERE growid = ? LIMIT 1";
    if (mysql_stmt_prepare(stmt, kSelect, std::strlen(kSelect)) != 0)
    {
        mysql_stmt_close(stmt);
        return std::nullopt;
    }
    BoundString growValue(growId);
    if (mysql_stmt_bind_param(stmt, &growValue.bind) != 0 || mysql_stmt_execute(stmt) != 0)
    {
        mysql_stmt_close(stmt);
        return std::nullopt;
    }
    uint32_t foundId = 0;
    MYSQL_BIND out{};
    out.buffer_type = MYSQL_TYPE_LONG;
    out.buffer = &foundId;
    out.is_unsigned = 1;
    mysql_stmt_bind_result(stmt, &out);

    std::optional<uint32_t> result;
    if (mysql_stmt_fetch(stmt) == 0)
        result = foundId;
    mysql_stmt_close(stmt);
    return result;
}

std::string Database::escape(const uint8_t* data, std::size_t length)
{
    std::string out;
    out.resize(length * 2 + 1);
    unsigned long encoded = mysql_real_escape_string(m_handle, out.data(),
                                                     reinterpret_cast<const char*>(data),
                                                     static_cast<unsigned long>(length));
    out.resize(encoded);
    return out;
}

namespace
{

std::string escapedString(Database& database, const std::string& text)
{
    return database.escape(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

} // namespace

bool Database::upsertWorld(const std::string& name, int ownerId, const uint8_t* blocks, std::size_t blocksLength,
                           const uint8_t* objects, std::size_t objectsLength)
{
    if (!ensureWorldsTable())
        return false;

    std::string sql = "INSERT INTO worlds (name, owner, blocks, objects) VALUES ('" + escapedString(*this, name) +
                      "', " + std::to_string(ownerId) + ", '" + escape(blocks, blocksLength) + "', '" +
                      escape(objects, objectsLength) +
                      "') ON DUPLICATE KEY UPDATE owner=VALUES(owner), blocks=VALUES(blocks), "
                      "objects=VALUES(objects)";
    return exec(sql);
}

bool Database::loadWorld(const std::string& name, int& ownerId, std::vector<uint8_t>& blocks,
                         std::vector<uint8_t>& objects)
{
    if (!ensureWorldsTable())
        return false;
    std::string sql = "SELECT owner, blocks, objects FROM worlds WHERE name='" + escapedString(*this, name) +
                      "' LIMIT 1";

    if (mysql_query(m_handle, sql.c_str()) != 0)
    {
        logError(std::string("World load failed: ") + mysql_error(m_handle));
        return false;
    }

    MYSQL_RES* result = mysql_store_result(m_handle);
    if (result == nullptr)
        return false;

    bool found = false;
    MYSQL_ROW row = mysql_fetch_row(result);
    if (row != nullptr)
    {
        unsigned long* lengths = mysql_fetch_lengths(result);
        ownerId = row[0] != nullptr ? std::atoi(row[0]) : 0;
        if (row[1] != nullptr)
            blocks.assign(reinterpret_cast<uint8_t*>(row[1]),
                          reinterpret_cast<uint8_t*>(row[1]) + lengths[1]);
        if (row[2] != nullptr)
            objects.assign(reinterpret_cast<uint8_t*>(row[2]),
                           reinterpret_cast<uint8_t*>(row[2]) + lengths[2]);
        found = true;
    }
    mysql_free_result(result);
    return found;
}

} // namespace WildanDev
