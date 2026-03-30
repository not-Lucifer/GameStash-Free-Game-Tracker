#include <httplib.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

using json = nlohmann::json;

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// Debug logging to file
std::ofstream g_debug_log("debug.log", std::ios::app);
void debug_log(const std::string& msg) {
    g_debug_log << "[" << std::chrono::system_clock::now().time_since_epoch().count() << "] " << msg << std::endl;
    g_debug_log.flush();
    std::cerr << msg << std::endl;
}

std::string get_worth_in_inr(const std::string& worth_usd) {
  if (worth_usd == "N/A") return "N/A";
  if (!worth_usd.empty() && worth_usd[0] == '$') {
    try {
      double usd_val = std::stod(worth_usd.substr(1));
      double inr_val = usd_val * 83.5; // Approximate 1 USD = 83.5 INR
      std::ostringstream stream;
      stream << std::fixed << std::setprecision(2) << "Rs. " << inr_val;
      return stream.str();
    } catch (...) {
      return worth_usd;
    }
  }
  return worth_usd;
}

std::string get_direct_url(const std::string &open_url) {
  if (open_url.find("http") != 0)
    return open_url;

  std::cerr << "Resolving URL: " << open_url << std::endl;
  std::wstring wurl(open_url.begin(), open_url.end());
  
  HINTERNET hSession = WinHttpOpen(
      L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0);
  if (!hSession) {
    std::cerr << "Failed to create WinHttp session" << std::endl;
    return open_url;
  }

  // Set timeouts
  WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

  std::string direct_url = open_url;
  URL_COMPONENTS urlComp;
  ZeroMemory(&urlComp, sizeof(urlComp));
  urlComp.dwStructSize = sizeof(urlComp);

  wchar_t hostName[256];
  wchar_t urlPath[2048];
  urlComp.lpszHostName = hostName;
  urlComp.dwHostNameLength = 256;
  urlComp.lpszUrlPath = urlPath;
  urlComp.dwUrlPathLength = 2048;

  if (WinHttpCrackUrl(wurl.c_str(), 0, 0, &urlComp)) {
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (hConnect) {
      DWORD dwOpenRequestFlag =
          (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
      HINTERNET hRequest = WinHttpOpenRequest(
          hConnect, L"GET", urlPath, NULL, WINHTTP_NO_REFERER,
          WINHTTP_DEFAULT_ACCEPT_TYPES, dwOpenRequestFlag);

      if (hRequest) {
        wchar_t headers[] = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36\r\n";
        
        if (WinHttpSendRequest(hRequest, headers, -1,
                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
          if (WinHttpReceiveResponse(hRequest, NULL)) {
            wchar_t finalUrl[4096];
            DWORD dwSize = sizeof(finalUrl);
            if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, finalUrl,
                                   &dwSize)) {
              std::wstring wfinal(finalUrl);
              direct_url = std::string(wfinal.begin(), wfinal.end());
              std::cerr << "Resolved URL: " << direct_url << std::endl;
            }
          } else {
            std::cerr << "WinHttpReceiveResponse failed" << std::endl;
          }
        } else {
          std::cerr << "WinHttpSendRequest failed" << std::endl;
        }
        WinHttpCloseHandle(hRequest);
      }
      WinHttpCloseHandle(hConnect);
    }
  }
  WinHttpCloseHandle(hSession);

  return direct_url;
}

#include <webview/webview.h>

