#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <random>
#include <webview/webview.h>
#include "resource.h"

using json = nlohmann::json;

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <bcrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

// SQLite
#include <sqlite3.h>

#include "version.h"

// Debug logging
std::ofstream g_debug_log;
void init_debug_log(const std::string& dir) {
    if (g_debug_log.is_open()) {
        return;
    }
    g_debug_log.open(dir + "\\debug.log", std::ios::app);
    if (!g_debug_log.is_open()) {
        g_debug_log.open("debug.log", std::ios::app);
    }
}

// Masks values that must never land in a log file that ships with the app:
// OAuth codes/tokens, client secrets, CSRF state and e-mail addresses.
std::string redact_sensitive(const std::string& in) {
    static const char* kKeys[] = {"code", "access_token", "refresh_token", "id_token",
                                  "client_secret", "state", "code_verifier"};
    std::string out = in;
    for (const char* key : kKeys) {
        const std::string needle = std::string(key) + "=";
        size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            // Only treat it as a parameter when preceded by ? & or start-of-string.
            if (pos != 0 && out[pos - 1] != '?' && out[pos - 1] != '&' && out[pos - 1] != ' ') {
                pos += needle.size();
                continue;
            }
            const size_t vstart = pos + needle.size();
            size_t vend = out.find_first_of("&\r\n \"", vstart);
            if (vend == std::string::npos) vend = out.size();
            if (vend > vstart) out.replace(vstart, vend - vstart, "[redacted]");
            pos = vstart + 10;
        }
    }
    // Mask the local part of anything that looks like an e-mail address.
    size_t at = 0;
    while ((at = out.find('@', at)) != std::string::npos) {
        size_t start = at;
        while (start > 0) {
            const unsigned char c = out[start - 1];
            if (std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+') start--;
            else break;
        }
        if (at > start) {
            out.replace(start, at - start, "[email]");
            at = start + 7 + 1;
        } else {
            at++;
        }
    }
    return out;
}

void debug_log(const std::string& raw) {
    const std::string msg = redact_sensitive(raw);
    if (g_debug_log.is_open()) {
        g_debug_log << "[" << std::chrono::system_clock::now().time_since_epoch().count() << "] " << msg << '\n';
        g_debug_log.flush();
    }
    std::cerr << msg << '\n';
}

// ─── Global State ─────────────────────────────────────────────────────────────
sqlite3* g_db = nullptr;
std::string g_app_html_path;
std::string g_app_dir;
std::string g_resource_dir;
std::string g_db_path;
json g_oauth_config;

// OAuth local callback server state
std::atomic<bool> g_oauth_server_running{false};
std::mutex g_oauth_mutex;               // guards the two fields below
std::string g_oauth_expected_provider;  // provider of the most recent login attempt
std::string g_oauth_expected_state;     // CSRF token sent with that attempt
std::string g_oauth_code_verifier;      // PKCE verifier for that attempt (RFC 7636)

// Update check state
bool g_is_dev_build = false;              // running from the source tree's build folder
std::atomic<bool> g_shutting_down{false}; // set once the message loop has returned
std::mutex g_update_mutex;                // guards g_update_download_url
std::string g_update_download_url;        // validated URL from the last successful check

// ─── SHA-256 Password Hashing (Windows CryptoAPI) ─────────────────────────────
std::string sha256_hex(const std::string& input, const std::string& salt = "") {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;
    std::string salted_input = input + salt;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    if (!CryptHashData(hHash, reinterpret_cast<const BYTE*>(salted_input.data()), (DWORD)salted_input.size(), 0)) {
        CryptDestroyHash(hHash);
        CryptReleaseContext(hProv, 0);
        return "";
    }
    BYTE hashBytes[32];
    DWORD hashLen = 32;
    if (CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashLen, 0)) {
        std::ostringstream oss;
        for (int i = 0; i < 32; i++) {
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)hashBytes[i];
        }
        result = oss.str();
    }
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return result;
}

// ─── Cryptographic randomness ─────────────────────────────────────────────────
// mt19937 is predictable from its output and must not generate CSRF tokens,
// PKCE verifiers or password salts; BCryptGenRandom is the OS CSPRNG.
bool secure_random_bytes(unsigned char* out, size_t len) {
    return BCRYPT_SUCCESS(BCryptGenRandom(nullptr, out, (ULONG)len,
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

std::string secure_random_hex(size_t byte_count) {
    std::vector<unsigned char> buf(byte_count);
    if (!secure_random_bytes(buf.data(), buf.size())) return "";
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(byte_count * 2);
    for (unsigned char b : buf) { out += hex[b >> 4]; out += hex[b & 0x0F]; }
    return out;
}

std::string generate_random_state() { return secure_random_hex(16); }

// ─── PBKDF2-HMAC-SHA256 password hashing ──────────────────────────────────────
// Plain SHA-256 is a fast hash: a GPU tries billions of candidates per second.
// PBKDF2 makes each guess deliberately expensive. Iteration count is stored per
// row so it can be raised later without locking existing users out.
constexpr uint32_t kPbkdf2Iterations = 310000;
constexpr const char* kAlgoPbkdf2 = "pbkdf2-sha256";
constexpr const char* kAlgoLegacy = "sha256";

std::string pbkdf2_sha256_hex(const std::string& password, const std::string& salt,
                              uint32_t iterations) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                    BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return "";
    }
    unsigned char derived[32] = {0};
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(
        alg,
        (PUCHAR)password.data(), (ULONG)password.size(),
        (PUCHAR)salt.data(), (ULONG)salt.size(),
        iterations, derived, sizeof(derived), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) return "";

    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(derived) * 2);
    for (unsigned char b : derived) { out += hex[b >> 4]; out += hex[b & 0x0F]; }
    return out;
}

// Compares without leaking where the first difference is via timing.
bool constant_time_equals(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

// ─── URL safety ───────────────────────────────────────────────────────────────
// Everything reaching ShellExecute or webview navigation originates in the
// WebView (ultimately from a third-party API), so only plain http(s) is allowed.
// This blocks file://, UNC paths, javascript: and arbitrary registered handlers.
bool is_safe_web_url(const std::string& url) {
    if (url.size() < 8 || url.size() > 2000) return false;
    auto starts_with_ci = [&url](const char* prefix) {
        const size_t n = strlen(prefix);
        if (url.size() < n) return false;
        for (size_t i = 0; i < n; i++)
            if (std::tolower((unsigned char)url[i]) != (unsigned char)prefix[i]) return false;
        return true;
    };
    if (!starts_with_ci("http://") && !starts_with_ci("https://")) return false;
    // Reject control characters, quotes and whitespace that could break out of
    // the surrounding command, attribute or header.
    for (unsigned char c : url) {
        if (c < 0x21 || c == 0x7F || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\')
            return false;
    }
    // A bare "http://" with no host, or an embedded credential, is not expected here.
    const size_t host_start = url.find("//") + 2;
    if (host_start >= url.size() || url[host_start] == '/') return false;
    return true;
}

// ─── PKCE helpers (RFC 7636) ──────────────────────────────────────────────────
std::string sha256_raw(const std::string& input) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string out;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return out;
    if (!CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) { CryptReleaseContext(prov, 0); return out; }
    if (CryptHashData(hash, reinterpret_cast<const BYTE*>(input.data()), (DWORD)input.size(), 0)) {
        BYTE digest[32];
        DWORD len = sizeof(digest);
        if (CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0))
            out.assign(reinterpret_cast<char*>(digest), len);
    }
    CryptDestroyHash(hash);
    CryptReleaseContext(prov, 0);
    return out;
}

