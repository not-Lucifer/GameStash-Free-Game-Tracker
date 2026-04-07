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
// Loaded from supabase_config.json at startup (not hardcoded!)
std::string g_supabase_url = "";
std::string g_supabase_anon_key = "";
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

// ─── Load Supabase Configuration ──────────────────────────────────────────────
bool load_supabase_config(const std::string& config_path) {
    debug_log("Attempting to load Supabase config from: " + config_path);
    std::string config_content = read_file(config_path);
    if (config_content.empty()) {
        debug_log("Warning: Could not read Supabase config from: " + config_path);
        return false;
    }
    
    try {
        json config = json::parse(config_content);
        if (config.contains("supabase")) {
            g_supabase_url = config["supabase"].value("url", "");
            g_supabase_anon_key = config["supabase"].value("anon_key", "");
            
            if (g_supabase_url.empty() || g_supabase_anon_key.empty()) {
                debug_log("Error: Supabase config incomplete (missing url or anon_key)");
                return false;
            }
            debug_log("✅ Supabase config loaded successfully!");
            debug_log("   URL: " + g_supabase_url);
            debug_log("   Anon key length: " + std::to_string(g_supabase_anon_key.length()));
            return true;
        }
    } catch (const std::exception& e) {
        debug_log("Error parsing Supabase config: " + std::string(e.what()));
    }
    return false;
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

// ─── Supabase Integration ─────────────────────────────────────────────────────

// Sync claimed games to Supabase
bool supabase_sync_claimed_games(const std::string& user_id) {
    if (user_id.empty() || g_user_id.empty() || g_supabase_url.empty()) return false;
    
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
    std::string url = g_supabase_url + "/rest/v1/claimed_games";
    
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
        
        std::string headers = "Authorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
        std::string response = winhttp_post(url, payload.dump(), headers);
        debug_log("Supabase sync response: " + response);
    }
    
    return true;
}

// Sync user profile to Supabase
bool supabase_sync_user_profile(const std::string& user_id, const json& profile_data) {
    if (user_id.empty() || g_supabase_url.empty()) return false;
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    
    std::string url = g_supabase_url + "/rest/v1/user_profiles?user_id=eq." + user_id;
    
    json payload = {
        {"user_id", user_id},
        {"bio", profile_data.value("bio", "")},
        {"country", profile_data.value("country", "")},
        {"favorite_platform", profile_data.value("favorite_platform", "")}
    };
    
    std::string headers = "Authorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
    std::string response = winhttp_post(url, payload.dump(), headers);
    debug_log("Supabase profile sync response: " + response);
    
    return true;
}

// ─── WinHTTP PATCH helper (for updates) ───────────────────────────────────────
std::string winhttp_patch(const std::string& url, const std::string& body,
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
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"PATCH", urlPath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return ""; }

    std::string final_headers = custom_headers;
    if (final_headers.empty()) {
        final_headers = "Content-Type: application/json\r\nAccept: application/json\r\n";
    }
    std::wstring wheaders(final_headers.begin(), final_headers.end());
    BOOL headerAdded = WinHttpAddRequestHeaders(hRequest, wheaders.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!headerAdded) {
        DWORD err = GetLastError();
        debug_log("[WARN] WinHttpAddRequestHeaders PATCH failed with error: " + std::to_string(err));
    }
    WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
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

// ─── Supabase CRUD Operations (Cloud-First) ────────────────────────────────────

// Fetch claimed games from Supabase
json supabase_fetch_claimed_games(const std::string& user_id) {
    if (user_id.empty() || g_supabase_url.empty() || g_supabase_anon_key.empty()) {
        debug_log("supabase_fetch_claimed_games: Missing credentials - user_id empty: " + std::to_string(user_id.empty()) 
            + ", url empty: " + std::to_string(g_supabase_url.empty()) + ", key empty: " + std::to_string(g_supabase_anon_key.empty()));
        return json::array();
    }
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    std::string url = g_supabase_url + "/rest/v1/claimed_games?user_id=eq." + user_id + "&order=claimed_at.desc";
    debug_log("Fetching claimed games from: " + url);
    
    std::string headers = "apikey: " + g_supabase_anon_key + "\r\nAuthorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
    std::string response = winhttp_get(url, headers);
    
    try {
        json result = json::parse(response);
        debug_log("✅ Fetched " + std::to_string(result.size()) + " claimed games from Supabase for user " + user_id);
        return result;
    } catch (...) {
        debug_log("❌ Error parsing claimed games response: " + response);
        return json::array();
    }
}

