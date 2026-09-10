#pragma once

#include "database.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct ssl_ctx_st; // OpenSSL SSL_CTX (typedef'd in openssl/ssl.h).
struct ssl_st;     // OpenSSL SSL.

namespace WildanDev
{

struct LoginConfig
{
    std::string resourcesDir;
    std::string certFile;
    std::string keyFile;
    std::string host{"0.0.0.0"};
    int port{8092};
    DbConfig db; // own MariaDB connection (register + recovery)
    // Resend.com email delivery (password recovery). Empty key disables it.
    std::string resendApiKey;
    std::string resendFrom{"WildanDev GTPS <onboarding@resend.dev>"};
    // Google sign-in (OAuth 2.0). Empty client id hides the button.
    std::string googleClientId;
    std::string googleClientSecret;
    std::string googleRedirectUri;
    int defaultRoleId{4};
};

// HTTPS login service (OpenSSL). Serves the Growtopia handshake endpoints
// plus web account management:
//   POST /growtopia/server_data.php         -> resources/server_data.txt
//   GET/POST /player/login/dashboard        -> login form (username+password)
//   GET/POST /player/register               -> register form (username,
//                                              password, confirm, email opt.)
//   GET/POST /player/forgot                 -> request a reset code (email)
//   GET/POST /player/reset                  -> code + new password
//   POST /player/growid/login/validate      -> JSON with the account token
//   POST /player/growid/checktoken          -> 307 to validate/checktoken
//   POST /player/growid/validate/checktoken -> token refresh (issued set only)
class LoginService
{
public:
    ~LoginService();
    LoginService() = default;
    LoginService(const LoginService&) = delete;
    LoginService& operator=(const LoginService&) = delete;

    // Returns false when the certificate files are missing or the socket
    // cannot be bound (the game server keeps running either way).
    bool start(const LoginConfig& config);
    void stop();

private:
    void serve(ssl_ctx_st* ctx);
    void handleConnection(ssl_st* ssl);

    int m_listenFd{-1};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    LoginConfig m_config;
    Database m_database;
    std::mutex m_dbMutex; // one MYSQL handle shared by connection threads
};

} // namespace WildanDev
