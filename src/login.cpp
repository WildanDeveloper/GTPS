#include "login.hpp"
#include "base64.hpp"
#include "logger.hpp"

#include <openssl/ssl.h>

#include <netdb.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <openssl/rand.h>
#include <sstream>
#include <unordered_map>

#include "items.hpp"

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
// Per-IP budget: reset emails sent in the last hour.
std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> g_mailBudget;

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

// Design system: navy base, orange primary, blue secondary (matches the
// SentinelX / AIGet look).
const char* kPageCss =
    ":root{--bg:#05070d;--bg2:#070b14;--surface:#0a101d;--line:rgba(148,163,184,.12);"
    "--line2:rgba(148,163,184,.26);--text:#edf1f9;--muted:#94a3b8;--dim:#5c6b82;"
    "--orange:#f97316;--orange2:#fb923c;--grad:linear-gradient(96deg,#3b82f6 0%,#60a5fa 34%,#fb923c 78%,#f97316 100%);"
    "--shadow-lg:0 24px 70px rgba(2,6,16,.65)}\n"
    "*{box-sizing:border-box;margin:0;padding:0}\n"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;padding:40px 18px;"
    "background:var(--bg2);color:var(--text);font-family:Inter,-apple-system,BlinkMacSystemFont,'Segoe UI',"
    "Roboto,sans-serif;font-size:15px;line-height:1.6;-webkit-font-smoothing:antialiased}\n"
    "body::before{content:'';position:fixed;inset:0;"
    "background:radial-gradient(420px 300px at 20% 0%,rgba(59,130,246,.13),transparent 60%),"
    "radial-gradient(420px 300px at 85% 100%,rgba(249,115,22,.11),transparent 60%)}\n"
    ".card{position:relative;width:100%;max-width:410px;background:var(--surface);"
    "border:1px solid var(--line2);border-radius:18px;padding:30px 28px;box-shadow:var(--shadow-lg)}\n"
    "h1{font-size:1.4rem;line-height:1.15;letter-spacing:-.02em;font-weight:750;margin-bottom:4px}\n"
    ".grad{background:var(--grad);-webkit-background-clip:text;background-clip:text;color:transparent}\n"
    ".sub{color:var(--muted);font-size:14px;margin-bottom:8px}\n"
    "label{display:block;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.08em;"
    "color:var(--dim);margin:12px 0 5px}\n"
    "label span{text-transform:none;letter-spacing:0;font-weight:500}\n"
    "input{width:100%;background:var(--bg);border:1px solid var(--line2);border-radius:10px;"
    "padding:11px 14px;color:var(--text);font-size:14.5px;font-family:inherit;outline:none;"
    "transition:border-color .15s}\n"
    "input:focus{border-color:var(--orange)}\n"
    "button{width:100%;margin-top:18px;padding:12px;border:0;border-radius:11px;"
    "background:linear-gradient(95deg,var(--orange2),var(--orange));color:#1a0d02;font-size:14.5px;"
    "font-weight:700;cursor:pointer;font-family:inherit;"
    "transition:transform .16s ease,box-shadow .16s ease,filter .16s ease}\n"
    "button:hover{transform:translateY(-2px);box-shadow:0 8px 28px rgba(249,115,22,.42);"
    "filter:brightness(1.06)}\n"
    ".row{display:flex;justify-content:space-between;margin-top:16px;font-size:13px}\n"
    "a{color:var(--orange2);text-decoration:none;font-weight:600}\n"
    "a:hover{text-decoration:underline}\n"
    ".msg{background:rgba(59,130,246,.08);border:1px solid rgba(96,165,250,.25);color:#bcd4f8;"
    "border-radius:10px;padding:10px 12px;font-size:13px;margin-bottom:10px}\n"
    ".err{background:rgba(248,113,113,.08);border-color:rgba(248,113,113,.3);color:#f8b4b4}\n"
    ".code{font-family:'JetBrains Mono',ui-monospace,monospace;font-size:34px;font-weight:700;"
    "letter-spacing:10px;background:var(--grad);-webkit-background-clip:text;background-clip:text;"
    "color:transparent;text-align:center;padding:16px 0 8px}\n"
    ".hint{font-size:12px;color:var(--dim);margin-top:12px;line-height:1.55}\n"
    ".gbtn{display:flex;align-items:center;justify-content:center;gap:10px;width:100%;margin-top:14px;"
    "padding:11px;border:1px solid var(--line2);border-radius:11px;background:#fff;color:#1f2937;"
    "font-size:14px;font-weight:600;transition:transform .16s ease,filter .16s ease}\n"
    ".gbtn:hover{transform:translateY(-2px);filter:brightness(1.03);text-decoration:none}\n"
    ".sep{display:flex;align-items:center;gap:12px;color:var(--dim);font-size:12px;margin-top:16px}\n"
    ".sep::before,.sep::after{content:'';flex:1;height:1px;background:var(--line)}\n";

