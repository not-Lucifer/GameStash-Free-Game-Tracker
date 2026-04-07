cmake_minimum_required(VERSION 3.15)
project(GameStash CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dependencies via vcpkg
find_package(nlohmann_json CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(unofficial-webview2 CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# zserge-webview header
set(ZSERGE_WEBVIEW_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/installed/x64-windows/include")

# Create GUI application (no console window)
add_executable(GameStash WIN32 main.cpp)

# ─── STATIC LINK C++ RUNTIME (compatibility with older Windows 10) ───
# This embeds the C++ runtime inside the executable instead of requiring DLLs
if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set_target_properties(GameStash PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS")
endif()

# Hide console window (alternative approach - not needed with WIN32 flag, but kept for compatibility)

target_include_directories(GameStash PRIVATE ${ZSERGE_WEBVIEW_INCLUDE_DIRS})

target_link_libraries(GameStash PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    unofficial::webview2::webview2
    unofficial::sqlite3::sqlite3
)
