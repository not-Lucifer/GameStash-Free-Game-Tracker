#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
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
        g_debug_log << "[" << std::chrono::system_clock::now().time_since_epoch().count() << "] " << msg << std::endl;
        g_debug_log.flush();
    }
    std::cerr << msg << std::endl;
}

// ─── Global State ─────────────────────────────────────────────────────────────
sqlite3* g_db = nullptr;
std::string g_app_html_path;
std::string g_oauth_config_path;
std::string g_app_dir;
std::string g_resource_dir;
std::string g_db_path;
json g_oauth_config;

// OAuth local server state
std::atomic<bool> g_oauth_server_running{false};
std::string g_pending_oauth_provider;
std::string g_oauth_code;
std::string g_oauth_state;
std::mutex g_oauth_mutex;

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
    if (!CryptHashData(hHash, (BYTE*)salted_input.c_str(), (DWORD)salted_input.size(), 0)) {
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
    for (char c : local) {
        if (!isalnum(c) && c != '.' && c != '_' && c != '-' && c != '+') 
            return false;
    }
    
    // Check for domain part
    std::string domain = email.substr(at_pos + 1);
    if (domain.find("..") != std::string::npos) return false;
    if (domain.find('.') == std::string::npos) return false;
    
    size_t last_dot = domain.rfind('.');
    std::string tld = domain.substr(last_dot + 1);
    if (tld.length() < 2) return false;
    
    for (char c : domain) {
        if (!isalnum(c) && c != '.' && c != '-') 
            return false;
    }
    
    return true;
}

// ─── URL Encode ───────────────────────────────────────────────────────────────
std::string url_encode(const std::string& s) {
    std::ostringstream oss;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
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
    size_t lastSlash = exePath.find_last_of("\\");
    if (lastSlash != std::string::npos) {
        return exePath.substr(0, lastSlash);
    }
    return "";
}

