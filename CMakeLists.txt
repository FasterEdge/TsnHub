cmake_minimum_required(VERSION 4.1)

project(TsnHub)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 查找依赖库
find_package(CLI11 REQUIRED)
find_package(open62541 REQUIRED)

add_executable(TsnHub
        main.cpp
        unix_socket/UnixSocketBridge.cpp
        name_pipe/NamedPipeBridge.cpp
        command_line/CommandLineBridge.cpp
        tsn/Open62541Adapter.cpp
)

target_include_directories(TsnHub
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 链接库
target_link_libraries(TsnHub
        PRIVATE
        CLI11::CLI11
        open62541::open62541
)
