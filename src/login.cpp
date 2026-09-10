#include "login.hpp"
#include "base64.hpp"
#include "logger.hpp"

#include <openssl/ssl.h>

#include <netdb.h>

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <openssl/rand.h>
#include <sstream>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace WildanDev
{

namespace
{

constexpr std::size_t kMaxBodyBytes = 64 * 1024;
constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr int kResetCodeTtlSeconds = 900; // 15 minutes

// Tokens issued by this process. A refresh (checktoken) must present one of
// these; anything else is rejected instead of blindly re-encoding whatever
// the client posted.
std::mutex g_issuedMutex;
std::unordered_map<std::string, std::pair<std::string, std::string>> g_issuedTokens;

// Per-username throttle for reset-code requests.
std::mutex g_throttleMutex;
std::map<std::string, std::chrono::steady_clock::time_point> g_resetThrottle;

const unsigned char kFaviconPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};
constexpr std::size_t kFaviconPngSize = sizeof(kFaviconPng);

// ---------------------------------------------------------------------------
// Page design
// ---------------------------------------------------------------------------

const char* kPageCss =
    "*{box-sizing:border-box;margin:0;padding:0}\n"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;"
    "font-family:'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
    "background:radial-gradient(1200px 600px at 20% -10%,#1d3a24 0%,#0d1712 55%,#090d0a 100%);color:#e8f0e8}\n"
    ".card{background:#121b14;border:1px solid #24382a;border-radius:14px;padding:34px 30px;width:340px;"
    "box-shadow:0 18px 50px rgba(0,0,0,.55)}\n"
    "h1{font-size:20px;letter-spacing:.4px;margin-bottom:4px;color:#b6e388}\n"
    "p.sub{font-size:12.5px;color:#8fa893;margin-bottom:22px}\n"
    "label{display:block;font-size:12px;color:#a8bfa9;margin:12px 0 5px;letter-spacing:.3px}\n"
    "input{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #2c4433;background:#0c130e;"
    "color:#e8f0e8;font-size:14px;outline:none}\n"
    "input:focus{border-color:#4c8a3f;box-shadow:0 0 0 3px rgba(76,138,63,.25)}\n"
    "button{width:100%;margin-top:20px;padding:11px;border:0;border-radius:8px;"
    "background:linear-gradient(180deg,#5aa63f,#417c2c);color:#fff;font-size:14.5px;font-weight:600;"
    "cursor:pointer}\n"
    "button:hover{filter:brightness(1.1)}\n"
    ".row{display:flex;justify-content:space-between;margin-top:16px;font-size:12.5px}\n"
    "a{color:#7ec850;text-decoration:none}a:hover{text-decoration:underline}\n"
    ".msg{background:#1a2b1e;border:1px solid #2f4a36;color:#c9e6b8;border-radius:8px;"
    "padding:10px 12px;font-size:13px;margin-bottom:14px}\n"
    ".err{background:#2b1a1a;border-color:#4a2f2f;color:#e6b8b8}\n"
    ".code{font-size:34px;font-weight:700;letter-spacing:10px;color:#b6e388;text-align:center;"
    "padding:18px 0 10px}\n"
    ".hint{font-size:11.5px;color:#7e967f;margin-top:14px;line-height:1.5}\n";

std::string pageShell(const std::string& title, const std::string& body)
{
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>" + title + "</title><style>" + kPageCss + "</style></head><body>"
           "<div class=\"card\">" + body + "</div></body></html>";
}

// The plain login form — the Growtopia client parses this page for the
// hidden _token input and submits it to /player/growid/login/validate.
const char* kLoginPage =
    "<form method=\"POST\" action=\"/player/growid/login/validate\">"
    "<input type=\"hidden\" name=\"_token\" value=\"__TOKEN__\">"
    "<label>Username</label><input name=\"growId\" required maxlength=\"32\">"
    "<label>Password</label><input type=\"password\" name=\"password\" required maxlength=\"64\">"
    "<button>Log in</button></form>";

std::string loginPage(const std::string& token, const std::string& message, bool isError)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    std::string page = kLoginPage;
    page.replace(page.find("__TOKEN__"), 9, token);
    return pageShell("WildanDev GTPS — Login",
                     "<h1>WildanDev GTPS</h1><p class=\"sub\">Log in with your account.</p>" + msg + page +
                         "<div class=\"row\"><a href=\"/player/forgot\">Forgot password?</a>"
                         "<a href=\"/player/register\">Create account</a></div>");
}