// ─── Get AppData Roaming Folder ───────────────────────────────────────────────
std::string get_appdata_folder() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, path))) {
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
    // Use CreateDirectoryA which creates single level only
    // For nested creation, we need to check parent first
    size_t pos = 0;
    while ((pos = path.find('\\', pos)) != std::string::npos) {
        std::string dir = path.substr(0, pos++);
        if (!dir.empty() && !CreateDirectoryA(dir.c_str(), NULL)) {
            // Directory might already exist, which is OK
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                // Might fail for other reasons, but continue trying
            }
        }
    }
    // Create the final directory
    return CreateDirectoryA(path.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
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
std::string get_worth_in_inr(const std::string& worth_usd) {
    if (worth_usd == "N/A") return "N/A";
    if (!worth_usd.empty() && worth_usd[0] == '$') {
        try {
            double usd_val = std::stod(worth_usd.substr(1));
            double inr_val = usd_val * 83.5;
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(2) << "Rs. " << inr_val;
            return stream.str();
        } catch (...) { return worth_usd; }
    }
    return worth_usd;
}

// ─── WinHTTP GET helper ───────────────────────────────────────────────────────
std::string winhttp_get(const std::string& url, const std::string& extra_headers = "") {
    std::wstring wurl(url.begin(), url.end());
    HINTERNET hSession = WinHttpOpen(
        L"GameStash/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256], urlPath[2048];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    std::wstring headers = L"User-Agent: GameStash/1.0\r\nAccept: application/json\r\n";
    if (!extra_headers.empty()) {
        std::wstring wex(extra_headers.begin(), extra_headers.end());
        headers += wex;
    }
    // Use WinHttpAddRequestHeaders to properly add headers before sending
    BOOL headerAdded = WinHttpAddRequestHeaders(hRequest, headers.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!headerAdded) {
        DWORD err = GetLastError();
        debug_log("[WARN] WinHttpAddRequestHeaders GET failed with error: " + std::to_string(err));
    }
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WinHttpReceiveResponse(hRequest, NULL);

    std::string response;
    DWORD dwSize = 0;
    do {
        dwSize = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (dwSize == 0) break;
        std::vector<char> buf(dwSize + 1, 0);
        DWORD dwRead = 0;
        WinHttpReadData(hRequest, buf.data(), dwSize, &dwRead);
        response.append(buf.data(), dwRead);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// ─── WinHTTP POST helper ──────────────────────────────────────────────────────
std::string winhttp_post(const std::string& url, const std::string& body,
                         const std::string& custom_headers = "") {
    std::wstring wurl(url.begin(), url.end());
    HINTERNET hSession = WinHttpOpen(L"GameStash/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t hostName[256], urlPath[2048];
    urlComp.lpszHostName = hostName; urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = urlPath; urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        WinHttpCloseHandle(hSession); return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", urlPath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    // Add headers
    std::string final_headers = custom_headers;
    if (final_headers.empty()) {
        final_headers = "Content-Type: application/json\r\nAccept: application/json\r\n";
    }
    std::wstring wheaders(final_headers.begin(), final_headers.end());
    BOOL headerAdded = WinHttpAddRequestHeaders(hRequest, wheaders.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!headerAdded) {
        DWORD err = GetLastError();
        debug_log("[WARN] WinHttpAddRequestHeaders POST failed with error: " + std::to_string(err));
    }
    
    BOOL sendResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    if (!sendResult) {
        DWORD err = GetLastError();
        debug_log("[ERROR] WinHttpSendRequest POST failed with error: " + std::to_string(err));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "{\"error\":\"SendRequest failed\"}";
    }
    
    BOOL recvResult = WinHttpReceiveResponse(hRequest, NULL);
    if (!recvResult) {
        DWORD err = GetLastError();
        debug_log("[ERROR] WinHttpReceiveResponse POST failed with error: " + std::to_string(err));
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "{\"error\":\"ReceiveResponse failed\"}";
    }

    // Check HTTP status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    debug_log("[INFO] POST HTTP Status Code: " + std::to_string(statusCode));

    std::string response;
    DWORD dwSize = 0;
    do {
        dwSize = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (dwSize == 0) break;
        std::vector<char> buf(dwSize + 1, 0);
        DWORD dwRead = 0;
        WinHttpReadData(hRequest, buf.data(), dwSize, &dwRead);
        response.append(buf.data(), dwRead);
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return response;
}

// ─── OAuth: Start Local Callback Server ───────────────────────────────────────
// Runs in a background thread, listens for OAuth redirect
void start_oauth_server_thread(const std::string& provider, const std::string& state,
                                webview::webview* w, const std::vector<char>& _unused) {
    httplib::Server svr;
    g_oauth_server_running = true;

    std::string callback_path = "/oauth/" + provider + "/callback";

    svr.Get(callback_path.c_str(), [&](const httplib::Request& req, httplib::Response& res) {
        std::string code = req.get_param_value("code");
        std::string recv_state = req.get_param_value("recv_state");

        // Exchange code for tokens + get user info
        json result;
        result["provider"] = provider;
        result["code"] = code;

        // Exchange token
        std::string token_resp;
        json token_json;
        if (provider == "google") {
            std::string body = "code=" + url_encode(code)
                + "&client_id=" + url_encode(g_oauth_config["google"]["client_id"].get<std::string>())
                + "&client_secret=" + url_encode(g_oauth_config["google"]["client_secret"].get<std::string>())
                + "&redirect_uri=" + url_encode(g_oauth_config["google"]["redirect_uri"].get<std::string>())
                + "&grant_type=authorization_code";
            token_resp = winhttp_post("https://oauth2.googleapis.com/token", body);
            try { token_json = json::parse(token_resp); } catch (...) {}

            std::string access_token = token_json.value("access_token", "");
            if (!access_token.empty()) {
                std::string user_resp = winhttp_get("https://www.googleapis.com/oauth2/v2/userinfo",
                    "Authorization: Bearer " + access_token + "\r\n");
                try {
                    json user_json = json::parse(user_resp);
                    result["user_id"] = user_json.value("id", "");
                    result["name"] = user_json.value("name", "Google User");
                    result["email"] = user_json.value("email", "");
                    result["avatar"] = user_json.value("picture", "");
                } catch (...) {}
            }
        } else if (provider == "discord") {
            std::string body = "code=" + url_encode(code)
                + "&client_id=" + url_encode(g_oauth_config["discord"]["client_id"].get<std::string>())
                + "&client_secret=" + url_encode(g_oauth_config["discord"]["client_secret"].get<std::string>())
                + "&redirect_uri=" + url_encode(g_oauth_config["discord"]["redirect_uri"].get<std::string>())
                + "&grant_type=authorization_code";
            token_resp = winhttp_post("https://discord.com/api/oauth2/token", body);
            try { token_json = json::parse(token_resp); } catch (...) {}

            std::string access_token = token_json.value("access_token", "");
            if (!access_token.empty()) {
                std::string user_resp = winhttp_get("https://discord.com/api/users/@me",
                    "Authorization: Bearer " + access_token + "\r\n");
                try {
                    json user_json = json::parse(user_resp);
                    std::string avatar_hash = user_json.value("avatar", "");
                    std::string uid = user_json.value("id", "");
                    std::string avatar_url = avatar_hash.empty() ? "" :
                        "https://cdn.discordapp.com/avatars/" + uid + "/" + avatar_hash + ".png";
                    result["user_id"] = uid;
                    result["name"] = user_json.value("username", "Discord User");
                    result["email"] = user_json.value("email", "");
                    result["avatar"] = avatar_url;
                } catch (...) {}
            }
        }

        // Save user to DB
        int db_user_id = -1;
        if (result.contains("user_id") && !result["user_id"].get<std::string>().empty()) {
            std::string pid = result["user_id"].get<std::string>();
            std::string prov = provider;
            std::string name = result.value("name", "User");
            std::string email = result.value("email", "");
            std::string avatar = result.value("avatar", "");

            // Upsert user
            const char* upsert = R"(
                INSERT INTO users (username, email, provider, provider_id, avatar_url)
                VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(email) DO UPDATE SET
                    username=excluded.username,
                    provider=excluded.provider,
                    provider_id=excluded.provider_id,
                    avatar_url=excluded.avatar_url
            )";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, upsert, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, prov.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, pid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, avatar.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }

            // Get DB id
            const char* sel = "SELECT id FROM users WHERE provider_id=? AND provider=?";
            if (sqlite3_prepare_v2(g_db, sel, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, pid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, prov.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    db_user_id = sqlite3_column_int(stmt, 0);
                }
                sqlite3_finalize(stmt);
            }
        }

        result["db_id"] = db_user_id;
        std::string result_str = result.dump();

        // Return a nice closing page
        res.set_content(R"(<!DOCTYPE html><html><head>
            <style>body{background:#0a0a0f;color:#fff;font-family:Inter,sans-serif;
            display:flex;align-items:center;justify-content:center;height:100vh;margin:0;flex-direction:column;}
            h2{color:#00d8ff;} p{color:#888;}</style></head><body>
            <h2>✓ Login Successful!</h2>
            <p>You can close this window and return to Game Stash.</p>
            </body></html>)", "text/html");

        // Notify the webview via dispatch
        std::string js = "handleOAuthResult(" + result_str + ");";
        w->dispatch([w, js]() { w->eval(js); });

        // Stop server after handling
        svr.stop();
        g_oauth_server_running = false;
    });

    svr.listen("127.0.0.1", 9876);
    g_oauth_server_running = false;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Get absolute path to the app directory from .exe location
    g_app_dir = get_app_directory();
    if (g_app_dir.empty()) {
        MessageBoxW(NULL, L"Failed to determine application directory!", L"GameStash Error", MB_ICONERROR);
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
        MessageBoxW(NULL, L"Failed to determine AppData directory!", L"GameStash Error", MB_ICONERROR);
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
        MessageBoxW(NULL, L"Failed to initialize database!", L"GameStash Error", MB_ICONERROR);
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
    w.bind("fetchGiveaways", [&](const std::string& req) -> std::string {
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
        } catch (...) {}
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
                auto escape_ps = [](std::string s) {
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
        } catch (...) {}
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

    // ─── Binding: testSupabaseConnection (Removed) ─────────────────────────────

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
            sqlite3_stmt* stmt;
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
            std::string salt = "";
            const char* salt_sql = "SELECT password_salt FROM users WHERE email=?";
            sqlite3_stmt* salt_stmt;
            if (sqlite3_prepare_v2(g_db, salt_sql, -1, &salt_stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(salt_stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(salt_stmt) == SQLITE_ROW) {
                    auto s = sqlite3_column_text(salt_stmt, 0);
                    if (s) salt = std::string(reinterpret_cast<const char*>(s));
                }
                sqlite3_finalize(salt_stmt);
            }

            std::string hash = sha256_hex(password, salt);

            const char* sql = "SELECT id, username, email, provider, avatar_url, bio, country, favorite_platform FROM users WHERE email=? AND password_hash=?";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);

            json user;
            int user_db_id = -1;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                user_db_id = sqlite3_column_int(stmt, 0);
                user["db_id"] = user_db_id;
                user["id"] = user_db_id;
                user["name"] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
                user["email"] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
                user["provider"] = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3)));
                auto av = sqlite3_column_text(stmt, 4);
                user["avatar"] = av ? std::string(reinterpret_cast<const char*>(av)) : "";
                auto bio = sqlite3_column_text(stmt, 5);
                user["bio"] = bio ? std::string(reinterpret_cast<const char*>(bio)) : "";
                auto country = sqlite3_column_text(stmt, 6);
                user["country"] = country ? std::string(reinterpret_cast<const char*>(country)) : "";
                auto fav_plat = sqlite3_column_text(stmt, 7);
                user["favorite_platform"] = fav_plat ? std::string(reinterpret_cast<const char*>(fav_plat)) : "";
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
            std::string state = generate_random_state();
            std::string auth_url;

            if (provider == "google") {
                std::string client_id = g_oauth_config["google"]["client_id"].get<std::string>();
                std::string redirect = g_oauth_config["google"]["redirect_uri"].get<std::string>();
                auth_url = "https://accounts.google.com/o/oauth2/v2/auth?"
                    "client_id=" + url_encode(client_id) +
                    "&redirect_uri=" + url_encode(redirect) +
                    "&response_type=code"
                    "&scope=" + url_encode("openid email profile") +
                    "&state=" + state;
            } else if (provider == "discord") {
                std::string client_id = g_oauth_config["discord"]["client_id"].get<std::string>();
                std::string redirect = g_oauth_config["discord"]["redirect_uri"].get<std::string>();
                auth_url = "https://discord.com/api/oauth2/authorize?"
                    "client_id=" + url_encode(client_id) +
                    "&redirect_uri=" + url_encode(redirect) +
                    "&response_type=code"
                    "&scope=" + url_encode("identify email") +
                    "&state=" + state;
            } else {
                return json({{"error", "Unknown provider"}}).dump();
            }

            // Start local callback server in background thread
            if (!g_oauth_server_running) {
                std::thread([provider, state, &w]() {
                    std::vector<char> unused;
                    start_oauth_server_thread(provider, state, &w, unused);
                }).detach();
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
            json game = args[1];

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
            sqlite3_stmt* stmt;
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
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json::array().dump();

            sqlite3_bind_int(stmt, 1, user_id_int);
            json results = json::array();
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                json g;
                g["id"] = sqlite3_column_int(stmt, 0);
                auto col1 = sqlite3_column_text(stmt, 1);
                auto col2 = sqlite3_column_text(stmt, 2);
                auto col3 = sqlite3_column_text(stmt, 3);
                auto col4 = sqlite3_column_text(stmt, 4);
                auto col5 = sqlite3_column_text(stmt, 5);
                auto col6 = sqlite3_column_text(stmt, 6);
                g["title"] = col1 ? (const char*)col1 : "";
                g["thumbnail"] = col2 ? (const char*)col2 : "";
                g["platforms"] = col3 ? (const char*)col3 : "";
                g["worth_inr"] = col4 ? (const char*)col4 : "";
                g["open_giveaway_url"] = col5 ? (const char*)col5 : "";
                g["claimed_at"] = col6 ? (const char*)col6 : "";
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
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                debug_log("DB prepare error: " + std::string(sqlite3_errmsg(g_db)));
                return json({{"error", "DB error: " + std::string(sqlite3_errmsg(g_db))}}).dump();
            }

            sqlite3_bind_int(stmt, 1, user_id_int);
            json profile = json::object();

            if (sqlite3_step(stmt) == SQLITE_ROW) {
                profile["id"] = sqlite3_column_int(stmt, 0);
                auto col1 = sqlite3_column_text(stmt, 1);
                auto col2 = sqlite3_column_text(stmt, 2);
                auto col3 = sqlite3_column_text(stmt, 3);
                auto col4 = sqlite3_column_text(stmt, 4);
                auto col5 = sqlite3_column_text(stmt, 5);
                auto col6 = sqlite3_column_text(stmt, 6);
                auto col7 = sqlite3_column_text(stmt, 7);
                auto col8 = sqlite3_column_text(stmt, 8);
                
                profile["username"] = col1 ? (const char*)col1 : "";
                profile["email"] = col2 ? (const char*)col2 : "";
                profile["avatar"] = col3 ? (const char*)col3 : "";
                profile["bio"] = col4 ? (const char*)col4 : "";
                profile["country"] = col5 ? (const char*)col5 : "";
                profile["favorite_platform"] = col6 ? (const char*)col6 : "";
                profile["created_at"] = col7 ? (const char*)col7 : "";
                profile["provider"] = col8 ? (const char*)col8 : "";
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
            json profile_data = args[1];

            std::string bio = profile_data.contains("bio") ? profile_data["bio"].get<std::string>() : "";
            std::string country = profile_data.contains("country") ? profile_data["country"].get<std::string>() : "";
            std::string fav_platform = profile_data.contains("favorite_platform") ? profile_data["favorite_platform"].get<std::string>() : "";
            std::string avatar = profile_data.contains("avatar") ? profile_data["avatar"].get<std::string>() : "";

            const char* sql = R"(
                UPDATE users 
                SET bio=?, country=?, favorite_platform=?, avatar_url=?, updated_at=CURRENT_TIMESTAMP
                WHERE id=?
            )";
            sqlite3_stmt* stmt;
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
