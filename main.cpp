#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
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
std::ofstream g_debug_log("debug.log", std::ios::app);
void debug_log(const std::string& msg) {
    g_debug_log << "[" << std::chrono::system_clock::now().time_since_epoch().count() << "] " << msg << std::endl;
    g_debug_log.flush();
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

// ─── Supabase Configuration ───────────────────────────────────────────────────
// Replace with your Supabase project URL and anon key
const std::string SUPABASE_URL = "https://dzhsobheihoickgvwyqt.supabase.co";
const std::string SUPABASE_ANON_KEY = "sb_anon_eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
const std::string SUPABASE_API_VERSION = "2024-01-01";
std::string g_user_id = "";  // Set after login
std::mutex g_supabase_mutex;

// OAuth local server state
std::atomic<bool> g_oauth_server_running{false};
std::string g_pending_oauth_provider;
std::string g_oauth_code;
std::string g_oauth_state;
std::mutex g_oauth_mutex;

// ─── SHA-256 Password Hashing (Windows CryptoAPI) ─────────────────────────────
std::string sha256_hex(const std::string& input) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;

    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return "";
    }
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return "";
    }
    if (!CryptHashData(hHash, (BYTE*)input.c_str(), (DWORD)input.size(), 0)) {
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
    debug_log("Database initialized OK");
    return true;
}

// ─── Check if file exists ─────────────────────────────────────────────────────
bool file_exists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
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
    WinHttpSendRequest(hRequest, headers.c_str(), -1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
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
                         const std::string& content_type = "application/x-www-form-urlencoded") {
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

    std::wstring wct(content_type.begin(), content_type.end());
    std::wstring headers = L"Content-Type: " + wct + L"\r\nAccept: application/json\r\n";
    WinHttpSendRequest(hRequest, headers.c_str(), -1,
        (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
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

// ─── Supabase Integration ─────────────────────────────────────────────────────

// Sync claimed games to Supabase
bool supabase_sync_claimed_games(const std::string& user_id) {
    if (user_id.empty() || g_user_id.empty()) return false;
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    
    // Get all claimed games for this user from local database
    std::string sql = "SELECT game_id, title, platforms, worth_inr, open_giveaway_url, claimed_at FROM claimed_games WHERE user_id = ?";
    sqlite3_stmt* stmt;
    
    if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        debug_log("Failed to prepare claimed games query");
        return false;
    }
    
    json games_array = json::array();
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        json game_obj = {
            {"game_id", sqlite3_column_int(stmt, 0)},
            {"title", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
            {"platforms", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
            {"worth_inr", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))},
            {"open_giveaway_url", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))},
            {"claimed_at", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))}
        };
        games_array.push_back(game_obj);
    }
    
    sqlite3_finalize(stmt);
    
    // Send to Supabase REST API
    std::string url = SUPABASE_URL + "/rest/v1/claimed_games";
    
    for (const auto& game : games_array) {
        json payload = {
            {"user_id", user_id},
            {"game_id", game["game_id"]},
            {"title", game["title"]},
            {"platforms", game["platforms"]},
            {"worth_inr", game["worth_inr"]},
            {"open_giveaway_url", game["open_giveaway_url"]},
            {"claimed_at", game["claimed_at"]}
        };
        
        std::string headers = "Authorization: Bearer " + SUPABASE_ANON_KEY + "\r\nContent-Type: application/json\r\n";
        std::string response = winhttp_post(url, payload.dump(), headers);
        debug_log("Supabase sync response: " + response);
    }
    
    return true;
}