std::string base64url_encode(const std::string& in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned b0 = (unsigned char)in[i];
        const unsigned b1 = (i + 1 < in.size()) ? (unsigned char)in[i + 1] : 0;
        const unsigned b2 = (i + 2 < in.size()) ? (unsigned char)in[i + 2] : 0;
        out += tbl[b0 >> 2];
        out += tbl[((b0 & 0x03) << 4) | (b1 >> 4)];
        if (i + 1 < in.size()) out += tbl[((b1 & 0x0F) << 2) | (b2 >> 6)];
        if (i + 2 < in.size()) out += tbl[b2 & 0x3F];
    }
    return out;  // no '=' padding, per RFC 7636
}

std::string base64_std_encode(const std::string& in) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) |
                            (unsigned char)in[i + 2];
        out += tbl[(v >> 18) & 0x3F]; out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];  out += tbl[v & 0x3F];
    }
    if (i < in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        const bool two = (i + 1 < in.size());
        if (two) v |= (unsigned char)in[i + 1] << 8;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += two ? tbl[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

std::wstring utf8_to_wide(const std::string& in) {
    if (in.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out((size_t)need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.data(), (int)in.size(), &out[0], need);
    return out;
}

// ─── Toast notification ───────────────────────────────────────────────────────
// The title and message come from the WebView, so they are passed as environment
// variables and never interpolated into a command line. The script itself is a
// fixed constant handed over as -EncodedCommand, so there is no quoting to get
// wrong in cmd.exe or PowerShell, and no cmd.exe in the chain at all.
void show_toast_notification(const std::string& title, const std::string& message) {
    static const char* kScript =
        "$ErrorActionPreference='Stop';"
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null;"
        "$t=[System.Security.SecurityElement]::Escape($env:GS_TOAST_TITLE);"
        "$m=[System.Security.SecurityElement]::Escape($env:GS_TOAST_MSG);"
        "$x=New-Object Windows.Data.Xml.Dom.XmlDocument;"
        "$x.LoadXml(\"<toast><visual><binding template='ToastText02'>\" +"
        "\"<text id='1'>$t</text><text id='2'>$m</text>\" +"
        "\"</binding></visual></toast>\");"
        "$toast=New-Object Windows.UI.Notifications.ToastNotification $x;"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('GameStash').Show($toast)";

    // -EncodedCommand expects base64 of the UTF-16LE script bytes.
    const std::wstring wscript = utf8_to_wide(kScript);
    std::string raw;
    raw.reserve(wscript.size() * 2);
    for (wchar_t ch : wscript) {
        raw += (char)(ch & 0xFF);
        raw += (char)((ch >> 8) & 0xFF);
    }
    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden "
                       L"-EncodedCommand " + utf8_to_wide(base64_std_encode(raw));

    // Child environment = ours plus the two toast variables.
    std::wstring env;
    if (LPWCH parent = GetEnvironmentStringsW()) {
        for (LPWCH v = parent; *v; ) {
            const size_t n = wcslen(v);
            // Drop any inherited copies so the caller cannot smuggle a value in.
            if (_wcsnicmp(v, L"GS_TOAST_TITLE=", 15) != 0 &&
                _wcsnicmp(v, L"GS_TOAST_MSG=", 13) != 0) {
                env.append(v, n);
                env.push_back(L'\0');
            }
            v += n + 1;
        }
        FreeEnvironmentStringsW(parent);
    }
    auto sanitize = [](const std::string& in) {
        std::string out;
        for (char c : in) if ((unsigned char)c >= 0x20 && c != 0x7F) out += c;
        return out.size() > 512 ? out.substr(0, 512) : out;
    };
    env += L"GS_TOAST_TITLE=" + utf8_to_wide(sanitize(title)); env.push_back(L'\0');
    env += L"GS_TOAST_MSG="   + utf8_to_wide(sanitize(message)); env.push_back(L'\0');
    env.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back(L'\0');

    if (CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                       (LPVOID)env.data(), nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        debug_log("[WARN] toast notification could not be launched: " +
                  std::to_string(GetLastError()));
    }
}

// ─── Email Validation ─────────────────────────────────────────────────────────
bool is_valid_email(const std::string& email) {
    // Basic email validation
    if (email.empty() || email.length() > 254) return false;

    size_t at_pos = email.find('@');
    if (at_pos == std::string::npos || at_pos == 0 || at_pos == email.length() - 1)
        return false;

    // Check for local part
    std::string local = email.substr(0, at_pos);
    if (local.find("..") != std::string::npos) return false;
    for (unsigned char c : local) {
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+')
            return false;
    }

    // Check for domain part
    std::string domain = email.substr(at_pos + 1);
    if (domain.find("..") != std::string::npos) return false;
    if (domain.find('.') == std::string::npos) return false;

    size_t last_dot = domain.rfind('.');
    std::string tld = domain.substr(last_dot + 1);
    if (tld.length() < 2) return false;

    for (unsigned char c : domain) {
        if (!std::isalnum(c) && c != '.' && c != '-')
            return false;
    }

    return true;
}

// ─── URL Encode ───────────────────────────────────────────────────────────────
std::string url_encode(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            oss << c;
        } else {
            oss << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
    }
    return oss.str();
}

// ─── SQLite Helpers ───────────────────────────────────────────────────────────
bool db_init() {
    // open_v2 states the intent explicitly rather than relying on defaults.
    int rc = sqlite3_open_v2(g_db_path.c_str(), &g_db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        debug_log("Cannot open database: " + std::string(sqlite3_errmsg(g_db)));
        return false;
    }
    debug_log("Database opened successfully at: " + g_db_path);

    // WAL survives a hard kill far better than the default rollback journal, and
    // foreign_keys defaults to OFF in SQLite - without it the FOREIGN KEY clause
    // declared on claimed_games below is silently inert.
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);

    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL,
            email TEXT UNIQUE,
            password_hash TEXT,
            password_salt TEXT,
            provider TEXT DEFAULT 'local',
            provider_id TEXT,
            avatar_url TEXT,
            bio TEXT,
            country TEXT,
            favorite_platform TEXT,
            notification_email INTEGER DEFAULT 1,
            theme TEXT DEFAULT 'dark',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE TABLE IF NOT EXISTS claimed_games (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            user_id INTEGER NOT NULL,
            game_id INTEGER NOT NULL,
            title TEXT,
            thumbnail TEXT,
            platforms TEXT,
            worth_inr TEXT,
            open_giveaway_url TEXT,
            claimed_at DATETIME DEFAULT CURRENT_TIMESTAMP,
            UNIQUE(user_id, game_id),
            FOREIGN KEY(user_id) REFERENCES users(id)
        );
    )";

    char* err = nullptr;
    rc = sqlite3_exec(g_db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        debug_log("DB init error: " + std::string(err));
        sqlite3_free(err);
        return false;
    }

    // Migrations. These fail harmlessly when the column already exists.
    sqlite3_exec(g_db, "ALTER TABLE users ADD COLUMN password_salt TEXT;", nullptr, nullptr, nullptr);
    // Which KDF produced password_hash, so legacy rows can be upgraded on login.
    sqlite3_exec(g_db, "ALTER TABLE users ADD COLUMN password_algo TEXT;", nullptr, nullptr, nullptr);
    sqlite3_exec(g_db, "ALTER TABLE users ADD COLUMN password_iterations INTEGER;", nullptr, nullptr, nullptr);

    debug_log("Database initialized OK");
    return true;
}

