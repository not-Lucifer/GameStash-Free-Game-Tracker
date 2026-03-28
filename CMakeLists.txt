cmake_minimum_required(VERSION 3.15)
project(GameStash CXX)

# Require C++17 for both cpp-httplib and general modern C++ practices
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann_json
find_package(nlohmann_json CONFIG REQUIRED)

# Find cpp-httplib
find_package(httplib CONFIG REQUIRED)

# Find webview2
find_package(unofficial-webview2 CONFIG REQUIRED)

# Find zserge-webview (header only)
set(ZSERGE_WEBVIEW_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/x64-windows/include")

# Create the executable
add_executable(GameStash main.cpp)

# Hide console window in MSVC by setting subsystem to Windows but entry to mainCRTStartup
if (MSVC)
    set_target_properties(GameStash PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
endif()

# Include webview
target_include_directories(GameStash PRIVATE ${ZSERGE_WEBVIEW_INCLUDE_DIRS})

# Link the libraries
target_link_libraries(GameStash PRIVATE 
    nlohmann_json::nlohmann_json 
    httplib::httplib
    unofficial::webview2::webview2
)