// Fetch user profile from Supabase
json supabase_fetch_user_profile(const std::string& user_id) {
    if (user_id.empty() || g_supabase_url.empty() || g_supabase_anon_key.empty()) {
        return json::object();
    }
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    std::string url = g_supabase_url + "/rest/v1/user_profiles?user_id=eq." + user_id;
    
    std::string headers = "apikey: " + g_supabase_anon_key + "\r\nAuthorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
    std::string response = winhttp_get(url, headers);
    
    try {
        json result = json::parse(response);
        if (result.is_array() && result.size() > 0) {
            debug_log("Fetched user profile from Supabase for user " + user_id);
            return result[0];
        }
        return json::object();
    } catch (...) {
        debug_log("Error parsing user profile response: " + response);
        return json::object();
    }
}

// Add claimed game to Supabase
bool supabase_add_claimed_game(const std::string& user_id, int game_id, const std::string& title,
                               const std::string& platforms, const std::string& worth_inr,
                               const std::string& open_giveaway_url, const std::string& claimed_at) {
    if (user_id.empty() || g_supabase_url.empty() || g_supabase_anon_key.empty()) {
        debug_log("❌ supabase_add_claimed_game: Missing credentials");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    std::string url = g_supabase_url + "/rest/v1/claimed_games";
    
    json payload = {
        {"user_id", user_id},
        {"game_id", game_id},
        {"title", title},
        {"platforms", platforms},
        {"worth_inr", worth_inr},
        {"open_giveaway_url", open_giveaway_url},
        {"claimed_at", claimed_at}
    };
    
    debug_log("Posting to Supabase: " + url);
    debug_log("Payload: " + payload.dump());
    debug_log("User: " + user_id + ", Game ID: " + std::to_string(game_id));
    
    std::string headers = "apikey: " + g_supabase_anon_key + "\r\nAuthorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
    std::string response = winhttp_post(url, payload.dump(), headers);
    
    debug_log("Raw Response: [" + response + "]");
    debug_log("Response length: " + std::to_string(response.length()));
    debug_log("Response empty? " + std::string(response.empty() ? "YES" : "NO"));
    
    // HTTP 201 Created means the insert was successful (even with empty body)
    // Only treat as failure if response has error message OR is actually empty with bad HTTP status
    if (response.empty()) {
        // Empty response - this means HTTP 201 with no body - its success!
        debug_log("✅ Claimed game successfully added to Supabase (HTTP 201 with empty body) - RETURNING TRUE");
        return true;
    }
    
    // If response has data, check for errors
    bool success = response.find("error") == std::string::npos && response.find("message") == std::string::npos;
    debug_log("Success check: error found? " + std::string(response.find("error") != std::string::npos ? "YES" : "NO") + 
              ", message found? " + std::string(response.find("message") != std::string::npos ? "YES" : "NO"));
    if (success) {
        debug_log("✅ Claimed game successfully added to Supabase - RETURNING TRUE");
    } else {
        debug_log("❌ Supabase returned error/message: " + response + " - RETURNING FALSE");
    }
    return success;
}

// Create or update user profile in Supabase
bool supabase_upsert_user_profile(const std::string& user_id, const std::string& bio = "",
                                   const std::string& country = "", const std::string& favorite_platform = "") {
    if (user_id.empty() || g_supabase_url.empty() || g_supabase_anon_key.empty()) {
        debug_log("❌ supabase_upsert_user_profile: Missing credentials");
        return false;
    }
    
    std::lock_guard<std::mutex> lock(g_supabase_mutex);
    std::string url = g_supabase_url + "/rest/v1/user_profiles?user_id=eq." + user_id;
    
    json payload = {
        {"user_id", user_id},
        {"bio", bio},
        {"country", country},
        {"favorite_platform", favorite_platform},
        {"updated_at", "now()"}
    };
    
    debug_log("Upsert user profile to: " + url);
    debug_log("Payload: " + payload.dump());
    
    std::string headers = "apikey: " + g_supabase_anon_key + "\r\nAuthorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
    
    // Try PATCH (update) first
    std::string response = winhttp_patch(url, payload.dump(), headers);
    debug_log("PATCH response: [" + response + "]");
    debug_log("PATCH response length: " + std::to_string(response.length()));
    
    // Empty response on PATCH usually means HTTP 204 No Content - success!
    if (response.empty()) {
        debug_log("✅ User profile successfully patched to Supabase (HTTP 204 with empty body)");
        return true;
    }
    
    // If response has error message, try POST (insert new)
    if (response.find("error") != std::string::npos || response.find("message") != std::string::npos) {
        debug_log("PATCH not found or failed, trying POST (insert)...");
        std::string insert_url = g_supabase_url + "/rest/v1/user_profiles";
        response = winhttp_post(insert_url, payload.dump(), headers);
        debug_log("POST response: [" + response + "]");
        
        if (response.empty()) {
            debug_log("✅ User profile successfully posted to Supabase (HTTP 201 with empty body)");
            return true;
        }
        
        bool success = response.find("error") == std::string::npos && response.find("message") == std::string::npos;
        if (success) {
            debug_log("✅ User profile successfully created in Supabase");
        } else {
            debug_log("❌ Supabase create profile failed: " + response);
        }
        return success;
    }
    
    // PATCH succeeded with response data
    debug_log("✅ User profile successfully patched to Supabase");
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

    // Load Supabase config from resource directory
    std::string supabase_config_path = g_resource_dir + "\\supabase_config.json";
    if (!load_supabase_config(supabase_config_path)) {
        // Try as fallback
        if (!load_supabase_config(g_resource_dir + "\\..\\supabase_config.json")) {
            MessageBoxW(NULL, L"Warning: Could not load Supabase configuration!\n\nPlease ensure supabase_config.json exists.\n\nThe app will work offline but won't sync to the cloud.", L"GameStash", MB_ICONWARNING);
        }
    }

    // Init Database
    if (!db_init()) {
        MessageBoxW(NULL, L"Failed to initialize database!", L"GameStash Error", MB_ICONERROR);
        return 1;
    }

    // ─── Quick Supabase connectivity test ─────────────────────────────────────
    if (!g_supabase_url.empty() && !g_supabase_anon_key.empty()) {
        debug_log("\n=== SUPABASE CONNECTIVITY TEST (on startup) ===");
        std::string test_url = g_supabase_url + "/rest/v1/user_profiles?select=*&limit=1";
        std::string test_headers = "apikey: " + g_supabase_anon_key + "\r\nAuthorization: Bearer " + g_supabase_anon_key + "\r\n";
        std::string test_response = winhttp_get(test_url, test_headers);
        debug_log("Startup test response: " + test_response);
        if (test_response.find("No API key") != std::string::npos) {
            debug_log("❌ HEADERS NOT WORKING - API key not received by Supabase");
        } else if (test_response.find("error") != std::string::npos || test_response.find("message") != std::string::npos) {
            debug_log("✅ HEADERS WORKING - Got response (check for permission errors vs auth)");
        } else {
            debug_log("✅✅ HEADERS WORKING - Successfully fetched data from Supabase!");
        }
        debug_log("=== END TEST ===\n");
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

    // ─── Binding: testSupabaseConnection ───────────────────────────────────────
    w.bind("testSupabaseConnection", [&](const std::string&) -> std::string {
        debug_log("🧪 Testing Supabase connection...");
        
        if (g_supabase_url.empty() || g_supabase_anon_key.empty()) {
            debug_log("❌ TEST FAILED: Supabase credentials not loaded");
            return json({{"error", "Supabase config not loaded"}}).dump();
        }
        
        // Test 1: Try to fetch user_profiles (GET request)
        debug_log("TEST 1: GET request to /rest/v1/user_profiles");
        std::string test_url = g_supabase_url + "/rest/v1/user_profiles?select=*&limit=1";
        std::string headers = "apikey: " + g_supabase_anon_key + "\r\nAuthorization: Bearer " + g_supabase_anon_key + "\r\nContent-Type: application/json\r\n";
        debug_log("Headers being sent: apikey and Authorization Bearer token");
        std::string get_response = winhttp_get(test_url, headers);
        
        if (get_response.empty()) {
            debug_log("❌ TEST 1 FAILED: No response from GET");
            return json({{"error", "GET request failed - no response"}}).dump();
        }
        debug_log("✅ TEST 1 PASSED: GET response: " + get_response);
        
        // Test 2: Try to POST a test record to user_profiles
        debug_log("TEST 2: POST test record to /rest/v1/user_profiles");
        json test_profile = {
            {"user_id", "test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count())},
            {"bio", "Test connection"},
            {"country", "Test"},
            {"favorite_platform", "Test"}
        };
        std::string post_response = winhttp_post(g_supabase_url + "/rest/v1/user_profiles", test_profile.dump(), headers);
        
        if (post_response.empty()) {
            debug_log("⚠️ TEST 2 WARNING: No response from POST to user_profiles");
        } else {
            debug_log("✅ TEST 2 RESPONSE: " + post_response);
        }

        // Test 3: Try to POST a test claimed game
        debug_log("TEST 3: POST test record to /rest/v1/claimed_games");
        json test_game = {
            {"user_id", "test@test.com"},
            {"game_id", 9999},
            {"title", "Test Game"},
            {"platforms", "Test"},
            {"worth_inr", "Rs. 0"},
            {"open_giveaway_url", "http://test.com"},
            {"claimed_at", "2026-04-07T10:00:00Z"}
        };
        std::string post_game_response = winhttp_post(g_supabase_url + "/rest/v1/claimed_games", test_game.dump(), headers);
        
        if (post_game_response.empty()) {
            debug_log("⚠️ TEST 3 WARNING: No response from POST to claimed_games");
        } else {
            debug_log("✅ TEST 3 RESPONSE: " + post_game_response);
        }
        
        return json({
            {"success", true},
            {"message", "Connection test completed - check debug log for details"},
            {"get_response", get_response},
            {"post_response", post_response},
            {"post_game_response", post_game_response}
        }).dump();
    });

    // ─── Binding: autoTestClaimGames (for automated testing) ──────────────────
    w.bind("autoTestClaimGames", [&](const std::string&) -> std::string {
        debug_log("\n\n=== AUTO TEST: CLAIMING GAMES ===");
        
        // Test user
        std::string test_email = "autotest@test.com";
        
        // Set g_user_id to test email (simulates login)
        g_user_id = test_email;
        debug_log("1. Set g_user_id = " + g_user_id);
        
        // Claim 5 test games
        for (int i = 1; i <= 5; i++) {
            debug_log("\n2." + std::to_string(i) + " - Claiming test game #" + std::to_string(i));
            
            // Call supabase_add_claimed_game directly
            bool success = supabase_add_claimed_game(
                g_user_id,
                9000 + i,
                "Auto Test Game " + std::to_string(i),
                "PC, Steam",
                "Rs. " + std::to_string(100 * i),
                "http://test.com/game" + std::to_string(i),
                "2026-04-07T10:00:00Z"
            );
            
            if (success) {
                debug_log("   ✅ Game #" + std::to_string(i) + " claim succeeded");
            } else {
                debug_log("   ❌ Game #" + std::to_string(i) + " claim FAILED");
            }
            
            // Small delay between claims
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        
        debug_log("\n=== AUTO TEST COMPLETE - Check Supabase tables ===\n");
        return json({{"success", true}, {"message", "Auto test completed - check debug.log"}}).dump();
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

            // ✅ Create user profile in Supabase using email as user_id
            if (!supabase_upsert_user_profile(email, "", "", "")) {
                debug_log("Warning: Could not create Supabase profile for: " + email);
            }

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
            
            // ✅ Set global user ID to EMAIL (for Supabase operations)
            g_user_id = email;
            
            // ✅ Fetch latest profile from Supabase and load claimed games
            std::thread sync_thread([email]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                // Fetch profile from Supabase
                json supabase_profile = supabase_fetch_user_profile(email);
                // Fetch claimed games from Supabase
                json claimed_games = supabase_fetch_claimed_games(email);
                debug_log("Loaded " + std::to_string(claimed_games.size()) + " games from Supabase for user: " + email);
            });
            sync_thread.detach();
            
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

    // ─── Binding: dbClaimGame (Cloud-First to Supabase) ───────────────────────
    w.bind("dbClaimGame", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            int user_id_int = args[0].get<int>();  // Still receive as int from frontend
            json game = args[1];

            // ✅ Use email (g_user_id) as the user_id for Supabase
            if (g_user_id.empty()) {
                return json({{"error", "User not logged in"}}).dump();
            }

            int game_id = game.value("id", -1);
            std::string title = game.value("title", "");
            std::string thumb = game.value("thumbnail", "");
            std::string plat  = game.value("platforms", "");
            std::string worth = game.value("worth_inr", "");
            std::string gurl  = game.value("open_giveaway_url", "");

            debug_log("dbClaimGame: User=" + g_user_id + ", GameID=" + std::to_string(game_id) + ", Title=" + title);

            // Also insert into local SQLite for offline support
            const char* sql = R"(
                INSERT OR IGNORE INTO claimed_games
                (user_id, game_id, title, thumbnail, platforms, worth_inr, open_giveaway_url)
                VALUES (?, ?, ?, ?, ?, ?, ?)
            )";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                debug_log("Warning: Could not insert to local DB");
            else {
                sqlite3_bind_int(stmt, 1, user_id_int);
                sqlite3_bind_int(stmt, 2, game_id);
                sqlite3_bind_text(stmt, 3, title.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, thumb.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, plat.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 6, worth.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 7, gurl.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }

            // ✅ POST to Supabase (Cloud-First)
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            char timestamp[30];
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&time_t_now));
            if (!supabase_add_claimed_game(g_user_id, game_id, title, plat, worth, gurl, std::string(timestamp))) {
                debug_log("Warning: Supabase add claimed game failed");
                // Still return success for offline support
            }

            return json({{"success", true}}).dump();
        } catch (const std::exception& e) {
            return json({{"error", e.what()}}).dump();
        }
    });

    // ─── Binding: getClaimedGames (Cloud-First from Supabase) ──────────────────
    w.bind("getClaimedGames", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 1)
                return json::array().dump();

            int user_id_int = args[0].get<int>();  // Still receive as int from frontend
            
            // ✅ Use email (g_user_id) to fetch from Supabase
            if (g_user_id.empty()) {
                debug_log("getClaimedGames: User not logged in, using local DB fallback");
                // Fallback to local SQLite if user not in Supabase context
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
            }

            // ✅ Fetch from Supabase (Cloud-First)
            json supabase_games = supabase_fetch_claimed_games(g_user_id);
            
            // Also fallback to local SQLite if Supabase fetch fails
            if (supabase_games.empty() || !supabase_games.is_array()) {
                debug_log("Supabase fetch failed, using local DB fallback");
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
            }

            return supabase_games.dump();
        } catch (...) {
            return json::array().dump();
        }
    });

    // ─── Binding: getUserProfile (Cloud-First from Supabase) ──────────────────
    w.bind("getUserProfile", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 1)
                return json({{"error", "Invalid args - no user_id provided"}}).dump();

            int user_id_int = args[0].get<int>();
            debug_log("getUserProfile called for user_id: " + std::to_string(user_id_int));
            
            if (user_id_int <= 0)
                return json({{"error", "Invalid user_id: " + std::to_string(user_id_int)}}).dump();
            
            // ✅ Try to fetch from Supabase if user_id (email) is available
            if (!g_user_id.empty()) {
                json supabase_profile = supabase_fetch_user_profile(g_user_id);
                if (!supabase_profile.empty() && supabase_profile.is_object()) {
                    debug_log("User profile loaded from Supabase");
                    return json({{"success", true}, {"profile", supabase_profile}}).dump();
                }
            }

            // ✅ Fallback to local SQLite
            debug_log("Supabase profile not available, using local DB fallback");
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
                debug_log("User not found for id: " + std::to_string(user_id_int));
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

    // ─── Binding: updateUserProfile (Cloud-First to Supabase) ──────────────────
    w.bind("updateUserProfile", [&](const std::string& req) -> std::string {
        try {
            json args = json::parse(req);
            if (!args.is_array() || args.size() < 2)
                return json({{"error", "Invalid args"}}).dump();

            int user_id_int = args[0].get<int>();
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

            // ✅ Update in local SQLite first (for offline support)
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
                debug_log("Warning: Local DB update failed, but will try Supabase");

            // ✅ Update to Supabase (Cloud-First)
            if (!g_user_id.empty()) {
                if (supabase_upsert_user_profile(g_user_id, bio, country, fav_platform)) {
                    debug_log("Profile updated successfully in Supabase");
                } else {
                    debug_log("Warning: Supabase update failed");
                }
            }

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
