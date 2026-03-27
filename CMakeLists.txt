cmake_minimum_required(VERSION 3.15)
project(FreeGameTracker CXX)

# Require C++17 for both cpp-httplib and general modern C++ practices
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find nlohmann_json
find_package(nlohmann_json CONFIG REQUIRED)

# Find cpp-httplib
find_package(httplib CONFIG REQUIRED)

# Create the executable
add_executable(FreeGameTracker main.cpp)

# Link the libraries
# Note: httplib::httplib might require threading, which vcpkg handles, or we just link it.
target_link_libraries(FreeGameTracker PRIVATE nlohmann_json::nlohmann_json httplib::httplib)
