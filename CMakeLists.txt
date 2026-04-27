cmake_minimum_required(VERSION 3.14)
project(asio_game_server)

# C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ===================== 核心：支持编译类型切换 =====================
# 默认：Debug 模式（本地开发调试）
# 线上发布时：cmake .. -DCMAKE_BUILD_TYPE=Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# Debug 模式配置（本地调试）
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -Wall")

# Release 模式配置（线上发布，最高优化 + 关闭调试）
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -Wall -DNDEBUG -s")

# ===================== Boost 配置 =====================
# 自动根据模式选择：
# Debug 时动态链接（方便调试）
# Release 时静态链接（打包独立程序，上线无依赖）
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(Boost_USE_STATIC_LIBS ON)  # 线上：静态打包Boost
else()
    set(Boost_USE_STATIC_LIBS OFF) # 本地：动态链接
endif()

find_package(Boost REQUIRED COMPONENTS system)

# ===================== 编译目标 =====================
add_executable(ws_server src/server.cpp src/redis_client.cpp src/room_manager.cpp src/session.cpp)
add_executable(ws_client src/client.cpp)

# 链接库
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    # 线上：静态链接 libgcc/libstdc++，完全无依赖
    target_link_libraries(ws_server PRIVATE Boost::system pthread -static-libgcc -static-libstdc++)
    target_link_libraries(ws_client PRIVATE Boost::system pthread -static-libgcc -static-libstdc++)
else()
    # 本地：普通链接
    target_link_libraries(ws_server PRIVATE Boost::system pthread)
    target_link_libraries(ws_client PRIVATE Boost::system pthread)
endif()

# ===================== Linux PIE 优化 =====================
if(UNIX AND NOT APPLE)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # 线上：开启安全PIE
    else()
        set(CMAKE_POSITION_INDEPENDENT_CODE OFF) # 本地：关闭方便调试
        add_compile_options(-no-pie)
        add_link_options(-no-pie)
    endif()
endif()
