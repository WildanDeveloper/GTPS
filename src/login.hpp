#pragma once

#include <atomic>
#include <string>
#include <thread>

struct ssl_ctx_st; // OpenSSL SSL_CTX (typedef'd in openssl/ssl.h).

namespace WildanDev
{

// HTTPS login service (OpenSSL), replacing the old Python backend. Serves
// the Growtopia handshake endpoints in-process:
//   POST /growtopia/server_data.php        -> resources/server_data.txt
//   GET/POST /player/login/dashboard       -> login page carrying a token
//   POST /player/growid/login/validate     -> JSON with the account token
//   POST /player/growid/checktoken         -> 307 to validate/checktoken
//   POST /player/growid/validate/checktoken-> token refresh (issued set only)
class LoginService
{
public:
    ~LoginService();
    LoginService() = default;
    LoginService(const LoginService&) = delete;
    LoginService& operator=(const LoginService&) = delete;

    // Blocks until the listener is up. Returns false when the certificate
    // files are missing or the socket cannot be bound.
    bool start(const std::string& resourcesDir, const std::string& certFile, const std::string& keyFile,
               const std::string& host, int port);
    void stop();

private:
    void serve(ssl_ctx_st* ctx);
    int m_listenFd{-1};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::string m_resourcesDir;
};

} // namespace WildanDev