std::string pageShell(const std::string& title, const std::string& body)
{
    return "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
           "<title>" + title + "</title>"
           "<link rel=\"preconnect\" href=\"https://fonts.googleapis.com\">"
           "<link rel=\"preconnect\" href=\"https://fonts.gstatic.com\" crossorigin>"
           "<link href=\"https://fonts.googleapis.com/css2?family=Inter:wght@400;600;700;800&"
           "family=JetBrains+Mono:wght@500;700&display=swap\" rel=\"stylesheet\">"
           "<style>" + kPageCss + "</style></head><body>"
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

std::string loginPage(const std::string& token, const std::string& message, bool isError,
                      bool googleEnabled)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    std::string page = kLoginPage;
    page.replace(page.find("__TOKEN__"), 9, token);
    std::string google = googleEnabled
                             ? "<div class=\"sep\"><span>or</span></div>"
                               "<a class=\"gbtn\" href=\"/player/auth/google\">"
                               "<svg width=\"18\" height=\"18\" viewBox=\"0 0 48 48\"><path fill=\"#EA4335\" d=\"M24 9.5c3.54 0 6.71 1.22 9.21 3.6l6.85-6.85C35.9 2.38 30.47 0 24 0 14.62 0 6.51 5.38 2.56 13.22l7.98 6.19C12.43 13.72 17.74 9.5 24 9.5z\"/><path fill=\"#4285F4\" d=\"M46.98 24.55c0-1.57-.15-3.09-.38-4.55H24v9.02h12.94c-.58 2.96-2.26 5.48-4.78 7.18l7.73 6c4.51-4.18 7.09-10.36 7.09-17.65z\"/><path fill=\"#FBBC05\" d=\"M10.53 28.59c-.48-1.45-.76-2.99-.76-4.59s.27-3.14.76-4.59l-7.98-6.19C.92 16.46 0 20.12 0 24c0 3.88.92 7.54 2.56 10.78l7.97-6.19z\"/><path fill=\"#34A853\" d=\"M24 48c6.48 0 11.93-2.13 15.89-5.81l-7.73-6c-2.15 1.45-4.92 2.3-8.16 2.3-6.26 0-11.57-4.22-13.47-9.91l-7.98 6.19C6.51 42.62 14.62 48 24 48z\"/></svg>"
                               "Continue with Google</a>"
                             : "";
    return pageShell("WildanDev GTPS — Login",
                     "<h1 class=\"grad\">WildanDev GTPS</h1><p class=\"sub\">Log in with your account.</p>" + msg + page +
                         google +
                         "<div class=\"row\"><a href=\"/player/forgot\">Forgot password?</a>"
                         "<a href=\"/player/register\">Create account</a></div>");
}

std::string registerPage(const std::string& message, bool isError, bool googleEnabled)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    std::string google = googleEnabled
                             ? "<div class=\"sep\"><span>or</span></div>"
                               "<a class=\"gbtn\" href=\"/player/auth/google\">"
                               "<svg width=\"18\" height=\"18\" viewBox=\"0 0 48 48\"><path fill=\"#EA4335\" d=\"M24 9.5c3.54 0 6.71 1.22 9.21 3.6l6.85-6.85C35.9 2.38 30.47 0 24 0 14.62 0 6.51 5.38 2.56 13.22l7.98 6.19C12.43 13.72 17.74 9.5 24 9.5z\"/><path fill=\"#4285F4\" d=\"M46.98 24.55c0-1.57-.15-3.09-.38-4.55H24v9.02h12.94c-.58 2.96-2.26 5.48-4.78 7.18l7.73 6c4.51-4.18 7.09-10.36 7.09-17.65z\"/><path fill=\"#FBBC05\" d=\"M10.53 28.59c-.48-1.45-.76-2.99-.76-4.59s.27-3.14.76-4.59l-7.98-6.19C.92 16.46 0 20.12 0 24c0 3.88.92 7.54 2.56 10.78l7.97-6.19z\"/><path fill=\"#34A853\" d=\"M24 48c6.48 0 11.93-2.13 15.89-5.81l-7.73-6c-2.15 1.45-4.92 2.3-8.16 2.3-6.26 0-11.57-4.22-13.47-9.91l-7.98 6.19C6.51 42.62 14.62 48 24 48z\"/></svg>"
                               "Continue with Google</a>"
                             : "";
    return pageShell("WildanDev GTPS — Register",
                     "<h1 class=\"grad\">Create account</h1><p class=\"sub\">Pick a username and password.</p>" + msg +
                         "<form method=\"POST\" action=\"/player/growid/register\">"
                         "<label>Username</label><input name=\"growId\" required maxlength=\"32\" "
                         "pattern=\"[A-Za-z0-9_]{1,32}\">"
                         "<label>Password</label><input type=\"password\" name=\"password\" required "
                         "maxlength=\"64\">"
                         "<label>Confirm password</label><input type=\"password\" name=\"password2\" required "
                         "maxlength=\"64\">"
                         "<label>Email <span style=\"color:#6f8a71\">(optional — for password reset)</span>"
                         "</label><input type=\"email\" name=\"email\" maxlength=\"255\">"
                         "<button>Create account</button></form>" + google +
                         "<div class=\"row\"><a href=\"/player/login/dashboard\">Back to login</a></div>");
}