// ─── Check if file exists ─────────────────────────────────────────────────────
bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

// ─── String helpers ───────────────────────────────────────────────────────────
bool ends_with(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) {
        return false;
    }
    return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
}

// ─── Protocol and App Launch Helpers ─────────────────────────────────────────
bool is_protocol_registered(const std::string& protocol) {
    HKEY hKey = nullptr;
    std::string key = protocol + "\\shell\\open\\command";
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, key.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool shell_open(const std::string& target) {
    auto result = (INT_PTR)ShellExecuteA(nullptr, "open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return result > 32;
}

std::string extract_epic_slug(const std::string& url) {
    const std::string marker1 = "/p/";
    const std::string marker2 = "/en-US/p/";
    size_t pos = url.find(marker2);
    size_t start = std::string::npos;
    if (pos != std::string::npos) {
        start = pos + marker2.size();
    } else {
        pos = url.find(marker1);
        if (pos != std::string::npos) start = pos + marker1.size();
    }
    if (start == std::string::npos || start >= url.size()) return "";
    size_t end = url.find_first_of("/?#", start);
    if (end == std::string::npos) end = url.size();
    return url.substr(start, end - start);
}

// ─── Get Application Directory from .exe location ─────────────────────────────
std::string get_app_directory() {
    char buffer[MAX_PATH] = {0};
    // Get absolute path to the running .exe file
    DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (length == 0 || length == MAX_PATH) {
        return "";
    }

    std::string exePath(buffer);
    // Strip the .exe filename to get directory
    size_t lastSlash = exePath.find_last_of('\\');
    if (lastSlash != std::string::npos) {
        return exePath.substr(0, lastSlash);
    }
    return "";
}

// ─── Get AppData Roaming Folder ───────────────────────────────────────────────
std::string get_appdata_folder() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::string(path);
    }
    // Fallback to environment variable
    const char* appdata = getenv("APPDATA");
    if (appdata) {
        return std::string(appdata);
    }
    return "";
}