std::string registerPage(const std::string& message, bool isError)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    return pageShell("WildanDev GTPS — Register",
                     "<h1>Create account</h1><p class=\"sub\">Pick a username and password.</p>" + msg +
                         "<form method=\"POST\" action=\"/player/growid/register\">"
                         "<label>Username</label><input name=\"growId\" required maxlength=\"32\" "
                         "pattern=\"[A-Za-z0-9_]{1,32}\">"
                         "<label>Password</label><input type=\"password\" name=\"password\" required "
                         "maxlength=\"64\">"
                         "<label>Confirm password</label><input type=\"password\" name=\"password2\" required "
                         "maxlength=\"64\">"
                         "<label>Email <span style=\"color:#6f8a71\">(optional — for password reset)</span>"
                         "</label><input type=\"email\" name=\"email\" maxlength=\"255\">"
                         "<button>Create account</button></form>"
                         "<div class=\"row\"><a href=\"/player/login/dashboard\">Back to login</a></div>");
}

std::string forgotPage(const std::string& message, bool isError)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    return pageShell("WildanDev GTPS — Reset password",
                     "<h1>Reset password</h1>"
                     "<p class=\"sub\">Enter your username. If it has an email on file, a 6-digit code is "
                     "sent to it.</p>" + msg +
                         "<form method=\"POST\" action=\"/player/growid/forgot\">"
                         "<label>Username</label><input name=\"growId\" required maxlength=\"32\">"
                         "<button>Send reset code</button></form>"
                         "<div class=\"row\"><a href=\"/player/login/dashboard\">Back to login</a>"
                         "<a href=\"/player/reset\">I have a code</a></div>");
}

std::string resetPage(const std::string& growId, const std::string& message, bool isError)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    return pageShell("WildanDev GTPS — New password",
                     "<h1>Set a new password</h1>"
                     "<p class=\"sub\">Enter the 6-digit code from your email.</p>" + msg +
                         "<form method=\"POST\" action=\"/player/growid/reset\">"
                         "<label>Username</label><input name=\"growId\" required maxlength=\"32\" value=\"" +
                         growId + "\">"
                         "<label>Reset code</label><input name=\"code\" required maxlength=\"6\" "
                         "pattern=\"[0-9]{6}\" inputmode=\"numeric\" placeholder=\"000000\">"
                         "<label>New password</label><input type=\"password\" name=\"password\" required "
                         "maxlength=\"64\">"
                         "<label>Confirm new password</label><input type=\"password\" name=\"password2\" "
                         "required maxlength=\"64\">"
                         "<button>Save password</button></form>"
                         "<div class=\"row\"><a href=\"/player/login/dashboard\">Back to login</a></div>");
}

std::string messagePage(const std::string& title, const std::string& message, const std::string& link,
                        const std::string& linkText)
{
    return pageShell("WildanDev GTPS — " + title,
                     "<h1>" + title + "</h1><div class=\"msg\">" + message + "</div>"
                     "<div class=\"row\"><a href=\"" + link + "\">" + linkText + "</a></div>");
}

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

bool validGrowId(const std::string& growId)
{
    if (growId.empty() || growId.size() > 32)
        return false;
    for (char c : growId)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    }
    return true;
}

bool validPassword(const std::string& password)
{
    if (password.empty() || password.size() > 64)
        return false;
    for (char c : password)
    {
        if (c == '&' || static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E)
            return false;
    }
    return true;
}

bool validEmail(const std::string& email)
{
    if (email.empty())
        return true; // optional
    if (email.size() > 255)
        return false;
    for (char c : email)
    {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '@' && c != '.' && c != '_' && c != '-' &&
            c != '+' && c != '%')
            return false;
    }
    return email.find('@') != std::string::npos && email.find('@') == email.rfind('@');
}

bool validResetCode(const std::string& code)
{
    if (code.size() != 6)
        return false;
    for (char c : code)
    {
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// JSON / HTTP helpers
// ---------------------------------------------------------------------------

std::string jsonEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text)
    {
        if (c == '"' || c == '\\')
        {
            out.push_back('\\');
            out.push_back(c);
        }
        else if (static_cast<unsigned char>(c) < 0x20)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        }
        else
        {
            out.push_back(c);
        }
    }
    return out;
}

