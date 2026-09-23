cmake_minimum_required(VERSION 3.21)

# Detect if Sub0TieredCache is being included inside another project (e.g. vendored into Sub0Llm, the
# recommended first integration seam -- see README.md sec 5).
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0TieredCache VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

option(SUB0TIEREDCACHE_BUILD_TESTING  "Build unit-tests" ON)
option(SUB0TIEREDCACHE_BUILD_EXAMPLES "Build examples"   OFF)

if(SUB0TIEREDCACHE_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
endif()

# Header-only, interface library -- the recommended first implementation shape (README.md sec 5.1:
# vendored header-only client, no new deployment shape, mirrors Sub0Llm's own gguf.hpp precedent).
# Grows a real compiled component only once the disk/remote tiers need background-thread machinery
# substantial enough that header-only stops being the right packaging (README.md sec 5.2).
add_library(Sub0TieredCache INTERFACE)
add_library(Sub0TieredCache::Sub0TieredCache ALIAS Sub0TieredCache)

target_include_directories(Sub0TieredCache INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# C++23, matching the Sub0 project family's own baseline (Sub0Llm, Sub0Log) -- not a Sub0Llm-specific
# constraint here, a deliberate choice for this project too (see STYLE_GUIDE.md).
target_compile_features(Sub0TieredCache INTERFACE cxx_std_23)

if(SUB0TIEREDCACHE_BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