// Sync user profile to Supabase
bool supabase_sync_user_profile(const std::string& user_id, const json& profile_data) {
    if (user_id.empty()) return false;
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    
    std::string url = SUPABASE_URL + "/rest/v1/user_profiles?user_id=eq." + user_id;
    
    json payload = {
        {"user_id", user_id},
        {"bio", profile_data.value("bio", "")},
        {"country", profile_data.value("country", "")},
        {"favorite_platform", profile_data.value("favorite_platform", "")}
    };
    
    std::string headers = "Authorization: Bearer " + SUPABASE_ANON_KEY + "\r\nContent-Type: application/json\r\n";
    std::string response = winhttp_post(url, payload.dump(), headers);
    debug_log("Supabase profile sync response: " + response);
    
    return true;
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
    
    debug_log("App Directory: " + g_app_dir);
    
    // Determine the resource directory
    // If running from build folder during development, use parent directory for resources
    g_resource_dir = g_app_dir;
    if (file_exists(g_app_dir + "\\..\\index.html")) {
        // Running from build folder - use parent directory
        g_resource_dir = g_app_dir + "\\..";
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
                std::string cmd = "powershell -WindowStyle Hidden -Command \""
                    "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null; "
                    "$APP_ID = 'GameStash'; "
                    "$template = @\\\"<toast><visual><binding template='ToastText02'><text id='1'>" + title + "</text><text id='2'>" + message + "</text></binding></visual></toast>\\\"; "
                    "$xml = New-Object Windows.Data.Xml.Dom.XmlDocument; "
                    "$xml.LoadXml($template); "
                    "$toast = New-Object Windows.UI.Notifications.ToastNotification $xml; "
                    "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($APP_ID).Show($toast)\"";
                std::thread([cmd]() { system(cmd.c_str()); }).detach();
            }
        } catch (...) {}
        return json({{"success", true}}).dump();
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

            std::string hash = sha256_hex(password);

            const char* sql = "INSERT INTO users (username, email, password_hash, provider) VALUES (?, ?, ?, 'local')";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, hash.c_str(), -1, SQLITE_TRANSIENT);
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
            
            std::string hash = sha256_hex(password);

            const char* sql = "SELECT id, username, email, provider, avatar_url, bio, country, favorite_platform FROM users WHERE email=? AND password_hash=?";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB error"}}).dump();

            sqlite3_bind_text(stmt, 1, email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, hash.c_str(), -1, SQLITE_TRANSIENT);

            json user;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                user["db_id"] = sqlite3_column_int(stmt, 0);
                user["id"] = sqlite3_column_int(stmt, 0);
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

    // ─── Binding: dbClaimGame ─────────────────────────────────────────────────
    w.bind("dbClaimGame", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            int user_id = args[0].get<int>();
            json game = args[1];

            const char* sql = R"(
                INSERT OR IGNORE INTO claimed_games
                (user_id, game_id, title, thumbnail, platforms, worth_inr, open_giveaway_url)
                VALUES (?, ?, ?, ?, ?, ?, ?)
            )";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json({{"error", "DB prepare error"}}).dump();

            int game_id = game.value("id", -1);
            std::string title = game.value("title", "");
            std::string thumb = game.value("thumbnail", "");
            std::string plat  = game.value("platforms", "");
            std::string worth = game.value("worth_inr", "");
            std::string gurl  = game.value("open_giveaway_url", "");

            sqlite3_bind_int(stmt, 1, user_id);
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

            int user_id = args[0].get<int>();
            const char* sql = R"(
                SELECT game_id, title, thumbnail, platforms, worth_inr, open_giveaway_url, claimed_at
                FROM claimed_games WHERE user_id=? ORDER BY claimed_at DESC
            )";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return json::array().dump();

            sqlite3_bind_int(stmt, 1, user_id);
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

    // ─── Binding: getUserProfile ───────────────────────────────────────────────
    w.bind("getUserProfile", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 1)
                return json({{"error", "Invalid args - no user_id provided"}}).dump();

            int user_id = args[0].get<int>();
            debug_log("getUserProfile called for user_id: " + std::to_string(user_id));
            
            if (user_id <= 0)
                return json({{"error", "Invalid user_id: " + std::to_string(user_id)}}).dump();
            
            const char* sql = R"(
                SELECT id, username, email, avatar_url, bio, country, favorite_platform, created_at, provider
                FROM users WHERE id=?
            )";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                debug_log("DB prepare error: " + std::string(sqlite3_errmsg(g_db)));
                return json({{"error", "DB error: " + std::string(sqlite3_errmsg(g_db))}}).dump();
            }

            sqlite3_bind_int(stmt, 1, user_id);
            json profile = json::object();

            int step_result = sqlite3_step(stmt);
            debug_log("Query step result: " + std::to_string(step_result) + " (SQLITE_ROW=100)");
            
            if (step_result == SQLITE_ROW) {
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
                debug_log("User found: " + profile["username"].get<std::string>());
            } else {
                debug_log("User not found for id: " + std::to_string(user_id));
            }
            sqlite3_finalize(stmt);

            if (profile.empty()) {
                debug_log("Profile is empty - returning error");
                return json({{"error", "User not found"}}).dump();
            }
            return json({{"success", true}, {"profile", profile}}).dump();
        } catch (const std::exception& e) {
            debug_log("Exception in getUserProfile: " + std::string(e.what()));
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: updateUserProfile ────────────────────────────────────────────
    w.bind("updateUserProfile", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            int user_id = args[0].get<int>();
            json profile_data = args[1];

            // Validate email if being changed
            if (profile_data.contains("email") && !profile_data["email"].is_null()) {
                std::string new_email = profile_data["email"].get<std::string>();
                if (!is_valid_email(new_email))
                    return json({{"error", "Invalid email format"}}).dump();
            }

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
            sqlite3_bind_int(stmt, 5, user_id);

            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            if (rc != SQLITE_DONE)
                return json({{"error", "Update failed"}}).dump();

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
    w.navigate(g_app_html_path);
    w.run();

    if (g_db) sqlite3_close(g_db);
    return 0;
}
