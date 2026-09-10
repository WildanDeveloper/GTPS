#include "login.hpp"
#include "base64.hpp"
#include "logger.hpp"

#include <openssl/ssl.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
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

// Tokens issued by this process. A refresh (checktoken) must present one of
// these; anything else is rejected instead of blindly re-encoding whatever
// the client posted.
std::mutex g_issuedMutex;
std::unordered_map<std::string, std::pair<std::string, std::string>> g_issuedTokens;

const unsigned char kFaviconPng[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
    0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
    0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49,
    0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};
constexpr std::size_t kFaviconPngSize = sizeof(kFaviconPng);

const char* kLoginPage =
    "<!doctype html><html><head><meta charset=\"utf-8\">\n"
    "<title>WildanDev GTPS login</title></head><body>\n"
    "<h2>WildanDev GTPS</h2>\n"
    "<form method=\"POST\" action=\"/player/growid/login/validate\">\n"
    "<input type=\"hidden\" name=\"_token\" value=\"__TOKEN__\">\n"
    "<label>GrowID: <input name=\"growId\" required></label><br>\n"
    "<label>Password: <input type=\"password\" name=\"password\" required></label><br>\n"
    "<button type=\"submit\">Login / Register</button>\n"
    "</form>\n"
    "</body></html>";

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
            out += "\\u";
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%04x", c);
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
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '+')
        {
            out.push_back(' ');
        }
        else if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(text[i + 2])))
        {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
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

// Form parsing: splits "a=1&b=2", URL-decodes keys and values.
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
            {
                fields[urlDecode(pair)] = "";
            }
            else
            {
                fields[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
            }
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
    // Strip any query string for routing purposes.
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

void handleConnection(SSL* ssl, const std::string& resourcesDir)
{
    HttpRequest request;
    if (readRequest(ssl, request))
    {
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
            std::string content = readFileOrEmpty(resourcesDir + "/server_data.txt");
            if (content.empty())
                sendResponse(ssl, 500, "Internal Server Error", "text/plain", "server_data unavailable");
            else
                sendResponse(ssl, 200, "OK", "text/plain", content);
        }
        else if (path.rfind("/player/login/dashboard", 0) == 0)
        {
            std::string tokenSource = "proto=225";
            if (isPost && !request.body.empty())
            {
                tokenSource = urlDecode(request.body.substr(0, request.body.find('&')));
                if (tokenSource.find('=') != std::string::npos &&
                    tokenSource.rfind("proto", 0) != 0)
                {
                    tokenSource = tokenSource.substr(0, tokenSource.find('='));
                }
            }
            std::string page = kLoginPage;
            page.replace(page.find("__TOKEN__"), 9, Base64::encode(tokenSource));
            sendResponse(ssl, 200, "OK", "text/html", page);
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
            {
                std::lock_guard<std::mutex> guard(g_issuedMutex);
                auto it = g_issuedTokens.find(refresh);
                if (it == g_issuedTokens.end())
                {
                    sendResponse(ssl, 401, "Unauthorized", "application/json",
                                 "{\"status\":\"error\",\"message\":\"invalid refresh token\"}");
                    SSL_shutdown(ssl);
                    SSL_free(ssl);
                    return;
                }
                known = it->second;
            }
            std::string account =
                Base64::encode("_token=" + clientData + "&growId=" + known.first + "&password=" + known.second);
            {
                std::lock_guard<std::mutex> guard(g_issuedMutex);
                g_issuedTokens[account] = known;
            }
            sendResponse(ssl, 200, "OK", "application/json",
                         "{\"status\":\"success\",\"message\":\"Token is valid.\",\"token\":\"" +
                             jsonEscape(account) + "\",\"url\":\"\",\"accountType\":\"growtopia\"}");
        }
        else
        {
            sendResponse(ssl, 404, "Not Found", "text/plain", "not found");
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

} // namespace

LoginService::~LoginService()
{
    stop();
}

bool LoginService::start(const std::string& resourcesDir, const std::string& certFile, const std::string& keyFile,
                         const std::string& host, int port)
{
    std::string cert = readFileOrEmpty(certFile);
    std::string key = readFileOrEmpty(keyFile);
    if (cert.empty() || key.empty())
    {
        logWarn("Login service disabled: missing " + certFile + " / " + keyFile);
        return false;
    }

    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr)
    {
        logError("Login service: SSL_CTX_new failed");
        return false;
    }
    if (SSL_CTX_use_certificate_chain_file(ctx, certFile.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, keyFile.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1)
    {
        logError("Login service: cannot load certificate/key");
        SSL_CTX_free(ctx);
        return false;
    }

    m_resourcesDir = resourcesDir;

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
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (host.empty() || host == "0.0.0.0")
    {
        address.sin_addr.s_addr = INADDR_ANY;
    }
    else if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1)
    {
        logError("Login service: invalid login_host " + host);
        close(fd);
        SSL_CTX_free(ctx);
        return false;
    }
    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 16) != 0)
    {
        logError("Login service: bind/listen failed on " + host + ":" + std::to_string(port) + " (" +
                 std::strerror(errno) + ")");
        close(fd);
        SSL_CTX_free(ctx);
        return false;
    }

    m_listenFd = fd;
    m_running = true;
    m_thread = std::thread([this, ctx]() { this->serve(ctx); });
    logInfo("Login service listening on https://" + host + ":" + std::to_string(port));
    return true;
}

void LoginService::serve(SSL_CTX* ctx)
{
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
        // Slow-loris guard: stalled clients are dropped after 30s.
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
                handleConnection(ssl, m_resourcesDir);
            else
            {
                SSL_free(ssl);
            }
        }).detach();
    }
    // Deliberately do not SSL_CTX_free here: detached connection threads may
    // still hold SSL objects derived from it, and stop() only runs at
    // process shutdown.
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