std::string forgotPage(const std::string& message, bool isError)
{
    std::string msg = message.empty()
                          ? ""
                          : "<div class=\"msg " + std::string(isError ? "err" : "") + "\">" + message + "</div>";
    return pageShell("WildanDev GTPS — Reset password",
                     "<h1 class=\"grad\">Reset password</h1>"
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
                     "<h1 class=\"grad\">Set a new password</h1>"
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
                     "<h1 class=\"grad\">" + title + "</h1><div class=\"msg\">" + message + "</div>"
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
    std::string query; // raw query string (after '?'), may be empty
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
    auto queryAt = request.path.find('?');
    if (queryAt != std::string::npos)
    {
        request.query = request.path.substr(queryAt + 1);
        request.path = request.path.substr(0, queryAt);
    }

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

// Minimal HTTPS POST client (OpenSSL). Returns true on 2xx; response body
// is copied into responseOut when non-null.
bool httpsPost(const std::string& host, const std::string& path, const std::string& body,
               const std::vector<std::string>& headers, std::string* responseOut)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    if (getaddrinfo(host.c_str(), "443", &hints, &resolved) != 0 || resolved == nullptr)
    {
        logError("HTTPS POST " + host + ": DNS resolve failed");
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
        logError("HTTPS POST " + host + ": connect failed");
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
    SSL_set_tlsext_host_name(ssl, host.c_str());
    SSL_set1_host(ssl, host.c_str());
    if (SSL_connect(ssl) != 1)
    {
        logError("HTTPS POST " + host + ": TLS connect failed");
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return false;
    }

    std::string request = "POST " + path + " HTTP/1.1\r\n"
                          "Host: " + host + "\r\n"
                          "Connection: close\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (const auto& header : headers)
        request += header + "\r\n";
    request += "\r\n" + body;

    bool sent = SSL_write(ssl, request.data(), static_cast<int>(request.size())) ==
                static_cast<int>(request.size());
    bool accepted = false;
    if (sent)
    {
        std::string response;
        char buffer[4096];
        int n;
        while (response.size() < 1024 * 1024 && (n = SSL_read(ssl, buffer, sizeof(buffer))) > 0)
            response.append(buffer, static_cast<std::size_t>(n));
        accepted = response.rfind("HTTP/1.1 2", 0) == 0 || response.rfind("HTTP/1.0 2", 0) == 0;
        if (responseOut != nullptr)
        {
            auto bodyStart = response.find("\r\n\r\n");
            if (bodyStart != std::string::npos)
                *responseOut = response.substr(bodyStart + 4);
            else
                *responseOut = response;
        }
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return accepted;
}

bool resendSendEmail(const std::string& apiKey, const std::string& from, const std::string& to,
                     const std::string& subject, const std::string& html)
{
    std::string payload = "{\"from\":\"" + jsonEscape(from) + "\",\"to\":[\"" + jsonEscape(to) +
                          "\"],\"subject\":\"" + jsonEscape(subject) + "\",\"html\":\"" + jsonEscape(html) +
                          "\"}";
    bool accepted = httpsPost("api.resend.com", "/emails", payload,
                              {"Authorization: Bearer " + apiKey, "Content-Type: application/json"}, nullptr);
    if (!accepted)
        logError("Resend: email rejected by API (check resend_api_key / sender address)");
    return accepted;
}

// Extracts "key":"value" from a flat JSON document (no nested objects in
// the values we care about).
bool jsonGetString(const std::string& json, const std::string& key, std::string& out)
{
    std::string needle = "\"" + key + "\"";
    auto at = json.find(needle);
    if (at == std::string::npos)
        return false;
    at = json.find(':', at + needle.size());
    if (at == std::string::npos)
        return false;
    ++at;
    while (at < json.size() && (json[at] == ' ' || json[at] == '\t'))
        ++at;
    if (at >= json.size() || json[at] != '"')
        return false;
    ++at;
    std::string value;
    while (at < json.size() && json[at] != '"')
    {
        if (json[at] == '\\' && at + 1 < json.size())
        {
            ++at;
            if (json[at] == 'n') value.push_back('\n');
            else value.push_back(json[at]);
        }
        else
        {
            value.push_back(json[at]);
        }
        ++at;
    }
    out = value;
    return true;
}

std::string base64UrlDecode(const std::string& input)
{
    std::string text = input;
    for (char& c : text)
    {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (text.size() % 4 != 0)
        text.push_back('=');
    auto decoded = Base64::decode(text);
    return decoded.has_value() ? decoded.value() : "";
}

std::string generateRandomPassword()
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789";
    unsigned char bytes[24] = {};
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return "";
    std::string out;
    for (unsigned char byte : bytes)
        out.push_back(alphabet[byte % (sizeof(alphabet) - 1)]);
    return out;
}

std::string generateStateToken()
{
    unsigned char bytes[16] = {};
    if (RAND_bytes(bytes, sizeof(bytes)) != 1)
        return "";
    std::ostringstream out;
    out << std::hex;
    for (unsigned char byte : bytes)
    {
        out.width(2);
        out.fill('0');
        out << static_cast<int>(byte);
    }
    return out.str();
}

// Pending OAuth states (CSRF protection), expiring after 10 minutes.
std::mutex g_stateMutex;
std::map<std::string, std::chrono::steady_clock::time_point> g_oauthStates;

// ---------------------------------------------------------------------------
// Anti-brute-force guard
// ---------------------------------------------------------------------------

struct AttemptRecord
{
    int count{0};
    std::chrono::steady_clock::time_point blockedUntil{};
    std::chrono::steady_clock::time_point lastActivity{};
};

std::mutex g_guardMutex;
std::unordered_map<std::string, AttemptRecord> g_attempts;
constexpr int kFailThreshold = 5;
constexpr long kBaseBlockSeconds = 300; // 5 minutes
constexpr long kMaxBlockSeconds = 86400; // 24 hours

namespace
{

std::chrono::steady_clock::time_point nowPoint()
{
    return std::chrono::steady_clock::now();
}

} // namespace

// Returns seconds remaining while blocked, 0 when allowed.
long guardCheck(const std::string& key)
{
    std::lock_guard<std::mutex> guard(g_guardMutex);
    auto it = g_attempts.find(key);
    if (it == g_attempts.end())
        return 0;
    long left = std::chrono::duration_cast<std::chrono::seconds>(it->second.blockedUntil - nowPoint()).count();
    return left > 0 ? left : 0;
}

// Records a failure. Returns the number of seconds the key is now blocked
// (0 = still allowed). Blocks escalate: the 5th failure blocks for 5
// minutes, every further forced attempt doubles the remaining window
// (capped at 24 hours).
long guardFail(const std::string& key)
{
    std::lock_guard<std::mutex> guard(g_guardMutex);
    auto now = nowPoint();
    AttemptRecord& record = g_attempts[key];
    record.lastActivity = now;
    record.count += 1;
    long blocked = std::chrono::duration_cast<std::chrono::seconds>(record.blockedUntil - now).count();
    if (blocked > 0)
    {
        // Forced attempts during a block extend it (escalation, capped).
        long extended = std::min(kMaxBlockSeconds, blocked + kBaseBlockSeconds);
        record.blockedUntil = now + std::chrono::seconds(extended);
        return extended;
    }
    if (record.count >= kFailThreshold)
    {
        long duration = std::min(kMaxBlockSeconds,
                                 kBaseBlockSeconds << std::min(12, record.count - kFailThreshold));
        record.blockedUntil = now + std::chrono::seconds(duration);
        return duration;
    }
    return 0;
}

void guardClear(const std::string& key)
{
    std::lock_guard<std::mutex> guard(g_guardMutex);
    g_attempts.erase(key);
}

void guardPrune()
{
    std::lock_guard<std::mutex> guard(g_guardMutex);
    auto now = nowPoint();
    for (auto it = g_attempts.begin(); it != g_attempts.end();)
    {
        // Forget entries idle for over an hour that are not actively
        // blocking (fresh failure counters must survive!).
        bool idle = now - it->second.lastActivity > std::chrono::hours(1);
        if (idle && it->second.blockedUntil < now)
            it = g_attempts.erase(it);
        else
            ++it;
    }
}

// Resolved client IP: behind a local nginx proxy the forwarded header is
// trusted; direct connections use the socket address.
std::string resolveClientIp(const HttpRequest& request, const std::string& socketIp)
{
    if (socketIp == "127.0.0.1")
    {
        auto real = request.headers.find("x-real-ip");
        if (real != request.headers.end() && !real->second.empty())
            return real->second;
        auto forwarded = request.headers.find("x-forwarded-for");
        if (forwarded != request.headers.end() && !forwarded->second.empty())
        {
            std::string chain = forwarded->second;
            auto comma = chain.find(',');
            return comma == std::string::npos ? chain : chain.substr(0, comma);
        }
    }
    return socketIp;
}

// Device fingerprint: hash of the identifying request headers.
std::string fingerprintOf(const HttpRequest& request)
{
    std::string material = request.headers.count("user-agent") ? request.headers.at("user-agent") : "";
    material.push_back('\n');
    material += request.headers.count("accept-language") ? request.headers.at("accept-language") : "";
    material.push_back('\n');
    material += request.headers.count("accept-encoding") ? request.headers.at("accept-encoding") : "";
    std::vector<uint8_t> bytes(material.begin(), material.end());
    uint32_t hash = WildanDev::fnv1a32(bytes);
    std::ostringstream out;
    out << std::hex << std::setw(8) << std::setfill('0') << hash;
    return out.str();
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
        std::string clientIp(16, 0);
        inet_ntop(AF_INET, &client.sin_addr, clientIp.data(), clientIp.size());
        std::thread([ssl, this, clientIp]() {
            if (SSL_accept(ssl) == 1)
                this->handleConnection(ssl, clientIp);
            else
            {
                SSL_free(ssl);
            }
        }).detach();
    }
    // Deliberately no SSL_CTX_free: detached connection threads may still
    // hold SSL objects derived from it; stop() only runs at shutdown.
}

void LoginService::handleConnection(SSL* ssl, const std::string& socketIp)
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
        sendResponse(ssl, 200, "OK", "text/html",
                     loginPage(Base64::encode(tokenSource), "", false, !m_config.googleClientId.empty()));
    }
    else if (path == "/player/register" && !isPost)
    {
        sendResponse(ssl, 200, "OK", "text/html", registerPage("", false, !m_config.googleClientId.empty()));
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
        std::string ip = resolveClientIp(request, socketIp);

        // One registration per minute per IP, two accounts per IP lifetime.
        bool registerThrottled = false;
        {
            std::lock_guard<std::mutex> guard(g_throttleMutex);
            auto now = std::chrono::steady_clock::now();
            auto& last = g_resetThrottle["reg:" + ip];
            if (now - last < std::chrono::seconds(60))
                registerThrottled = true;
            else
                last = now;
        }
        int accountsOnIp = 0;
        {
            std::lock_guard<std::mutex> guard(m_dbMutex);
            accountsOnIp = m_database.countByRegIp(ip);
        }
        if (registerThrottled)
        {
            sendResponse(ssl, 200, "OK", "text/html",
                         registerPage("Please wait a minute before creating another account.", true,
                                      !m_config.googleClientId.empty()));
            SSL_shutdown(ssl);
            SSL_free(ssl);
            return;
        }
        if (accountsOnIp >= 2)
        {
            logWarn("Registration blocked for " + ip + " (" + std::to_string(accountsOnIp) +
                    " accounts already registered from this IP)");
            sendResponse(ssl, 200, "OK", "text/html",
                         registerPage("Account limit reached for this network (max 2).", true,
                                      !m_config.googleClientId.empty()));
            SSL_shutdown(ssl);
            SSL_free(ssl);
            return;
        }

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
            sendResponse(ssl, 200, "OK", "text/html", registerPage(error, true, !m_config.googleClientId.empty()));
        }
        else
        {
            std::lock_guard<std::mutex> guard(m_dbMutex);
            auto result = m_database.registerPlayer(grow, password, email, m_config.defaultRoleId, ip);
            if (result == Database::RegisterResult::Duplicate)
                sendResponse(ssl, 200, "OK", "text/html",
                             registerPage("That username is already taken.", true,
                                          !m_config.googleClientId.empty()));
            else if (result != Database::RegisterResult::Ok)
                sendResponse(ssl, 200, "OK", "text/html",
                             registerPage("Could not create the account, try again.", true,
                                          !m_config.googleClientId.empty()));
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
                // Per-IP email budget: max 3 reset emails per hour.
                if (failure.empty())
                {
                    auto& stamps = g_mailBudget[resolveClientIp(request, socketIp)];
                    while (!stamps.empty() && now - stamps.front() > std::chrono::hours(1))
                        stamps.pop_front();
                    if (stamps.size() >= 3)
                        failure = "Too many reset emails requested. Try again later.";
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
                        {
                            std::lock_guard<std::mutex> guard(g_throttleMutex);
                            g_mailBudget[resolveClientIp(request, socketIp)].push_back(
                                std::chrono::steady_clock::now());
                        }
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

        // Brute-force protection for the 6-digit code: the same guard system
        // as login (5 wrong attempts -> 5-minute block, escalating).
        std::string ip = resolveClientIp(request, socketIp);
        std::string fp = fingerprintOf(request);
        std::string userKey = "r:" + grow + "|" + ip;
        std::string fpKey = "r:" + grow + "|" + fp;
        guardPrune();
        long wait = std::max({guardCheck(userKey), guardCheck(fpKey)});
        if (wait > 0)
        {
            guardFail(userKey);
            guardFail(fpKey);
            long escalated = std::max({guardCheck(userKey), guardCheck(fpKey)});
            logWarn("Rate-limited reset attempt for '" + grow + "' from " + ip + " (fp " + fp + ", " +
                    std::to_string(escalated) + "s left)");
            sendResponse(ssl, 200, "OK", "text/html",
                         resetPage(grow, "Too many attempts. Try again in " +
                                             std::to_string((escalated + 59) / 60) + " minute(s).",
                                   true));
            SSL_shutdown(ssl);
            SSL_free(ssl);
            return;
        }

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
                    m_database.clearResetCodes(playerId);
                    guardClear(userKey);
                    guardClear(fpKey);
                    sendResponse(ssl, 200, "OK", "text/html",
                                 messagePage("Password updated",
                                             "Your password has been changed. Log in with the new one.",
                                             "/player/login/dashboard", "Go to login"));
                }
                else
                {
                    // Wrong code: rate-limit AND burn the code after 5 tries.
                    guardFail(userKey);
                    guardFail(fpKey);
                    bool invalidated = false;
                    if (found)
                        invalidated = m_database.failResetCode(playerId);
                    logWarn("Wrong reset code for '" + grow + "' from " + ip + " (fp " + fp + ")" +
                            (invalidated ? " — code invalidated" : ""));
                    sendResponse(ssl, 200, "OK", "text/html",
                                 resetPage(grow, invalidated ? "Too many wrong attempts — request a new "
                                                               "code."
                                                             : "That code is wrong or expired.",
                                           true));
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

        std::string ip = resolveClientIp(request, socketIp);
        std::string fp = fingerprintOf(request);
        std::string userKey = "u:" + grow + "|" + ip;
        std::string fpKey = "f:" + grow + "|" + fp;
        std::string ipKey = "i:" + ip;
        guardPrune();

        auto blockedJson = [](long seconds) {
            long minutes = (seconds + 59) / 60;
            return "{\"status\":\"error\",\"message\":\"Too many failed attempts. Try again in " +
                   std::to_string(minutes) + " minute(s).\"}";
        };

        long wait = std::max({guardCheck(userKey), guardCheck(fpKey)});
        if (wait > 0)
        {
            // Forced attempts during a block escalate the penalty.
            guardFail(userKey);
            guardFail(fpKey);
            long escalated = std::max({guardCheck(userKey), guardCheck(fpKey)});
            logWarn("Rate-limited login for '" + grow + "' from " + ip + " (fp " + fp + ", " +
                    std::to_string(escalated) + "s left)");
            sendResponse(ssl, 429, "Too Many Requests", "application/json", blockedJson(escalated));
        }
        else if (grow.empty() || password.empty())
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
            // Verify the credentials here so failures can be counted; the
            // game server re-verifies on connect (defense in depth).
            Database::LoginCheck check;
            {
                std::lock_guard<std::mutex> guard(m_dbMutex);
                check = m_database.verifyLogin(grow, password);
            }
            if (check == Database::LoginCheck::Ok)
            {
                guardClear(userKey);
                guardClear(fpKey);
                std::string account =
                    Base64::encode("_token=" + token + "&growId=" + grow + "&password=" + password);
                {
                    std::lock_guard<std::mutex> guard(g_issuedMutex);
                    g_issuedTokens[account] = {grow, password};
                }
                std::string body =
                    "{\"status\":\"success\",\"message\":\"Account Validated.\",\"token\":\"" +
                    jsonEscape(account) + "\",\"url\":\"\",\"accountType\":\"growtopia\"}";
                sendResponse(ssl, 200, "OK", "application/json", body,
                             {"Set-Cookie: gtps_token=" + account + "; Path=/; HttpOnly; Secure",
                              "Set-Cookie: growId=" + grow + "; Path=/; HttpOnly; Secure"});
            }
            else if (check == Database::LoginCheck::Banned)
            {
                sendResponse(ssl, 403, "Forbidden", "application/json",
                             "{\"status\":\"error\",\"message\":\"This account is banned.\"}");
            }
            else if (check == Database::LoginCheck::Error)
            {
                sendResponse(ssl, 500, "Internal Server Error", "application/json",
                             "{\"status\":\"error\",\"message\":\"Server error, try again.\"}");
            }
            else
            {
                // Wrong username or password: count against the account+IP,
                // the fingerprint, and the IP-wide budget.
                long userBlock = guardFail(userKey);
                guardFail(fpKey);
                guardFail(ipKey);
                logWarn("Failed login for '" + grow + "' from " + ip + " (fp " + fp + ")" +
                        (userBlock > 0 ? " — rate limited " + std::to_string(userBlock) + "s" : ""));
                sendResponse(ssl, 401, "Unauthorized", "application/json",
                             userBlock > 0 ? blockedJson(userBlock)
                                           : "{\"status\":\"error\",\"message\":\"Wrong username or "
                                             "password.\"}");
            }
        }
    }
    else if (path == "/player/auth/google" && !isPost)
    {
        if (m_config.googleClientId.empty())
        {
            sendResponse(ssl, 200, "OK", "text/html",
                         messagePage("Google sign-in", "Google sign-in is not configured on this server.",
                                     "/player/login/dashboard", "Back to login"));
        }
        else
        {
            std::string state = generateStateToken();
            if (state.empty())
            {
                sendResponse(ssl, 500, "Internal Server Error", "text/plain", "state generation failed");
            }
            else
            {
                {
                    std::lock_guard<std::mutex> guard(g_stateMutex);
                    g_oauthStates[state] = std::chrono::steady_clock::now();
                }
                // Encode only the parameter values (':' and '/' in the
                // redirect_uri must not break the query).
                auto urlEncode = [](const std::string& text) {
                    std::string encoded;
                    for (char c : text)
                    {
                        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
                            c == '.' || c == '~')
                            encoded.push_back(c);
                        else
                        {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
                            encoded += buf;
                        }
                    }
                    return encoded;
                };
                std::ostringstream out;
                out << "HTTP/1.1 302 Found\r\nLocation: https://accounts.google.com/o/oauth2/v2/auth"
                    << "?client_id=" << urlEncode(m_config.googleClientId)
                    << "&redirect_uri=" << urlEncode(m_config.googleRedirectUri)
                    << "&response_type=code"
                    << "&scope=openid%20email"
                    << "&state=" << state
                    << "&prompt=select_account"
                    << "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                SSL_write(ssl, out.str().data(), static_cast<int>(out.str().size()));
            }
        }
    }
    else if (path == "/player/auth/google/callback" && !isPost)
    {
        auto fields = parseForm(request.body);
        // Callback query params live in the raw request line; re-parse them
        // from the original path-with-query is already stripped, so read the
        // code/state from the stored query (kept in request.headers by readRequest).
        std::string code = fields.count("code") ? fields["code"] : "";
        std::string state = fields.count("state") ? fields["state"] : "";
        std::string oauthError = fields.count("error") ? fields["error"] : "";
        // readRequest strips the query; recover it from the raw path we saved.
        std::string rawQuery = request.query;
        if (!rawQuery.empty())
        {
            auto callbackFields = parseForm(rawQuery);
            if (callbackFields.count("code")) code = callbackFields["code"];
            if (callbackFields.count("state")) state = callbackFields["state"];
            if (callbackFields.count("error")) oauthError = callbackFields["error"];
        }

        std::string fail;
        bool validState = false;
        {
            std::lock_guard<std::mutex> guard(g_stateMutex);
            auto it = g_oauthStates.find(state);
            if (it != g_oauthStates.end())
            {
                validState = true;
                g_oauthStates.erase(it);
            }
            // Expire stale states.
            auto now = std::chrono::steady_clock::now();
            for (auto sit = g_oauthStates.begin(); sit != g_oauthStates.end();)
            {
                if (now - sit->second > std::chrono::minutes(10))
                    sit = g_oauthStates.erase(sit);
                else
                    ++sit;
            }
        }
        if (m_config.googleClientId.empty())
            fail = "Google sign-in is not configured.";
        else if (!oauthError.empty())
            fail = "Google sign-in was cancelled.";
        else if (!validState || code.empty())
            fail = "Invalid sign-in session. Try again.";

        std::string email, sub;
        if (fail.empty())
        {
            // Exchange the authorization code for an id_token.
            std::string body = "code=" + code + "&client_id=" + m_config.googleClientId +
                               "&client_secret=" + m_config.googleClientSecret + "&redirect_uri=" +
                               m_config.googleRedirectUri + "&grant_type=authorization_code";
            std::string response;
            if (!httpsPost("oauth2.googleapis.com", "/token", body,
                           {"Content-Type: application/x-www-form-urlencoded"}, &response))
            {
                fail = "Could not reach Google. Try again.";
            }
            else if (!jsonGetString(response, "id_token", email))
            {
                fail = "Google did not return a sign-in token.";
            }
            else
            {
                // id_token = header.payload.signature; the payload carries sub/email.
                auto p1 = email.find('.');
                auto p2 = email.find('.', p1 + 1);
                if (p1 == std::string::npos || p2 == std::string::npos)
                {
                    fail = "Malformed Google token.";
                    email.clear();
                }
                else
                {
                    std::string payload = base64UrlDecode(email.substr(p1 + 1, p2 - p1 - 1));
                    std::string subValue;
                    std::string emailValue;
                    if (!jsonGetString(payload, "sub", subValue) ||
                        !jsonGetString(payload, "email", emailValue) || subValue.empty() ||
                        emailValue.empty())
                    {
                        fail = "Google account has no usable email.";
                    }
                    else
                    {
                        // The token came straight from Google's token endpoint
                        // over TLS, so the payload is trusted as-is.
                        sub = subValue;
                        email = emailValue;
                    }
                }
            }
        }

        if (!fail.empty())
        {
            sendResponse(ssl, 200, "OK", "text/html",
                         messagePage("Google sign-in", fail, "/player/login/dashboard", "Back to login"));
        }
        else
        {
            // Find or create the linked account, then issue a fresh random
            // password and return the same JSON the client expects from
            // login/validate — the webview sniffs it and continues the login.
            std::lock_guard<std::mutex> guard(m_dbMutex);
            uint32_t playerId = 0;
            std::string growId;
            if (auto linked = m_database.findPlayerByGoogleSub(sub))
            {
                playerId = linked->first;
                growId = linked->second;
            }
            else
            {
                auto byEmail = m_database.findPlayerByEmail(email);
                if (byEmail.has_value())
                {
                    auto [id, name, alreadyLinked] = byEmail.value();
                    if (alreadyLinked)
                    {
                        fail = "That Google account is linked to a different player.";
                    }
                    else
                    {
                        playerId = id;
                        growId = name;
                        m_database.setGoogleSub(playerId, sub);
                    }
                }
                else
                {
                    // New account: derive a unique username from the email.
                    std::string base = email.substr(0, email.find('@'));
                    std::string candidate;
                    for (char c : base)
                    {
                        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
                            candidate.push_back(c);
                    }
                    if (candidate.empty() || std::isdigit(static_cast<unsigned char>(candidate[0])))
                        candidate = "Player" + candidate;
                    if (candidate.size() > 24)
                        candidate.resize(24);
                    std::string unique = candidate;
                    for (int i = 2; m_database.growIdExists(unique); ++i)
                        unique = candidate + std::to_string(i);
                    if (unique.size() > 32)
                        unique = unique.substr(0, 32);
                    auto created = m_database.registerGooglePlayer(unique, sub, email,
                                                                   m_config.defaultRoleId,
                                                                   resolveClientIp(request, socketIp));
                    if (created == Database::RegisterResult::Ok)
                    {
                        if (auto row = m_database.findPlayerId(unique))
                        {
                            playerId = *row;
                            growId = unique;
                        }
                    }
                    else if (created == Database::RegisterResult::Duplicate)
                    {
                        fail = "That username is taken — try signing in again.";
                    }
                    else
                    {
                        fail = "Could not create the account, try again.";
                    }
                }
            }

            if (!fail.empty())
            {
                sendResponse(ssl, 200, "OK", "text/html",
                             messagePage("Google sign-in", fail, "/player/login/dashboard",
                                         "Back to login"));
            }
            else
            {
                std::string password = generateRandomPassword();
                m_database.updatePassword(playerId, password);
                std::string account = Base64::encode("_token=google&growId=" + growId +
                                                     "&password=" + password);
                {
                    std::lock_guard<std::mutex> tokenGuard(g_issuedMutex);
                    g_issuedTokens[account] = {growId, password};
                }
                std::string body = "{\"status\":\"success\",\"message\":\"Account Validated.\",\"token\":\"" +
                                   jsonEscape(account) + "\",\"url\":\"\",\"accountType\":\"growtopia\"}";
                sendResponse(ssl, 200, "OK", "application/json", body);
                logInfo("Google sign-in completed for " + growId);
            }
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
