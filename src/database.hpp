#pragma once

#include <cstdint>
#include <memory>
#include <mysql/mysql.h>
#include <optional>
#include <string>
#include <vector>

namespace WildanDev
{

struct DbConfig
{
    std::string host{"127.0.0.1"};
    std::string user{"root"};
    std::string password;
    std::string name{"wildandev"};
    int port{3306};
};

struct PlayerRecord
{
    uint32_t id{0};
    std::string growId;
    int roleId{0};
    bool muted{false};
    bool banned{false};
    bool isNew{false};
    int gems{0};
};

// MariaDB-backed player store. Passwords are never stored in clear text:
// new accounts keep a random salt plus PBKDF2-HMAC-SHA256
// ("pbkdf2_sha256$iterations$salthex$hashhex"). Legacy rows written as
// SHA-256(salt + password) hex are verified and transparently upgraded on
// the next successful login.
class Database
{
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool connect(const DbConfig& config);
    bool isConnected() const { return m_handle != nullptr; }
    void reset()
    {
        if (m_handle != nullptr)
        {
            mysql_close(m_handle);
            m_handle = nullptr;
        }
    }

    // Returns the existing record when the password verifies, creates the
    // account with the default role otherwise. Empty optional on failure.
    std::optional<PlayerRecord> loginOrRegister(const std::string& growId, const std::string& password,
                                                int defaultRoleId);

    bool setRole(uint32_t playerId, int roleId);
    bool setMuted(uint32_t playerId, bool muted);
    bool setBanned(uint32_t playerId, bool banned);
    bool setGems(uint32_t playerId, int gems);
    std::optional<uint32_t> findPlayerId(const std::string& growId);
    std::optional<std::string> findPlayerById(uint32_t playerId);

    // Web account management (register + password recovery).
    enum class RegisterResult { Ok, Duplicate, Error };
    RegisterResult registerPlayer(const std::string& growId, const std::string& password,
                                  const std::string& email, int defaultRoleId);
    // (player_id, email) — email empty when the account has none.
    std::optional<std::pair<uint32_t, std::string>> getPlayerContact(const std::string& growId);
    bool createResetCode(uint32_t playerId, const std::string& sixDigitCode, int ttlSeconds);
    bool consumeResetCode(uint32_t playerId, const std::string& sixDigitCode);
    bool updatePassword(uint32_t playerId, const std::string& newPassword);

    // Low-level helpers for binary-safe world storage.
    bool exec(const std::string& sql);
    std::string escape(const uint8_t* data, std::size_t length);
    bool upsertWorld(const std::string& name, int ownerId, const uint8_t* blocks, std::size_t blocksLength,
                     const uint8_t* objects, std::size_t objectsLength);
    bool loadWorld(const std::string& name, int& ownerId, std::vector<uint8_t>& blocks,
                   std::vector<uint8_t>& objects);

    bool loadInventory(uint32_t playerId, std::vector<std::pair<int, int>>& slots);
    bool setInventoryItem(uint32_t playerId, int itemId, int count);
private:
    bool ensureSchema();
    bool ensureWorldsTable();
    bool ensureConnection();
    bool columnExists(const char* table, const char* column);
    std::string hashPassword(const std::string& saltHex, const std::string& password) const;
    std::string hashPasswordRaw(const std::string& saltBytes, const std::string& password) const;
    std::string sha256Hex(const std::string& text);

    DbConfig m_config;
    MYSQL* m_handle{nullptr};
};

} // namespace WildanDev
