// ============================================================
//  FreeGameTracker - CLI Version (Working)
//  Reconstructed from build history — commit: cli-version branch
//
//  Features:
//    - Fetches live giveaways from GamerPower API
//    - Filters to trusted platforms (Steam, Epic, GOG, Itch.io)
//    - Converts USD price to INR
//    - Resolves redirect URLs via WinHTTP (shows real store link)
//    - Displays formatted table in console
// ============================================================

#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// ── Helpers ──────────────────────────────────────────────────

std::string get_worth_in_inr(const std::string& worth_usd) {
    if (worth_usd == "N/A") return "N/A";
    if (!worth_usd.empty() && worth_usd[0] == '$') {
        try {
            double usd_val = std::stod(worth_usd.substr(1));
            double inr_val = usd_val * 83.5; // ~1 USD = 83.5 INR
            std::ostringstream stream;
            stream << std::fixed << std::setprecision(2) << "Rs. " << inr_val;
            return stream.str();
        } catch (...) {
            return worth_usd;
        }
    }
    return worth_usd;
}

std::string get_direct_url(const std::string& open_url) {
    if (open_url.find("http") != 0)
        return open_url;

    std::wstring wurl(open_url.begin(), open_url.end());
    HINTERNET hSession =
        WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return open_url;

    std::string direct_url = open_url;
    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);

    wchar_t hostName[256];
    wchar_t urlPath[2048];
    urlComp.lpszHostName     = hostName;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath      = urlPath;
    urlComp.dwUrlPathLength  = 2048;

    if (WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
        HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
        if (hConnect) {
            DWORD dwOpenRequestFlag =
                (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
            HINTERNET hRequest = WinHttpOpenRequest(
                hConnect, L"HEAD", urlPath, NULL, WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES, dwOpenRequestFlag);

            if (hRequest) {
                if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                    if (WinHttpReceiveResponse(hRequest, NULL)) {
                        wchar_t finalUrl[4096];
                        DWORD dwSize = sizeof(finalUrl);
                        if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, finalUrl, &dwSize)) {
                            std::wstring wfinal(finalUrl);
                            direct_url = std::string(wfinal.begin(), wfinal.end());
                        }
                    }
                }
                WinHttpCloseHandle(hRequest);
            }
            WinHttpCloseHandle(hConnect);
        }
    }
    WinHttpCloseHandle(hSession);
    return direct_url;
}

// ── Pretty-print helpers ──────────────────────────────────────

void print_separator(int width = 80) {
    std::cout << std::string(width, '-') << "\n";
}

std::string truncate(const std::string& s, size_t max_len) {
    if (s.length() <= max_len) return s;
    return s.substr(0, max_len - 3) + "...";
}

// ── Main ─────────────────────────────────────────────────────

int main() {
    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════════╗\n";
    std::cout << "  ║       FREE GAME TRACKER  (CLI v1.0)      ║\n";
    std::cout << "  ║    Powered by GamerPower API             ║\n";
    std::cout << "  ╚══════════════════════════════════════════╝\n\n";
    std::cout << "  Fetching live giveaways...\n\n";

    httplib::Client cli("http://www.gamerpower.com");
    cli.set_follow_location(true);

    httplib::Headers headers = {
        {"User-Agent", "cpp-httplib/1.0 CLI/1.0"}
    };

    auto res = cli.Get("/api/giveaways", headers);

    if (!res || res->status != 200) {
        std::cerr << "  [ERROR] Failed to fetch giveaways. HTTP status: "
                  << (res ? std::to_string(res->status) : "no response") << "\n";
        return 1;
    }

    json data;
    try {
        data = json::parse(res->body);
    } catch (const json::parse_error& e) {
        std::cerr << "  [ERROR] JSON parse error: " << e.what() << "\n";
        return 1;
    }

    // Filter to trusted platforms only
    std::vector<json> trusted;
    for (const auto& item : data) {
        std::string platforms = item.value("platforms", "N/A");
        bool is_trusted = (platforms.find("Steam")      != std::string::npos ||
                           platforms.find("Epic Games") != std::string::npos ||
                           platforms.find("GOG")        != std::string::npos ||
                           platforms.find("Itch.io")    != std::string::npos ||
                           platforms.find("itchio")     != std::string::npos ||
                           platforms.find("Itchio")     != std::string::npos);
        if (is_trusted)
            trusted.push_back(item);
    }

    if (trusted.empty()) {
        std::cout << "  No active giveaways found on trusted platforms right now.\n";
        return 0;
    }

    std::cout << "  Found " << trusted.size() << " giveaway(s) on trusted platforms:\n\n";

    int idx = 1;
    for (const auto& item : trusted) {
        std::string title     = item.value("title",     "Unknown");
        std::string platforms = item.value("platforms", "N/A");
        std::string worth     = item.value("worth",     "N/A");
        std::string end_date  = item.value("end_date",  "N/A");
        std::string open_url  = item.value("open_giveaway_url", "");
        std::string type      = item.value("type",      "N/A");

        std::string worth_inr = get_worth_in_inr(worth);

        print_separator();
        std::cout << "  #" << idx++ << "  " << title << "\n";
        print_separator();
        std::cout << "  Type      : " << type << "\n";
        std::cout << "  Platforms : " << truncate(platforms, 70) << "\n";
        std::cout << "  Worth     : " << worth << " (" << worth_inr << ")\n";
        std::cout << "  Ends      : " << end_date << "\n";

        if (!open_url.empty()) {
            std::cout << "  Resolving store link";
            std::cout.flush();
            std::string direct = get_direct_url(open_url);
            std::cout << "\r  Store URL : " << truncate(direct, 70) << "\n";
        }

        std::cout << "\n";
    }

    print_separator();
    std::cout << "  Done! " << (idx - 1) << " giveaway(s) listed.\n\n";

    std::cout << "  Press Enter to exit...";
    std::cin.get();
    return 0;
}