// ─── Ensure Directory Exists ──────────────────────────────────────────────────
bool ensure_directory_exists(const std::string& path) {
    // CreateDirectoryA only creates a single level, so create each parent first.
    // Failures here are ignored (usually ERROR_ALREADY_EXISTS); the final call decides.
    size_t pos = 0;
    while ((pos = path.find('\\', pos)) != std::string::npos) {
        std::string dir = path.substr(0, pos++);
        if (!dir.empty()) CreateDirectoryA(dir.c_str(), nullptr);
    }
    return CreateDirectoryA(path.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

// ─── Read Local File ──────────────────────────────────────────────────────────
std::string read_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ─── INR Conversion ───────────────────────────────────────────────────────────
constexpr double USD_TO_INR_RATE = 83.5;

std::string get_worth_in_inr(const std::string& worth_usd) {
    if (worth_usd == "N/A") return "N/A";
    if (!worth_usd.empty() && worth_usd[0] == '$') {
        try {
            double usd_val = std::stod(worth_usd.substr(1));
            double inr_val = usd_val * USD_TO_INR_RATE;
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(2) << "Rs. " << inr_val;
            return stream.str();
        } catch (...) { return worth_usd; }
    }
    return worth_usd;
}

// ─── WinHTTP helpers ──────────────────────────────────────────────────────────
struct WinHttpHandle {
    HINTERNET h = nullptr;
    explicit WinHttpHandle(HINTERNET handle) : h(handle) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

// Performs a single HTTP(S) request and returns the response body, or "" on failure.
// `headers` is a CRLF-separated header block; `body` is sent as-is when non-empty.
std::string winhttp_request(const wchar_t* method, const std::string& url,
                            const std::string& headers, const std::string& body = "") {
    std::wstring wurl(url.begin(), url.end());
    WinHttpHandle session(WinHttpOpen(L"GameStash/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return "";
    WinHttpSetTimeouts(session.h, 10000, 10000, 10000, 10000);

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256], urlPath[2048];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) return "";

    WinHttpHandle connect(WinHttpConnect(session.h, hostName, urlComp.nPort, 0));
    if (!connect) return "";

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(WinHttpOpenRequest(connect.h, method, urlPath, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) return "";

    std::wstring wheaders(headers.begin(), headers.end());
    if (!WinHttpAddRequestHeaders(request.h, wheaders.c_str(), (DWORD)-1,
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        debug_log("[WARN] WinHttpAddRequestHeaders failed with error: " + std::to_string(GetLastError()));
    }

    LPVOID data = body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data();
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            data, (DWORD)body.size(), (DWORD)body.size(), 0)) {
        debug_log("[ERROR] WinHttpSendRequest failed with error: " + std::to_string(GetLastError()));
        return "";
    }
    if (!WinHttpReceiveResponse(request.h, nullptr)) {
        debug_log("[ERROR] WinHttpReceiveResponse failed with error: " + std::to_string(GetLastError()));
        return "";
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    debug_log("[INFO] HTTP " + std::to_string(statusCode) + " from " + url);

    std::string response;
    DWORD dwSize = 0;
    while (WinHttpQueryDataAvailable(request.h, &dwSize) && dwSize > 0) {
        std::vector<char> buf(dwSize);
        DWORD dwRead = 0;
        if (!WinHttpReadData(request.h, buf.data(), dwSize, &dwRead)) break;
        response.append(buf.data(), dwRead);
    }
    return response;
}

std::string winhttp_get(const std::string& url, const std::string& extra_headers = "") {
    return winhttp_request(L"GET", url,
        "User-Agent: GameStash/1.0\r\nAccept: application/json\r\n" + extra_headers);
}

// POSTs a form-encoded body (what the OAuth token endpoints expect).
std::string winhttp_post_form(const std::string& url, const std::string& body) {
    return winhttp_request(L"POST", url,
        "Content-Type: application/x-www-form-urlencoded\r\nAccept: application/json\r\n", body);
}

// ─── JSON / SQLite helpers ────────────────────────────────────────────────────
// Parses `s` and returns the object, or an empty object if it is malformed or not an object.
json parse_json_object(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    return j.is_object() ? j : json::object();
}

// Reads a TEXT column as std::string, mapping NULL to "".
std::string column_str(sqlite3_stmt* stmt, int col) {
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : "";
}

// ─── Update check (GitHub Releases) ───────────────────────────────────────────
// Releases live on GitHub. The check never downloads or runs anything itself:
// the binaries are not code-signed yet, so there is nothing to verify a
// download against. It only offers to open the release download in the
// browser, where SmartScreen and the user stay in the loop.
constexpr const char* kUpdateRepo = "not-Lucifer/GameStash-Free-Game-Tracker";

struct SemVer {
    int major = 0, minor = 0, patch = 0;
    bool ok = false;
};

// Accepts "1.2.3", "v1.2.3" and "v1.2.3-beta"; any suffix after the numbers
// is ignored for ordering. Missing minor/patch parts count as 0.
SemVer parse_semver(const std::string& raw) {
    SemVer v;
    size_t i = 0;
    if (i < raw.size() && (raw[i] == 'v' || raw[i] == 'V')) i++;
    int* parts[3] = {&v.major, &v.minor, &v.patch};
    int part = 0;
    bool any_digit = false;
    for (; i < raw.size() && part < 3; i++) {
        const char c = raw[i];
        if (c >= '0' && c <= '9') {
            if (*parts[part] > 100000) return SemVer{};  // absurd, reject
            *parts[part] = *parts[part] * 10 + (c - '0');
            any_digit = true;
        } else if (c == '.') {
            if (!any_digit) return SemVer{};
            part++;
            any_digit = false;
        } else {
            break;  // start of a suffix such as "-beta"
        }
    }
    v.ok = (part > 0 || any_digit);
    return v;
}

int compare_semver(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

// Only links that point back into this repository are ever offered to the
// user, so a tampered or spoofed API response cannot redirect the download.
bool is_own_release_url(const std::string& url) {
    const std::string prefix = std::string("https://github.com/") + kUpdateRepo + "/releases/";
    return url.compare(0, prefix.size(), prefix) == 0 && is_safe_web_url(url);
}

bool iends_with(const std::string& s, const std::string& suffix) {
    if (suffix.size() > s.size()) return false;
    for (size_t i = 0; i < suffix.size(); i++) {
        if (std::tolower((unsigned char)s[s.size() - suffix.size() + i]) !=
            std::tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

// Asks GitHub for the release list and reports the newest one.
// Uses /releases rather than /releases/latest on purpose: /latest ignores
// pre-releases, and every Game Stash release so far is marked as one.
json check_for_update() {
    json out;
    out["current"] = GS_VERSION_STRING;
    out["update_available"] = false;

    const std::string url = std::string("https://api.github.com/repos/") + kUpdateRepo +
                            "/releases?per_page=30";
    const std::string body = winhttp_get(url, "X-GitHub-Api-Version: 2022-11-28\r\n");
    if (body.empty()) {
        out["error"] = "Could not reach GitHub";
        return out;
    }
    const json releases = json::parse(body, nullptr, false);
    if (!releases.is_array()) {
        // Rate limiting (60 requests/hour per IP) and "Not Found" both arrive
        // as an object carrying a message.
        const json obj = parse_json_object(body);
        out["error"] = obj.value("message", std::string("Unexpected response from GitHub"));
        return out;
    }

    const SemVer current = parse_semver(GS_VERSION_STRING);
    const json* best = nullptr;
    SemVer best_ver;
    for (const auto& r : releases) {
        if (!r.is_object() || r.value("draft", false)) continue;
        const auto tag = r.find("tag_name");
        if (tag == r.end() || !tag->is_string()) continue;
        const SemVer v = parse_semver(tag->get<std::string>());
        if (!v.ok) continue;
        if (!best || compare_semver(v, best_ver) > 0) {
            best = &r;
            best_ver = v;
        }
    }
    if (!best) {
        out["error"] = "No releases found";
        return out;
    }

    const json& r = *best;
    const std::string page_url = r.value("html_url", std::string());
    std::string download_url, asset_name;
    // Prefer the installer, then a portable zip; otherwise fall back to the page.
    for (const char* wanted : {"-setup.exe", ".zip"}) {
        const auto assets = r.find("assets");
        if (assets == r.end() || !assets->is_array()) break;
        for (const auto& a : *assets) {
            if (!a.is_object()) continue;
            const std::string name = a.value("name", std::string());
            const std::string dl = a.value("browser_download_url", std::string());
            if (iends_with(name, wanted) && is_own_release_url(dl)) {
                download_url = dl;
                asset_name = name;
                break;
            }
        }
        if (!download_url.empty()) break;
    }
    if (download_url.empty() && is_own_release_url(page_url)) download_url = page_url;

    std::string notes = r.value("body", std::string());
    if (notes.size() > 4000) notes = notes.substr(0, 4000) + "\n...";

    out["latest"] = r.value("tag_name", std::string());
    out["prerelease"] = r.value("prerelease", false);
    out["published_at"] = r.value("published_at", std::string());
    out["notes"] = notes;
    out["asset_name"] = asset_name;
    out["update_available"] = !download_url.empty() && compare_semver(best_ver, current) > 0;
    {
        std::lock_guard<std::mutex> lock(g_update_mutex);
        g_update_download_url = out["update_available"].get<bool>() ? download_url : std::string();
    }
    return out;
}

// ─── OAuth: Local Callback Server ─────────────────────────────────────────────
// Runs in a background thread and serves the provider's redirect. It verifies the
// state token, exchanges the code for an access token, fetches the user profile,
// upserts it into the DB and hands the result to the webview via handleOAuthResult.
void run_oauth_callback_server(webview::webview* w) {
    httplib::Server svr;

    svr.Get(R"(/oauth/(google|discord)/callback)", [&](const httplib::Request& req, httplib::Response& res) {
        const std::string provider = req.matches[1];
        const std::string code = req.get_param_value("code");
        const std::string state = req.get_param_value("state");

        std::string expected_provider, expected_state, code_verifier;
        {
            std::lock_guard<std::mutex> lock(g_oauth_mutex);
            expected_provider = g_oauth_expected_provider;
            expected_state = g_oauth_expected_state;
            code_verifier = g_oauth_code_verifier;
            // One login attempt per authorization: clear immediately so a replayed
            // or duplicated callback cannot be accepted a second time.
            g_oauth_expected_provider.clear();
            g_oauth_expected_state.clear();
            g_oauth_code_verifier.clear();
        }

        json result;
        result["provider"] = provider;

        if (provider != expected_provider || state.empty() || state != expected_state) {
            debug_log("[WARN] OAuth callback rejected: provider/state mismatch for " + provider);
            res.status = 400;
            res.set_content("Invalid OAuth response. Please return to Game Stash and try again.", "text/plain");
        } else {
            const bool is_google = provider == "google";
            const json cfg = g_oauth_config.value(provider, json::object());
            std::string body = "code=" + url_encode(code)
                + "&client_id=" + url_encode(cfg.value("client_id", ""))
                + "&redirect_uri=" + url_encode(cfg.value("redirect_uri", ""))
                + "&grant_type=authorization_code"
                + "&code_verifier=" + url_encode(code_verifier);
            // Only sent when the deployment still has a confidential client
            // configured; with PKCE in play it is no longer required.
            const std::string client_secret = cfg.value("client_secret", "");
            if (!client_secret.empty())
                body += "&client_secret=" + url_encode(client_secret);
            const char* token_url = is_google ? "https://oauth2.googleapis.com/token"
                                              : "https://discord.com/api/oauth2/token";
            const char* userinfo_url = is_google ? "https://www.googleapis.com/oauth2/v2/userinfo"
                                                 : "https://discord.com/api/users/@me";

            json token_json = parse_json_object(winhttp_post_form(token_url, body));
            std::string access_token = token_json.value("access_token", "");
            if (!access_token.empty()) {
                json user_json = parse_json_object(winhttp_get(userinfo_url,
                    "Authorization: Bearer " + access_token + "\r\n"));
                std::string uid = user_json.value("id", "");
                result["user_id"] = uid;
                result["email"] = user_json.value("email", "");
                if (is_google) {
                    result["name"] = user_json.value("name", "Google User");
                    result["avatar"] = user_json.value("picture", "");
                } else {
                    std::string avatar_hash = user_json.value("avatar", "");
                    result["name"] = user_json.value("username", "Discord User");
                    result["avatar"] = avatar_hash.empty() ? "" :
                        "https://cdn.discordapp.com/avatars/" + uid + "/" + avatar_hash + ".png";
                }
            }

            // Upsert the user and look up its DB id
            int db_user_id = -1;
            std::string pid = result.value("user_id", "");
            if (!pid.empty()) {
                std::string name = result.value("name", "User");
                std::string email = result.value("email", "");
                std::string avatar = result.value("avatar", "");

                const char* upsert = R"(
                    INSERT INTO users (username, email, provider, provider_id, avatar_url)
                    VALUES (?, ?, ?, ?, ?)
                    ON CONFLICT(email) DO UPDATE SET
                        username=excluded.username,
                        provider=excluded.provider,
                        provider_id=excluded.provider_id,
                        avatar_url=excluded.avatar_url
                )";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(g_db, upsert, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, provider.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 4, pid.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 5, avatar.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }

                const char* sel = "SELECT id FROM users WHERE provider_id=? AND provider=?";
                if (sqlite3_prepare_v2(g_db, sel, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, pid.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(stmt) == SQLITE_ROW) {
                        db_user_id = sqlite3_column_int(stmt, 0);
                    }
                    sqlite3_finalize(stmt);
                }
            }
            result["db_id"] = db_user_id;

            res.set_content(R"(<!DOCTYPE html><html><head>
                <style>body{background:#0a0a0f;color:#fff;font-family:Inter,sans-serif;
                display:flex;align-items:center;justify-content:center;height:100vh;margin:0;flex-direction:column;}
                h2{color:#00d8ff;} p{color:#888;}</style></head><body>
                <h2>✓ Login Successful!</h2>
                <p>You can close this window and return to Game Stash.</p>
                </body></html>)", "text/html");

            // ensure_ascii escapes every non-ASCII character, including U+2028 and
            // U+2029, which are legal in JSON strings but terminate a JS line.
            std::string js = "handleOAuthResult(" + result.dump(-1, ' ', true) + ");";
            w->dispatch([w, js]() { w->eval(js); });
        }

        // One callback per server lifetime; startOAuth spins up a fresh one next time
        svr.stop();
    });

    if (!svr.listen("127.0.0.1", 9876)) {
        debug_log("[ERROR] OAuth callback server could not listen on 127.0.0.1:9876");
    }
    g_oauth_server_running = false;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Get absolute path to the app directory from .exe location
    g_app_dir = get_app_directory();
    if (g_app_dir.empty()) {
        MessageBoxW(nullptr, L"Failed to determine application directory!", L"GameStash Error", MB_ICONERROR);
        return 1;
    }

    // Determine the resource directory
    // If running from a build folder during development, use parent directory for resources
    g_resource_dir = g_app_dir;
    if (file_exists(g_app_dir + "\\..\\CMakeLists.txt") &&
        (ends_with(g_app_dir, "\\build") ||
         ends_with(g_app_dir, "\\build\\Release") ||
         ends_with(g_app_dir, "\\build\\Debug"))) {
        g_resource_dir = g_app_dir + "\\..";
    }
    // Distribution builds run from their install folder; a dev build runs from
    // build\ inside the source tree. Only distribution builds check for updates
    // on their own (set GAMESTASH_UPDATE_CHECK=1 to test the check in dev).
    g_is_dev_build = (g_resource_dir != g_app_dir);

    init_debug_log(g_resource_dir);
    debug_log("App Directory: " + g_app_dir);
    if (g_resource_dir != g_app_dir) {
        debug_log("Development mode detected - using parent directory for resources");
    }

    // Set up AppData folder for database storage
    std::string appdata_path = get_appdata_folder();
    if (appdata_path.empty()) {
        MessageBoxW(nullptr, L"Failed to determine AppData directory!", L"GameStash Error", MB_ICONERROR);
        return 1;
    }

    std::string gamestash_appdata = appdata_path + "\\GameStash";
    if (!ensure_directory_exists(gamestash_appdata)) {
        debug_log("Warning: Could not create GameStash AppData directory, using app directory instead");
        g_db_path = g_resource_dir + "\\gamestash.db";
    } else {
        g_db_path = gamestash_appdata + "\\gamestash.db";
        debug_log("AppData directory created at: " + gamestash_appdata);
    }

    debug_log("Database Path: " + g_db_path);

    // Load OAuth config from resource directory
    std::string oauth_config_path = g_resource_dir + "\\oauth_config.json";
    std::string cfg_content = read_file(oauth_config_path);
    if (cfg_content.empty()) {
        // Try as fallback
        cfg_content = read_file(g_resource_dir + "\\..\\oauth_config.json");
    }
    try {
        g_oauth_config = json::parse(cfg_content);
        debug_log("OAuth config loaded");
    } catch (...) {
        g_oauth_config = json::object();
        debug_log("Warning: could not parse oauth_config.json");
    }

    // Init Database
    if (!db_init()) {
        MessageBoxW(nullptr, L"Failed to initialize database!", L"GameStash Error", MB_ICONERROR);
        return 1;
    }

    webview::webview w(true, nullptr);
    w.set_title("Game Stash");
    w.set_size(1100, 750, WEBVIEW_HINT_NONE);

    // zserge-webview registers its window class with IDI_APPLICATION (the generic
    // Windows icon), so the embedded resource alone does not reach the title bar
    // or taskbar. Set both sizes explicitly on the real HWND.
    auto win_res = w.window();
    if (win_res.ok()) {
        HWND hwnd = static_cast<HWND>(win_res.value());
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        HICON hIconBig = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
        HICON hIconSmall = static_cast<HICON>(LoadImageW(
            hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (hIconBig)   SendMessageW(hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(hIconBig));
        if (hIconSmall) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSmall));
    }

    // ─── Compute HTML path ───────────────────────────────────────────────────
    std::string html_file = g_resource_dir + "\\index.html";
    g_app_html_path = "file:///" + g_resource_dir + "/index.html";
    for (char& c : g_app_html_path) if (c == '\\') c = '/';
    debug_log("HTML path: " + g_app_html_path);
    debug_log("HTML file exists: " + std::string(file_exists(html_file) ? "YES" : "NO"));

    // ─── Binding: fetchGiveaways ──────────────────────────────────────────────
    w.bind("fetchGiveaways", [&](const std::string&) -> std::string {
        debug_log("fetchGiveaways called");
        try {
            // Over plain HTTP any network attacker could rewrite these titles and
            // URLs, which then flow into the UI and into ShellExecute.
            const std::string body = winhttp_get("https://www.gamerpower.com/api/giveaways");
            if (body.empty()) return json({{"error", "No response from API"}}).dump();

            json response_json = json::parse(body, nullptr, false);
            if (!response_json.is_array())
                return json({{"error", "Unexpected API response"}}).dump();
            // Every platform is passed through (Ubisoft Connect, EA, Battle.net,
            // consoles, mobile, ...). Grouping by store is the UI's job, so a store
            // GamerPower adds later shows up under "All" instead of vanishing here.
            json giveaways = json::array();
            for (const auto& item : response_json) {
                if (!item.is_object()) continue;
                json ni = item;
                const auto worth = item.find("worth");
                ni["worth_inr"] = get_worth_in_inr(
                    worth != item.end() && worth->is_string() ? worth->get<std::string>() : "N/A");
                giveaways.push_back(std::move(ni));
            }
            debug_log("Returning " + std::to_string(giveaways.size()) + " giveaways");
            return giveaways.dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: navigateToUrl (in-app webview navigation) ──────────────────
    w.bind("navigateToUrl", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (args.is_array() && args.size() > 0) {
                std::string url = args[0].get<std::string>();
                if (!is_safe_web_url(url)) {
                    debug_log("navigateToUrl: rejected non-http(s) target");
                    return json({{"success", false}, {"error", "Unsupported URL"}}).dump();
                }
                debug_log("navigateToUrl: " + url);
                w.dispatch([&w, url]() { w.navigate(url); });
                return json({{"success", true}}).dump();
            }
        } catch (const std::exception& e) {
            debug_log("navigateToUrl: bad args: " + std::string(e.what()));
        }
        return json({{"success", false}}).dump();
    });

    // ─── Binding: navigateBack (back to app) ─────────────────────────────────
    w.bind("navigateBack", [&](const std::string&) -> std::string {
        debug_log("navigateBack called");
        w.dispatch([&w]() {
            w.navigate(g_app_html_path);
            // After navigation, show the back button overlay setup script
            std::string js = R"(
                setTimeout(() => {
                    const overlay = document.getElementById('back-button-overlay');
                    if (overlay) {
                        overlay.classList.remove('hidden');
                        overlay.classList.add('show');
                    }
                    if (pendingGameClaim) {
                        showClaimVerification(pendingGameClaim);
                    }
                }, 100);
            )";
            w.eval(js);
        });
        return json({{"success", true}}).dump();
    });

    // ─── Binding: sendNotification ────────────────────────────────────────────
    w.bind("sendNotification", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (args.is_array() && args.size() >= 2) {
                const std::string title   = args[0].get<std::string>();
                const std::string message = args[1].get<std::string>();
                // Escaping quotes was not enough: the old here-string expanded
                // $(...) subexpressions, so a crafted title could run commands.
                std::thread([title, message]() {
                    show_toast_notification(title, message);
                }).detach();
            }
        } catch (const std::exception& e) {
            debug_log("sendNotification: bad args: " + std::string(e.what()));
        }
        return json({{"success", true}}).dump();
    });

    // ─── Binding: openSystemBrowser (prefer store clients) ───────────────────
    w.bind("openSystemBrowser", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.empty())
                return json({{"success", false}, {"error", "Missing URL"}}).dump();

            std::string url = args[0].get<std::string>();
            std::string platforms = args.size() > 1 ? args[1].get<std::string>() : "";
            if (!is_safe_web_url(url)) {
                debug_log("openSystemBrowser: rejected non-http(s) target");
                return json({{"success", false}, {"error", "Unsupported URL"}}).dump();
            }
            bool opened = false;

            if (platforms.find("Steam") != std::string::npos && is_protocol_registered("steam")) {
                opened = shell_open("steam://openurl/" + url);
                if (opened) return json({{"success", true}, {"handler", "steam"}}).dump();
            }

            if (!opened && platforms.find("Epic") != std::string::npos &&
                is_protocol_registered("com.epicgames.launcher")) {
                std::string slug = extract_epic_slug(url);
                if (!slug.empty()) {
                    opened = shell_open("com.epicgames.launcher://store/en-US/p/" + slug);
                    if (opened) return json({{"success", true}, {"handler", "epic"}}).dump();
                }
            }

            opened = shell_open(url);
            return json({{"success", opened}, {"handler", "browser"}}).dump();
        } catch (const std::exception& e) {
            return json({{"success", false}, {"error", e.what()}}).dump();
        }
    });

    // ─── Binding: registerUser (email + password) ─────────────────────────────
    w.bind("registerUser", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 3)
                return json({{"error", "Invalid args"}}).dump();

            std::string username = args[0].get<std::string>();
            std::string email    = args[1].get<std::string>();
            std::string password = args[2].get<std::string>();

            // Validate input
            if (username.empty() || email.empty() || password.empty())
                return json({{"error", "All fields required"}}).dump();

            if (username.length() < 3)
                return json({{"error", "Username must be at least 3 characters"}}).dump();

            if (password.length() < 6)
                return json({{"error", "Password must be at least 6 characters"}}).dump();

            // Email validation
            if (!is_valid_email(email))
                return json({{"error", "Invalid email format"}}).dump();

            // Salt from the OS CSPRNG, then a deliberately slow KDF.
            std::string salt = secure_random_hex(16);
            if (salt.empty())
                return json({{"error", "Could not generate a secure salt"}}).dump();
            std::string hash = pbkdf2_sha256_hex(password, salt, kPbkdf2Iterations);
            if (hash.empty())
                return json({{"error", "Password hashing failed"}}).dump();

            const char* sql = "INSERT INTO users (username, email, password_hash, password_salt, password_algo, password_iterations, provider) VALUES (?, ?, ?, ?, ?, ?, 'local')";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, salt.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, kAlgoPbkdf2, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 6, (int)kPbkdf2Iterations);
            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            if (rc == SQLITE_CONSTRAINT)
                return json({{"error", "Email already registered"}}).dump();
            if (rc != SQLITE_DONE)
                return json({{"error", "Registration failed"}}).dump();

            int64_t uid = sqlite3_last_insert_rowid(g_db);
            json user = {
                {"id", uid},
                {"name", username},
                {"email", email},
                {"provider", "local"},
                {"avatar", ""},
                {"db_id", uid},
                {"bio", ""},
                {"country", ""},
                {"favorite_platform", ""}
            };
            return json({{"success", true}, {"user", user}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: loginUser (email + password) ────────────────────────────────
    w.bind("loginUser", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            std::string email    = args[0].get<std::string>();
            std::string password = args[1].get<std::string>();

            // Email validation
            if (!is_valid_email(email))
                return json({{"error", "Invalid email format"}}).dump();

            // Fetch the stored credential and verify it here, rather than matching
            // the hash inside the SQL. That allows a legacy row to be re-hashed
            // with the current KDF the first time its owner signs in.
            const char* sql = "SELECT id, username, email, provider, avatar_url, bio, country, "
                              "favorite_platform, password_hash, password_salt, password_algo, "
                              "password_iterations FROM users WHERE email=?";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();
            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);

            json user;
            std::string stored_hash, salt, algo;
            int iterations = 0;
            int user_db_id = -1;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                user_db_id = sqlite3_column_int(stmt, 0);
                user["db_id"] = user_db_id;
                user["id"] = user_db_id;
                user["name"] = column_str(stmt, 1);
                user["email"] = column_str(stmt, 2);
                user["provider"] = column_str(stmt, 3);
                user["avatar"] = column_str(stmt, 4);
                user["bio"] = column_str(stmt, 5);
                user["country"] = column_str(stmt, 6);
                user["favorite_platform"] = column_str(stmt, 7);
                stored_hash = column_str(stmt, 8);
                salt        = column_str(stmt, 9);
                algo        = column_str(stmt, 10);
                iterations  = sqlite3_column_int(stmt, 11);
            }
            sqlite3_finalize(stmt);

            // Same generic message whether the address is unknown or the password
            // is wrong, so this cannot be used to enumerate registered accounts.
            const char* kBadCreds = "Invalid email or password";
            if (user.empty() || stored_hash.empty())
                return json({{"error", kBadCreds}}).dump();

            const bool legacy = (algo.empty() || algo == kAlgoLegacy);
            const std::string candidate = legacy
                ? sha256_hex(password, salt)
                : pbkdf2_sha256_hex(password, salt,
                                    iterations > 0 ? (uint32_t)iterations : kPbkdf2Iterations);
            if (candidate.empty() || !constant_time_equals(candidate, stored_hash))
                return json({{"error", kBadCreds}}).dump();

            if (legacy) {
                // Correct password against an old SHA-256 row: transparently
                // migrate it to PBKDF2 with a fresh salt.
                const std::string new_salt = secure_random_hex(16);
                const std::string new_hash = new_salt.empty() ? std::string()
                    : pbkdf2_sha256_hex(password, new_salt, kPbkdf2Iterations);
                if (!new_hash.empty()) {
                    const char* upd = "UPDATE users SET password_hash=?, password_salt=?, "
                                      "password_algo=?, password_iterations=? WHERE id=?";
                    sqlite3_stmt* ustmt = nullptr;
                    if (sqlite3_prepare_v2(g_db, upd, -1, &ustmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(ustmt, 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(ustmt, 2, new_salt.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(ustmt, 3, kAlgoPbkdf2, -1, SQLITE_STATIC);
                        sqlite3_bind_int(ustmt, 4, (int)kPbkdf2Iterations);
                        sqlite3_bind_int(ustmt, 5, user_db_id);
                        sqlite3_step(ustmt);
                        sqlite3_finalize(ustmt);
                        debug_log("Upgraded a stored password to PBKDF2");
                    }
                }
            }

            return json({{"success", true}, {"user", user}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: startOAuth ──────────────────────────────────────────────────
    w.bind("startOAuth", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 1)
                return json({{"error", "No provider"}}).dump();

            std::string provider = args[0].get<std::string>();
            const bool is_google = provider == "google";
            if (!is_google && provider != "discord")
                return json({{"error", "Unknown provider"}}).dump();

            json cfg = g_oauth_config.value(provider, json::object());
            if (!cfg.contains("client_id") || !cfg.contains("redirect_uri"))
                return json({{"error", "OAuth is not configured for " + provider}}).dump();

            std::string state = generate_random_state();
            if (state.empty())
                return json({{"error", "Could not generate a secure state token"}}).dump();

            // PKCE (RFC 7636). A desktop app cannot keep a client_secret secret,
            // so the authorization code is bound to a one-time verifier that never
            // leaves this process instead of relying on the shipped secret.
            const std::string code_verifier = secure_random_hex(32);
            if (code_verifier.empty())
                return json({{"error", "Could not generate a PKCE verifier"}}).dump();
            const std::string code_challenge = base64url_encode(sha256_raw(code_verifier));

            std::string auth_url = std::string(is_google
                    ? "https://accounts.google.com/o/oauth2/v2/auth?"
                    : "https://discord.com/api/oauth2/authorize?")
                + "client_id=" + url_encode(cfg.value("client_id", ""))
                + "&redirect_uri=" + url_encode(cfg.value("redirect_uri", ""))
                + "&response_type=code"
                + "&scope=" + url_encode(is_google ? "openid email profile" : "identify email")
                + "&code_challenge=" + url_encode(code_challenge)
                + "&code_challenge_method=S256"
                + "&state=" + state;

            // Record what the callback must match, then make sure a server is listening
            {
                std::lock_guard<std::mutex> lock(g_oauth_mutex);
                g_oauth_expected_provider = provider;
                g_oauth_expected_state = state;
                g_oauth_code_verifier = code_verifier;
            }
            bool not_running = false;
            if (g_oauth_server_running.compare_exchange_strong(not_running, true)) {
                std::thread(run_oauth_callback_server, &w).detach();
            }

            // Navigate webview to OAuth URL
            w.dispatch([&w, auth_url]() { w.navigate(auth_url); });

            return json({{"success", true}, {"auth_url", auth_url}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: dbClaimGame ────────────────────────────────────────────────
    w.bind("dbClaimGame", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            int user_id_int = args[0].get<int>();
            const json& game = args[1];

            int game_id = game.value("id", -1);
            std::string title = game.value("title", "");
            std::string thumb = game.value("thumbnail", "");
            std::string plat  = game.value("platforms", "");
            std::string worth = game.value("worth_inr", "");
            std::string gurl  = game.value("open_giveaway_url", "");

            debug_log("dbClaimGame: UserID=" + std::to_string(user_id_int) + ", GameID=" + std::to_string(game_id) + ", Title=" + title);

            const char* sql = R"(
                INSERT OR IGNORE INTO claimed_games
                (user_id, game_id, title, thumbnail, platforms, worth_inr, open_giveaway_url)
                VALUES (?, ?, ?, ?, ?, ?, ?)
            )";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_int(stmt, 1, user_id_int);
            sqlite3_bind_int(stmt, 2, game_id);
            sqlite3_bind_text(stmt, 3, title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, thumb.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, plat.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, worth.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 7, gurl.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            return json({{"success", true}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: getClaimedGames ─────────────────────────────────────────────
    w.bind("getClaimedGames", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 1)
                return json::array().dump();

            int user_id_int = args[0].get<int>();

            const char* sql = R"(
                SELECT game_id, title, thumbnail, platforms, worth_inr, open_giveaway_url, claimed_at
                FROM claimed_games WHERE user_id=? ORDER BY claimed_at DESC
            )";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json::array().dump();

            sqlite3_bind_int(stmt, 1, user_id_int);
            json results = json::array();
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                json g;
                g["id"] = sqlite3_column_int(stmt, 0);
                g["title"] = column_str(stmt, 1);
                g["thumbnail"] = column_str(stmt, 2);
                g["platforms"] = column_str(stmt, 3);
                g["worth_inr"] = column_str(stmt, 4);
                g["open_giveaway_url"] = column_str(stmt, 5);
                g["claimed_at"] = column_str(stmt, 6);
                results.push_back(g);
            }
            sqlite3_finalize(stmt);
            return results.dump();
        } catch (...) {
            return json::array().dump();
        }
    });

    // ─── Binding: getUserProfile ──────────────────────────────────────────────
    w.bind("getUserProfile", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 1)
                return json({{"error", "Invalid args - no user_id provided"}}).dump();

            int user_id_int = args[0].get<int>();
            debug_log("getUserProfile called for user_id: " + std::to_string(user_id_int));

            if (user_id_int <= 0)
                return json({{"error", "Invalid user_id: " + std::to_string(user_id_int)}}).dump();

            const char* sql = R"(
                SELECT id, username, email, avatar_url, bio, country, favorite_platform, created_at, provider
                FROM users WHERE id=?
            )";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                debug_log("DB prepare error: " + std::string(sqlite3_errmsg(g_db)));
                return json({{"error", "DB error: " + std::string(sqlite3_errmsg(g_db))}}).dump();
            }

            sqlite3_bind_int(stmt, 1, user_id_int);
            json profile = json::object();

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                profile["id"] = sqlite3_column_int(stmt, 0);
                profile["username"] = column_str(stmt, 1);
                profile["email"] = column_str(stmt, 2);
                profile["avatar"] = column_str(stmt, 3);
                profile["bio"] = column_str(stmt, 4);
                profile["country"] = column_str(stmt, 5);
                profile["favorite_platform"] = column_str(stmt, 6);
                profile["created_at"] = column_str(stmt, 7);
                profile["provider"] = column_str(stmt, 8);
            }
            sqlite3_finalize(stmt);

            if (profile.empty()) {
                return json({{"error", "User not found"}}).dump();
            }
            return json({{"success", true}, {"profile", profile}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: checkForUpdate (async) ──────────────────────────────────────
    // Arg: [manual]. Automatic checks are skipped in dev builds; a manual check
    // from the menu always runs. The GitHub request runs on a worker thread so
    // the window stays responsive; resolve() marshals the reply back.
    w.bind("checkForUpdate", [&](const std::string& id, const std::string& req, void*) {
        bool manual = false;
        const json args = json::parse(req, nullptr, false);
        if (args.is_array() && !args.empty() && args[0].is_boolean()) manual = args[0].get<bool>();

        const char* force = std::getenv("GAMESTASH_UPDATE_CHECK");
        const bool forced = force && std::string(force) == "1";
        if (g_is_dev_build && !manual && !forced) {
            w.resolve(id, 0, json({{"current", GS_VERSION_STRING}, {"skipped", "dev-build"}}).dump());
            return;
        }
        std::thread([&w, id]() {
            json result = check_for_update();
            debug_log("Update check: current " + std::string(GS_VERSION_STRING) +
                      ", latest " + result.value("latest", std::string("?")) +
                      (result.value("update_available", false) ? " (update available)" : "") +
                      (result.contains("error") ? " error: " + result.value("error", std::string()) : ""));
            if (!g_shutting_down) w.resolve(id, 0, result.dump());
        }).detach();
    }, nullptr);

    // ─── Binding: openUpdateDownload ──────────────────────────────────────────
    // Takes no URL from the page: it opens only the link the last check found
    // and validated, so script in the WebView cannot choose what gets opened.
    w.bind("openUpdateDownload", [&](const std::string&) -> std::string {
        std::string url;
        {
            std::lock_guard<std::mutex> lock(g_update_mutex);
            url = g_update_download_url;
        }
        if (url.empty() || !is_own_release_url(url))
            return json({{"success", false}, {"error", "No update to download"}}).dump();
        return json({{"success", shell_open(url)}}).dump();
    });

    // ─── Binding: updateUserProfile ───────────────────────────────────────────
    w.bind("updateUserProfile", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            int user_id_int = args[0].get<int>();
            const json& profile_data = args[1];

            std::string bio = profile_data.value("bio", "");
            std::string country = profile_data.value("country", "");
            std::string fav_platform = profile_data.value("favorite_platform", "");
            std::string avatar = profile_data.value("avatar", "");

            const char* sql = R"(
                UPDATE users
                SET bio=?, country=?, favorite_platform=?, avatar_url=?, updated_at=CURRENT_TIMESTAMP
                WHERE id=?
            )";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, bio.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, country.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, fav_platform.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, avatar.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 5, user_id_int);

            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            if (rc != SQLITE_DONE)
                return json({{"error", "Failed to update profile"}}).dump();

            return json({{"success", true}, {"message", "Profile updated successfully"}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Back Button Injection for External Sites ───────────────────────────
    w.init(R"(
        window.addEventListener('load', function() {
            if (window.location.protocol !== 'file:') {
                setInterval(function() {
                    if (!document.getElementById('gamestash-back-btn')) {
                        var btn = document.createElement('button');
                        btn.id = 'gamestash-back-btn';
                        btn.innerHTML = '&#8592; Back to Game Stash';
                        btn.style.position = 'fixed';
                        btn.style.top = '16px';
                        btn.style.left = '16px';
                        btn.style.zIndex = '2147483647';
                        btn.style.padding = '12px 24px';
                        btn.style.background = '#00d8ff';
                        btn.style.color = '#000';
                        btn.style.border = 'none';
                        btn.style.borderRadius = '8px';
                        btn.style.fontWeight = 'bold';
                        btn.style.fontFamily = 'sans-serif';
                        btn.style.cursor = 'pointer';
                        btn.style.boxShadow = '0 4px 15px rgba(0,0,0,0.5)';
                        btn.onclick = function() {
                            btn.innerHTML = 'Returning...';
                            if (window.navigateBack) window.navigateBack();
                        };
                        document.documentElement.appendChild(btn);
                    }
                }, 500);
            }
        });
    )");

    // ─── Navigate and run ─────────────────────────────────────────────────────
    debug_log("About to navigate to HTML and start webview...");
    try {
        w.navigate(g_app_html_path);
        debug_log("Navigation successful, starting message loop...");
        w.run();
        g_shutting_down = true;  // detached workers must not resolve into a dead window
        debug_log("Message loop ended normally");
    } catch (const std::exception& e) {
        debug_log("ERROR: WebView crashed with exception: " + std::string(e.what()));
        // Keep window open for 5 seconds so user can see the error
        std::this_thread::sleep_for(std::chrono::seconds(5));
    } catch (...) {
        debug_log("ERROR: WebView crashed with unknown exception");
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    if (g_db) sqlite3_close(g_db);
    debug_log("Application exiting normally");
    return 0;
}