std::string urlDecode(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '+')
        {
            out.push_back(' ');
        }
        else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])));
            i += 2;
        }
        else
        {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parseForm(const std::string& body)
{
    std::unordered_map<std::string, std::string> fields;
    std::size_t start = 0;
    while (start <= body.size())
    {
        std::size_t end = body.find('&', start);
        if (end == std::string::npos)
            end = body.size();
        std::string pair = body.substr(start, end - start);
        if (!pair.empty())
        {
            std::size_t eq = pair.find('=');
            if (eq == std::string::npos)
                fields[urlDecode(pair)] = "";
            else
                fields[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
        }
        if (end == body.size())
            break;
        start = end + 1;
    }
    return fields;
}

std::string readFileOrEmpty(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return "";
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

struct HttpRequest
{
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

bool readRequest(SSL* ssl, HttpRequest& request)
{
    std::string raw;
    char buffer[4096];
    std::size_t headerEnd = std::string::npos;
    while (raw.size() < kMaxRequestBytes)
    {
        int n = SSL_read(ssl, buffer, static_cast<int>(sizeof(buffer)));
        if (n <= 0)
            return false;
        raw.append(buffer, static_cast<std::size_t>(n));
        headerEnd = raw.find("\r\n\r\n");
        if (headerEnd != std::string::npos)
        {
            std::size_t want = headerEnd + 4;
            std::string lower;
            lower.reserve(headerEnd);
            for (char c : raw.substr(0, headerEnd))
                lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            auto at = lower.find("content-length:");
            if (at != std::string::npos)
            {
                long length = std::strtol(raw.c_str() + at + 15, nullptr, 10);
                if (length < 0 || static_cast<std::size_t>(length) > kMaxBodyBytes)
                    return false;
                want += static_cast<std::size_t>(length);
            }
            if (raw.size() >= want)
                break;
            headerEnd = std::string::npos;
        }
    }
    if (headerEnd == std::string::npos)
        return false;

    std::istringstream head(raw.substr(0, headerEnd));
    std::string line;
    std::getline(head, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    auto space1 = line.find(' ');
    auto space2 = line.find(' ', space1 + 1);
    if (space1 == std::string::npos || space2 == std::string::npos)
        return false;
    request.method = line.substr(0, space1);
    request.path = line.substr(space1 + 1, space2 - space1 - 1);
    auto query = request.path.find('?');
    if (query != std::string::npos)
        request.path = request.path.substr(0, query);

    std::string headerLine;
    while (std::getline(head, headerLine))
    {
        if (!headerLine.empty() && headerLine.back() == '\r')
            headerLine.pop_back();
        auto colon = headerLine.find(':');
        if (colon == std::string::npos)
            continue;
        std::string key = headerLine.substr(0, colon);
        for (char& c : key)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::size_t valueStart = colon + 1;
        while (valueStart < headerLine.size() && headerLine[valueStart] == ' ')
            ++valueStart;
        request.headers[key] = headerLine.substr(valueStart);
    }

    request.body = raw.substr(headerEnd + 4);
    return true;
}

bool sendResponse(SSL* ssl, int code, const std::string& reason, const std::string& contentType,
                  const std::string& body, const std::vector<std::string>& extraHeaders = {})
{
    std::ostringstream out;
    out << "HTTP/1.1 " << code << " " << reason << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n";
    for (const auto& header : extraHeaders)
        out << header << "\r\n";
    out << "\r\n"
        << body;
    std::string response = out.str();
    int written = SSL_write(ssl, response.data(), static_cast<int>(response.size()));
    return written == static_cast<int>(response.size());
}

// ---------------------------------------------------------------------------
// Resend.com email delivery
// ---------------------------------------------------------------------------

bool resendSendEmail(const std::string& apiKey, const std::string& from, const std::string& to,
                     const std::string& subject, const std::string& html)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo("api.resend.com", "443", &hints, &resolved) != 0 || resolved == nullptr)
    {
        logError("Resend: DNS resolve failed");
        return false;
    }

    int fd = -1;
    for (addrinfo* it = resolved; it != nullptr; it = it->ai_next)
    {
        fd = static_cast<int>(socket(it->ai_family, it->ai_socktype, it->ai_protocol));
        if (fd < 0)
            continue;
        timeval timeout{};
        timeout.tv_sec = 10;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(resolved);
    if (fd < 0)
    {
        logError("Resend: connect failed");
        return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr)
    {
        close(fd);
        return false;
    }
    SSL_CTX_set_default_verify_paths(ctx);
    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "api.resend.com");
    SSL_set1_host(ssl, "api.resend.com");
    if (SSL_connect(ssl) != 1)
    {
        logError("Resend: TLS connect failed");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return false;
    }

    std::string payload = "{\"from\":\"" + jsonEscape(from) + "\",\"to\":[\"" + jsonEscape(to) +
                          "\"],\"subject\":\"" + jsonEscape(subject) + "\",\"html\":\"" + jsonEscape(html) +
                          "\"}";
    std::string request = "POST /emails HTTP/1.1\r\n"
                          "Host: api.resend.com\r\n"
                          "Authorization: Bearer " + apiKey + "\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(payload.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + payload;
    bool sent = SSL_write(ssl, request.data(), static_cast<int>(request.size())) ==
                static_cast<int>(request.size());

    // Read the status line only; the rest does not matter for delivery.
    bool accepted = false;
    if (sent)
    {
        char buffer[512] = {};
        int n = SSL_read(ssl, buffer, sizeof(buffer) - 1);
        if (n > 0)
        {
            buffer[n] = 0;
            accepted = std::strncmp(buffer, "HTTP/1.1 2", 10) == 0 ||
                       std::strncmp(buffer, "HTTP/1.0 2", 10) == 0;
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    if (!accepted)
        logError("Resend: email rejected by API (check resend_api_key / sender address)");
    return accepted;
}

std::string resetCodeEmailHtml(const std::string& code)
{
    return pageShell(
        "Password reset", "<h1>Password reset</h1>"
                          "<p class=\"sub\">Use this code to set a new password. It expires in 15 minutes.</p>"
                          "<div class=\"code\">" + code + "</div>"
                          "<p class=\"hint\">If you did not request this, you can ignore this email — your "
                          "password stays unchanged.</p>");
}

std::string generateResetCode()
{
    unsigned char bytes[4] = {};
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return "";
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    value %= 1000000;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", value);
    return buf;
}

} // namespace

LoginService::~LoginService()
{
    stop();
}

bool LoginService::start(const LoginConfig& config)
{
    m_config = config;

    if (!m_database.connect(config.db))
    {
        logError("Login service: cannot connect to the database (register/recovery disabled)");
        m_database.reset();
    }

    if (readFileOrEmpty(config.certFile).empty() || readFileOrEmpty(config.keyFile).empty())
    {
        logWarn("Login service disabled: missing " + config.certFile + " / " + config.keyFile);
        return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr)
    {
        logError("Login service: SSL_CTX_new failed");
        return false;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, config.certFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, config.keyFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1)
    {
        logError("Login service: cannot load certificate/key");
        SSL_CTX_free(ctx);
        return false;
    }

    int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0)
    {
        logError("Login service: socket() failed");
        SSL_CTX_free(ctx);
        return false;
    }
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(config.port));
    if (config.host.empty() || config.host == "0.0.0.0")
    {
        address.sin_addr.s_addr = INADDR_ANY;
    }
    else if (inet_pton(AF_INET, config.host.c_str(), &address.sin_addr) != 1)
    {
        logError("Login service: invalid login_host " + config.host);
        close(fd);
        SSL_CTX_free(ctx);
        return false;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 16) != 0)
    {
        logError("Login service: bind/listen failed on " + config.host + ":" + std::to_string(config.port) +
                 " (" + std::strerror(errno) + ")");
        close(fd);
        SSL_CTX_free(ctx);
        return false;
    }

    m_listenFd = fd;
    m_running = true;
    m_thread = std::thread([this, ctx]() { serve(ctx); });
    logInfo("Login service listening on https://" + config.host + ":" + std::to_string(config.port));
    return true;
}

void LoginService::serve(ssl_ctx_st* ctxPtr)
{
    SSL_CTX* ctx = reinterpret_cast<SSL_CTX*>(ctxPtr);
    while (m_running)
    {
        sockaddr_in client{};
        socklen_t clientLen = sizeof(client);
        int fd = accept(m_listenFd, reinterpret_cast<sockaddr*>(&client), &clientLen);
        if (fd < 0)
        {
            if (!m_running)
                break;
            continue;
        }
        timeval timeout{};
        timeout.tv_sec = 30;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        SSL* ssl = SSL_new(ctx);
        if (ssl == nullptr)
        {
            close(fd);
            continue;
        }
        SSL_set_fd(ssl, fd);
        std::thread([ssl, this]() {
            if (SSL_accept(ssl) == 1)
                this->handleConnection(ssl);
            else
            {
                SSL_free(ssl);
            }
        }).detach();
    }
    // Deliberately no SSL_CTX_free: detached connection threads may still
    // hold SSL objects derived from it; stop() only runs at shutdown.
}

void LoginService::handleConnection(SSL* ssl)
{
    HttpRequest request;
    if (!readRequest(ssl, request))
    {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        return;
    }

    const std::string& path = request.path;
    bool isPost = request.method == "POST" || request.method == "PUT";

    if (path == "/" && !isPost)
    {
        sendResponse(ssl, 200, "OK", "text/plain", "WildanDev login ok");
    }
    else if (path == "/favicon.ico" && !isPost)
    {
        sendResponse(ssl, 200, "OK", "image/png",
                     std::string(reinterpret_cast<const char*>(kFaviconPng), kFaviconPngSize));
    }
    else if (path.size() >= 26 && path.compare(path.size() - 26, 26, "/growtopia/server_data.php") == 0)
    {
        std::string content = readFileOrEmpty(m_config.resourcesDir + "/server_data.txt");
        if (content.empty())
            sendResponse(ssl, 500, "Internal Server Error", "text/plain", "server_data unavailable");
        else
            sendResponse(ssl, 200, "OK", "text/plain", content);
    }
    else if (path.rfind("/player/login/dashboard", 0) == 0)
    {
        // The Growtopia client reaches the dashboard with both GET (page
        // load) and POST (form submit). On POST the client expects the page
        // re-rendered with the first form field key as the token.
        std::string tokenSource = "proto=225";
        if (isPost && !request.body.empty())
        {
            tokenSource = urlDecode(request.body.substr(0, request.body.find('&')));
            if (tokenSource.find('=') != std::string::npos && tokenSource.rfind("proto", 0) != 0)
                tokenSource = tokenSource.substr(0, tokenSource.find('='));
        }
        sendResponse(ssl, 200, "OK", "text/html", loginPage(Base64::encode(tokenSource), "", false));
    }
    else if (path == "/player/register" && !isPost)
    {
        sendResponse(ssl, 200, "OK", "text/html", registerPage("", false));
    }
    else if (path == "/player/forgot" && !isPost)
    {
        sendResponse(ssl, 200, "OK", "text/html", forgotPage("", false));
    }
    else if (path == "/player/reset" && !isPost)
    {
        sendResponse(ssl, 200, "OK", "text/html", resetPage("", "", false));
    }
    else if (path == "/player/growid/register" && isPost)
    {
        auto fields = parseForm(request.body);
        std::string grow = fields.count("growId") ? fields["growId"] : "";
        std::string password = fields.count("password") ? fields["password"] : "";
        std::string password2 = fields.count("password2") ? fields["password2"] : "";
        std::string email = fields.count("email") ? fields["email"] : "";

        const char* error = nullptr;
        if (!validGrowId(grow))
            error = "Username must be 1-32 letters, digits or underscore.";
        else if (!validPassword(password))
            error = "Password must be 1-64 printable characters, no '&'.";
        else if (password != password2)
            error = "Passwords do not match.";
        else if (!validEmail(email))
            error = "That email address does not look valid.";

        if (error != nullptr)
        {
            sendResponse(ssl, 200, "OK", "text/html", registerPage(error, true));
        }
        else
        {
            std::lock_guard<std::mutex> guard(m_dbMutex);
            auto result = m_database.registerPlayer(grow, password, email, m_config.defaultRoleId);
            if (result == Database::RegisterResult::Duplicate)
                sendResponse(ssl, 200, "OK", "text/html",
                             registerPage("That username is already taken.", true));
            else if (result != Database::RegisterResult::Ok)
                sendResponse(ssl, 200, "OK", "text/html",
                             registerPage("Could not create the account, try again.", true));
            else
                sendResponse(ssl, 200, "OK", "text/html",
                             messagePage("Account created", "Account <b>" + grow + "</b> is ready. Log in "
                                                                        "with it in the game.",
                                         "/player/login/dashboard", "Go to login"));
        }
    }
    else if (path == "/player/growid/forgot" && isPost)
    {
        auto fields = parseForm(request.body);
        std::string grow = fields.count("growId") ? fields["growId"] : "";

        bool sent = false;
        std::string failure;
        if (!validGrowId(grow))
        {
            failure = "Invalid username.";
        }
        else
        {
            {
                std::lock_guard<std::mutex> guard(g_throttleMutex);
                auto now = std::chrono::steady_clock::now();
                auto& last = g_resetThrottle[grow];
                if (now - last < std::chrono::seconds(30))
                {
                    failure = "Please wait 30 seconds before requesting another code.";
                }
                else
                {
                    last = now;
                }
            }
            if (failure.empty())
            {
                std::pair<uint32_t, std::string> contact;
                bool found = false;
                {
                    std::lock_guard<std::mutex> guard(m_dbMutex);
                    if (auto row = m_database.getPlayerContact(grow))
                    {
                        contact = *row;
                        found = true;
                    }
                }
                std::string code;
                if (found && !contact.second.empty() && !m_config.resendApiKey.empty() &&
                    !(code = generateResetCode()).empty())
                {
                    if (m_database.createResetCode(contact.first, code, kResetCodeTtlSeconds))
                    {
                        uint32_t playerId = contact.first;
                        std::string email = contact.second;
                        std::string html = resetCodeEmailHtml(code);
                        std::string from = m_config.resendFrom;
                        std::string apiKey = m_config.resendApiKey;
                        std::thread([apiKey, from, email, html, playerId]() {
                            if (resendSendEmail(apiKey, from, email,
                                                "WildanDev GTPS — password reset code", html))
                                logInfo("Password reset code emailed to player " + std::to_string(playerId));
                        }).detach();
                        sent = true;
                    }
                    else
                    {
                        failure = "Could not start the reset, try again.";
                    }
                }
                else if (found && !contact.second.empty() && m_config.resendApiKey.empty())
                {
                    failure = "Email delivery is not configured on this server (resend_api_key missing).";
                }
            }
        }
        if (!failure.empty())
            sendResponse(ssl, 200, "OK", "text/html", forgotPage(failure, true));
        else
            sendResponse(ssl, 200, "OK", "text/html",
                         messagePage("Check your email",
                                     sent ? "If your account has an email on file, a 6-digit code is on its "
                                            "way. It expires in 15 minutes."
                                          : "If your account has an email on file, a 6-digit code is on its "
                                            "way. It expires in 15 minutes.",
                                     "/player/reset", "Enter the code"));
    }
    else if (path == "/player/growid/reset" && isPost)
    {
        auto fields = parseForm(request.body);
        std::string grow = fields.count("growId") ? fields["growId"] : "";
        std::string code = fields.count("code") ? fields["code"] : "";
        std::string password = fields.count("password") ? fields["password"] : "";
        std::string password2 = fields.count("password2") ? fields["password2"] : "";

        const char* error = nullptr;
        if (!validGrowId(grow))
            error = "Invalid username.";
        else if (!validResetCode(code))
            error = "The code must be 6 digits.";
        else if (!validPassword(password))
            error = "Password must be 1-64 printable characters, no '&'.";
        else if (password != password2)
            error = "Passwords do not match.";

        if (error != nullptr)
        {
            sendResponse(ssl, 200, "OK", "text/html", resetPage(grow, error, true));
        }
        else
        {
            uint32_t playerId = 0;
            bool found = false;
            {
                std::lock_guard<std::mutex> guard(m_dbMutex);
                if (auto row = m_database.getPlayerContact(grow))
                {
                    playerId = row->first;
                    found = true;
                }
                if (found && m_database.consumeResetCode(playerId, code))
                {
                    m_database.updatePassword(playerId, password);
                    sendResponse(ssl, 200, "OK", "text/html",
                                 messagePage("Password updated",
                                             "Your password has been changed. Log in with the new one.",
                                             "/player/login/dashboard", "Go to login"));
                }
                else
                {
                    sendResponse(ssl, 200, "OK", "text/html",
                                 resetPage(grow, "That code is wrong or expired.", true));
                }
            }
        }
    }
    else if (path == "/player/growid/login/validate" && isPost)
    {
        auto fields = parseForm(request.body);
        std::string grow = fields.count("growId") ? fields["growId"] : "";
        std::string password = fields.count("password") ? fields["password"] : "";
        std::string token = fields.count("_token") ? fields["_token"] : "";
        if (grow.empty() || password.empty())
        {
            sendResponse(ssl, 400, "Bad Request", "application/json",
                         "{\"status\":\"error\",\"message\":\"growId/password required\"}");
        }
        else if (!validGrowId(grow))
        {
            sendResponse(ssl, 400, "Bad Request", "application/json",
                         "{\"status\":\"error\",\"message\":\"GrowID: 1-32 letters, digits or "
                         "underscore\"}");
        }
        else if (!validPassword(password))
        {
            sendResponse(ssl, 400, "Bad Request", "application/json",
                         "{\"status\":\"error\",\"message\":\"Password: 1-64 printable characters, "
                         "no '&'\"}");
        }
        else
        {
            std::string account =
                Base64::encode("_token=" + token + "&growId=" + grow + "&password=" + password);
            {
                std::lock_guard<std::mutex> guard(g_issuedMutex);
                g_issuedTokens[account] = {grow, password};
            }
            std::string body = "{\"status\":\"success\",\"message\":\"Account Validated.\",\"token\":\"" +
                               jsonEscape(account) + "\",\"url\":\"\",\"accountType\":\"growtopia\"}";
            sendResponse(ssl, 200, "OK", "application/json", body,
                         {"Set-Cookie: gtps_token=" + account + "; Path=/; HttpOnly; Secure",
                          "Set-Cookie: growId=" + grow + "; Path=/; HttpOnly; Secure"});
        }
    }
    else if (path == "/player/growid/checktoken" && isPost)
    {
        std::ostringstream out;
        out << "HTTP/1.1 307 Temporary Redirect\r\n"
            << "Location: /player/growid/validate/checktoken\r\n"
            << "Content-Length: 0\r\n"
            << "Connection: close\r\n\r\n";
        SSL_write(ssl, out.str().data(), static_cast<int>(out.str().size()));
    }
    else if (path == "/player/growid/validate/checktoken" && isPost)
    {
        auto fields = parseForm(request.body);
        auto pick = [&fields](std::initializer_list<const char*> names) {
            for (const char* name : names)
            {
                auto it = fields.find(name);
                if (it != fields.end() && !it->second.empty())
                    return it->second;
            }
            return std::string();
        };
        std::string refresh = pick({"refreshToken", "refresh_token", "token"});
        std::string clientData = pick({"clientData", "client_data", "_token"});
        std::pair<std::string, std::string> known;
        bool knownToken = false;
        {
            std::lock_guard<std::mutex> guard(g_issuedMutex);
            auto it = g_issuedTokens.find(refresh);
            if (it != g_issuedTokens.end())
            {
                known = it->second;
                knownToken = true;
            }
        }
        if (!knownToken)
        {
            sendResponse(ssl, 401, "Unauthorized", "application/json",
                         "{\"status\":\"error\",\"message\":\"invalid refresh token\"}");
        }
        else
        {
            std::string account = Base64::encode("_token=" + clientData + "&growId=" + known.first +
                                                 "&password=" + known.second);
            {
                std::lock_guard<std::mutex> guard(g_issuedMutex);
                g_issuedTokens[account] = known;
            }
            sendResponse(ssl, 200, "OK", "application/json",
                         "{\"status\":\"success\",\"message\":\"Token is valid.\",\"token\":\"" +
                             jsonEscape(account) + "\",\"url\":\"\",\"accountType\":\"growtopia\"}");
        }
    }
    else
    {
        sendResponse(ssl, 404, "Not Found", "text/plain", "not found");
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
}

void LoginService::stop()
{
    if (!m_running.exchange(false))
        return;
    if (m_listenFd >= 0)
    {
        shutdown(m_listenFd, SHUT_RDWR);
        close(m_listenFd);
        m_listenFd = -1;
    }
    if (m_thread.joinable())
        m_thread.join();
}

} // namespace WildanDev
