#include <iostream>
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    std::cout << "Free Game Tracker Initializing...\n" << std::endl;

    // Example of using cpp-httplib
    httplib::Client cli("http://api.zippopotam.us");
    
    std::cout << "Sending a test GET request to api.zippopotam.us...\n";
    auto res = cli.Get("/us/90210");
    if (res && res->status == 200) {
        // Example of using nlohmann_json
        try {
            json response_json = json::parse(res->body);
            std::cout << "Response JSON from server: \n" << response_json.dump(4) << "\n" << std::endl;
            std::cout << "Libraries are successfully linked and working!" << std::endl;
        } catch (json::parse_error& e) {
            std::cerr << "JSON parse error: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "HTTP request failed!" << std::endl;
        if (res) {
            std::cerr << "Status Code: " << res->status << std::endl;
        } else {
            std::cerr << "Error: " << httplib::to_string(res.error()) << std::endl;
        }
    }

    return 0;
}
