cmake_minimum_required(VERSION 3.16)

project(omni_arm_link
    VERSION 0.1.0
    DESCRIPTION "IEEE 802.11 raw-link (WFB-NG) communication stack for competition robots"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

option(OAL_BUILD_TOOLS "Build test/simulation tools" ON)
option(OAL_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(OAL_ENABLE_ASAN "Enable AddressSanitizer/UBSan" OFF)

set(OAL_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)

add_library(oal_core STATIC
    src/codec.cpp
    src/crc.cpp
    src/clock.cpp
    src/log.cpp
    src/config.cpp
    src/stats.cpp
    src/signal.cpp
    src/udp_socket.cpp
    src/wfb_link.cpp
)

target_include_directories(oal_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_options(oal_core PRIVATE ${OAL_WARNING_FLAGS})

find_package(Threads REQUIRED)
target_link_libraries(oal_core PUBLIC Threads::Threads)

if(OAL_WARNINGS_AS_ERRORS)
    target_compile_options(oal_core PRIVATE -Werror)
endif()

if(OAL_ENABLE_ASAN)
    target_compile_options(oal_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(oal_core PUBLIC -fsanitize=address,undefined)
endif()

function(oal_add_app target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE oal_core)
    target_compile_options(${target} PRIVATE ${OAL_WARNING_FLAGS})
    if(OAL_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()
endfunction()

oal_add_app(oal_robot      src/robot/main.cpp)
oal_add_app(oal_controller src/controller/main.cpp)
oal_add_app(oal_monitor    src/monitor/main.cpp)

if(OAL_BUILD_TOOLS)
    oal_add_app(oal_linktest src/tools/linktest.cpp)
    oal_add_app(oal_sim      src/tools/sim.cpp)
endif()

install(TARGETS oal_robot oal_controller oal_monitor
    RUNTIME DESTINATION bin)
if(OAL_BUILD_TOOLS)
    install(TARGETS oal_linktest oal_sim RUNTIME DESTINATION bin)
endif()

install(DIRECTORY config/ DESTINATION share/omni-arm-link/config
    FILES_MATCHING PATTERN "*.cfg" PATTERN "*.conf")
install(DIRECTORY scripts/ DESTINATION share/omni-arm-link/scripts
    FILES_MATCHING PATTERN "*.sh" PATTERN "*.py")
install(DIRECTORY systemd/ DESTINATION share/omni-arm-link/systemd
    FILES_MATCHING PATTERN "*.service")

enable_testing()
add_test(NAME protocol_roundtrip COMMAND oal_linktest --self-test)
