#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cctype>
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

using json = nlohmann::json;

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <wincrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")

// SQLite
#include <sqlite3.h>

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

void debug_log(const std::string& msg) {
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

// ─── Random State Generator for OAuth ─────────────────────────────────────────
std::string generate_random_state() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    const char hex[] = "0123456789abcdef";
    std::string result(32, '0');
    for (auto& c : result) c = hex[dis(gen)];
    return result;
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
    int rc = sqlite3_open(g_db_path.c_str(), &g_db);
    if (rc != SQLITE_OK) {
        debug_log("Cannot open database: " + std::string(sqlite3_errmsg(g_db)));
        return false;
    }
    debug_log("Database opened successfully at: " + g_db_path);

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

    // Migration: ensure password_salt column exists
    sqlite3_exec(g_db, "ALTER TABLE users ADD COLUMN password_salt TEXT;", nullptr, nullptr, nullptr);

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

        std::string expected_provider, expected_state;
        {
            std::lock_guard<std::mutex> lock(g_oauth_mutex);
            expected_provider = g_oauth_expected_provider;
            expected_state = g_oauth_expected_state;
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
            const std::string body = "code=" + url_encode(code)
                + "&client_id=" + url_encode(cfg.value("client_id", ""))
                + "&client_secret=" + url_encode(cfg.value("client_secret", ""))
                + "&redirect_uri=" + url_encode(cfg.value("redirect_uri", ""))
                + "&grant_type=authorization_code";
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

            std::string js = "handleOAuthResult(" + result.dump() + ");";
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
            httplib::Client cli("http://www.gamerpower.com");
            cli.set_follow_location(true);
            cli.set_connection_timeout(10, 0);
            cli.set_read_timeout(10, 0);
            httplib::Headers headers = {{"User-Agent", "Mozilla/5.0"}};
            auto res = cli.Get("/api/giveaways", headers);
            if (!res) return json({{"error", "No response from API"}}).dump();
            if (res->status != 200) return json({{"error", "HTTP " + std::to_string(res->status)}}).dump();

            json response_json = json::parse(res->body);
            json filtered = json::array();
            for (const auto& item : response_json) {
                std::string platforms = item.value("platforms", "N/A");
                bool ok = platforms.find("Steam") != std::string::npos ||
                          platforms.find("Epic Games") != std::string::npos ||
                          platforms.find("GOG") != std::string::npos ||
                          platforms.find("Itch.io") != std::string::npos ||
                          platforms.find("itchio") != std::string::npos ||
                          platforms.find("Itchio") != std::string::npos;
                if (!ok) continue;
                json ni = item;
                ni["worth_inr"] = get_worth_in_inr(item.value("worth", "N/A"));
                filtered.push_back(ni);
            }
            debug_log("Returning " + std::to_string(filtered.size()) + " giveaways");
            return filtered.dump();
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
                std::string title = args[0].get<std::string>();
                std::string message = args[1].get<std::string>();

                // Security: Escape single quotes for PowerShell to prevent command injection
                auto escape_ps = [](const std::string& s) {
                    std::string res;
                    for (char c : s) {
                        if (c == '\'') res += "''";
                        else if (c == '\"') res += "`\"";
                        else if (c == '`') res += "``";
                        else res += c;
                    }
                    return res;
                };

                std::string e_title = escape_ps(title);
                std::string e_message = escape_ps(message);

                std::string cmd = "powershell -WindowStyle Hidden -Command \""
                    "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null; "
                    "$APP_ID = 'GameStash'; "
                    "$template = @\\\"<toast><visual><binding template='ToastText02'><text id='1'>" + e_title + "</text><text id='2'>" + e_message + "</text></binding></visual></toast>\\\"; "
                    "$xml = New-Object Windows.Data.Xml.Dom.XmlDocument; "
                    "$xml.LoadXml($template); "
                    "$toast = New-Object Windows.UI.Notifications.ToastNotification $xml; "
                    "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($APP_ID).Show($toast)\"";
                std::thread([cmd]() { system(cmd.c_str()); }).detach();
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

            // Generate a random salt
            std::string salt = generate_random_state();
            std::string hash = sha256_hex(password, salt);

            const char* sql = "INSERT INTO users (username, email, password_hash, password_salt, provider) VALUES (?, ?, ?, ?, 'local')";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, salt.c_str(), -1, SQLITE_TRANSIENT);
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

            // First, get the salt for this email
            std::string salt;
            const char* salt_sql = "SELECT password_salt FROM users WHERE email=?";
            sqlite3_stmt* salt_stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, salt_sql, -1, &salt_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(salt_stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(salt_stmt) == SQLITE_ROW) {
                    salt = column_str(salt_stmt, 0);
                }
                sqlite3_finalize(salt_stmt);
            }

            std::string hash = sha256_hex(password, salt);

            const char* sql = "SELECT id, username, email, provider, avatar_url, bio, country, favorite_platform FROM users WHERE email=? AND password_hash=?";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);

            json user;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int user_db_id = sqlite3_column_int(stmt, 0);
                user["db_id"] = user_db_id;
                user["id"] = user_db_id;
                user["name"] = column_str(stmt, 1);
                user["email"] = column_str(stmt, 2);
                user["provider"] = column_str(stmt, 3);
                user["avatar"] = column_str(stmt, 4);
                user["bio"] = column_str(stmt, 5);
                user["country"] = column_str(stmt, 6);
                user["favorite_platform"] = column_str(stmt, 7);
            }
            sqlite3_finalize(stmt);

            if (user.empty()) return json({{"error", "Invalid email or password"}}).dump();

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
            std::string auth_url = std::string(is_google
                    ? "https://accounts.google.com/o/oauth2/v2/auth?"
                    : "https://discord.com/api/oauth2/authorize?")
                + "client_id=" + url_encode(cfg.value("client_id", ""))
                + "&redirect_uri=" + url_encode(cfg.value("redirect_uri", ""))
                + "&response_type=code"
                + "&scope=" + url_encode(is_google ? "openid email profile" : "identify email")
                + "&state=" + state;

            // Record what the callback must match, then make sure a server is listening
            {
                std::lock_guard<std::mutex> lock(g_oauth_mutex);
                g_oauth_expected_provider = provider;
                g_oauth_expected_state = state;
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