std::string read_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return "";
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int main() {
  webview::webview w(true, nullptr);
  w.set_title("Game Stash");
  w.set_size(1000, 700, WEBVIEW_HINT_NONE);

  w.bind("fetchGiveaways", [&](const std::string &req) -> std::string {
    debug_log("fetchGiveaways called with: " + req);
    
    try {
      httplib::Client cli("http://www.gamerpower.com");
      cli.set_follow_location(true);
      cli.set_connection_timeout(5, 0);
      cli.set_read_timeout(5, 0);

      httplib::Headers headers = {
          {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}};

      debug_log("Making API request to http://www.gamerpower.com/api/giveaways");
      auto res = cli.Get("/api/giveaways", headers);

      if (!res) {
        debug_log("API Error: No response from server");
        debug_log("  Error details: " + to_string(res.error()));
        return json({{"error", "No response from API"}}).dump();
      }

      debug_log("API Status: " + std::to_string(res->status));
      if (res->status != 200) {
        debug_log("API Error: HTTP Status " + std::to_string(res->status));
        return json({{"error", "HTTP " + std::to_string(res->status)}}).dump();
      }

      json response_json = json::parse(res->body);
      json filtered = json::array();
      
      for (const auto &item : response_json) {
        std::string platforms = item.value("platforms", "N/A");
        bool is_trusted = (platforms.find("Steam") != std::string::npos ||
                           platforms.find("Epic Games") != std::string::npos ||
                           platforms.find("GOG") != std::string::npos ||
                           platforms.find("Itch.io") != std::string::npos ||
                           platforms.find("itchio") != std::string::npos ||
                           platforms.find("Itchio") != std::string::npos);

        if (!is_trusted) continue;

        json new_item = item;
        std::string worth = item.value("worth", "N/A");
        new_item["worth_inr"] = get_worth_in_inr(worth);
        filtered.push_back(new_item);
      }
      
      debug_log("Returning " + std::to_string(filtered.size()) + " trusted giveaways");
      return filtered.dump();
    } catch (const std::exception &e) {
      debug_log("Exception: " + std::string(e.what()));
      return json({{"error", e.what()}}).dump();
    }
  });

  w.bind("resolveDirectUrl", [&](const std::string &req) -> std::string {
    try {
      json args = json::parse(req);
      if (args.is_array() && args.size() > 0) {
        std::string url = args[0].get<std::string>();
        std::string direct_url = get_direct_url(url);
        return json(direct_url).dump();
      }
    } catch (...) {}
    return "\"\"";
  });

  w.bind("sendNotification", [&](const std::string &req) -> std::string {
    try {
      json args = json::parse(req);
      if (args.is_array() && args.size() >= 2) {
        std::string title = args[0].get<std::string>();
        std::string message = args[1].get<std::string>();
        
        // Use Windows Toast Notifications via PowerShell
        std::string cmd = "powershell -Command \"[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null; "
                         "[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] > $null; "
                         "$APP_ID = 'GameStash'; "
                         "$template = @\"<toast><visual><binding template='ToastText02'><text id='1'>" + title + "</text><text id='2'>" + message + "</text></binding></visual></toast>@\"; "
                         "$xml = New-Object Windows.Data.Xml.Dom.XmlDocument; "
                         "$xml.LoadXml($template); "
                         "$toast = New-Object Windows.UI.Notifications.ToastNotification $xml; "
                         "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier($APP_ID).Show($toast)\"";
        
        system(cmd.c_str());
        return json({{"success", true}}).dump();
      }
    } catch (const std::exception &e) {
      std::cerr << "Notification error: " << e.what() << std::endl;
    }
    return json({{"success", false}}).dump();
  });

  w.bind("openInBrowser", [&](const std::string &req) -> std::string {
    try {
      json args = json::parse(req);
      if (args.is_array() && args.size() > 0) {
        std::string url = args[0].get<std::string>();
        std::cerr << "Opening in system browser: " << url << std::endl;
        
        // Use ShellExecute to open URL in default browser
        std::wstring wurl(url.begin(), url.end());
        ShellExecuteW(NULL, L"open", wurl.c_str(), NULL, NULL, SW_SHOW);
        
        return json({{"success", true}}).dump();
      }
    } catch (const std::exception &e) {
      std::cerr << "Browser open error: " << e.what() << std::endl;
    }
    return json({{"success", false}}).dump();
  });

  // Since `file://` might block fetch requests in some webview environments, 
  // Get absolute directory to accurately find the HTML files 
  char buffer[MAX_PATH];
  GetCurrentDirectoryA(MAX_PATH, buffer);
  std::string cwd(buffer);
  
  // If run from inside the 'build' or 'build/Debug' directory (like via run.bat)
  // we pop up to the root project folder
  size_t build_pos = cwd.find("\\build");
  if (build_pos != std::string::npos) {
      cwd = cwd.substr(0, build_pos);
  }
  
  // Format as a proper file:// URI
  std::string html_path = "file:///" + cwd + "/index.html";
  for (char& c : html_path) {
      if (c == '\\') c = '/';
  }
  
  // Load the beautiful new GUI
  w.navigate(html_path);
  w.run();

  return 0;
}
