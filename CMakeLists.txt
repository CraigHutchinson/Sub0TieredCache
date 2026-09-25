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

# Dependency: Sub0TieredCache -> Sub0MemPage only (lead decision 1; find_package-free, matches
# STYLE_GUIDE.md's "no third-party dependencies" spirit -- Sub0MemPage is a sibling Sub0 project, not a
# third party, but the same "no surprise system search" preference applies). Pinned to the exact commit
# docs/integration-plan.md's acceptance-order gate reports (README.md records the pinned revision).
#
# Offline/sibling-checkout development: set FETCHCONTENT_SOURCE_DIR_SUB0MEMPAGE to a local Sub0MemPage
# checkout to skip the network fetch entirely (CMake's own FetchContent convention: the variable name is
# the fetched dependency's name, upper-cased, exactly as SUB0MEMPAGE here). Combine with
# -DFETCHCONTENT_FULLY_DISCONNECTED=ON once that variable is set to guarantee no network access is even
# attempted (the Sub0Llm stack plan requires ordinary builds/tests to stay offline). Example:
#   cmake --preset default \
#     -DFETCHCONTENT_SOURCE_DIR_SUB0MEMPAGE=/path/to/Sub0MemPage \
#     -DFETCHCONTENT_FULLY_DISCONNECTED=ON
set(SUB0TIEREDCACHE_SUB0MEMPAGE_REVISION "2340409b43db2cb03efe6d586a50f4ced5114b73"
    CACHE STRING "Pinned Sub0MemPage commit (docs/integration-plan.md acceptance order).")

if(NOT IS_SUBPROJECT)
    include(FetchContent)
    # Sub0MemPage's own tests/bench/tools must not build as part of Sub0TieredCache's build -- set its
    # options OFF before MakeAvailable (its CMakeLists.txt option names: SUB0MEMPAGE_BUILD_TESTING,
    # SUB0MEMPAGE_BUILD_EXAMPLES; see docs/integration-plan.md's "Contract feedback" section for why
    # there is no single "don't build tools" switch to flip instead).
    set(SUB0MEMPAGE_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(SUB0MEMPAGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        Sub0MemPage
        GIT_REPOSITORY https://github.com/CraigHutchinson/Sub0MemPage.git
        GIT_TAG ${SUB0TIEREDCACHE_SUB0MEMPAGE_REVISION}
    )
    FetchContent_MakeAvailable(Sub0MemPage)
    target_link_libraries(Sub0TieredCache INTERFACE Sub0MemPage::Sub0MemPage)
endif()

if(SUB0TIEREDCACHE_BUILD_TESTING AND NOT IS_SUBPROJECT AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
