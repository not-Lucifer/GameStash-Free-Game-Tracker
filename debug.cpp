#include <iostream>
#include <fstream>
#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    httplib::Client cli("http://www.gamerpower.com");
    cli.set_follow_location(true); 
    httplib::Headers headers = { {"User-Agent", "cpp-httplib/1.0"} };
    auto res = cli.Get("/api/giveaways", headers);
    if (res && res->status == 200) {
        json response_json = json::parse(res->body);
        std::ofstream out("debug.json");
        out << response_json[0].dump(4);
    }
    return 0;
}
