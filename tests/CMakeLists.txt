find_package(Threads REQUIRED)

# One portable offline test executable per file; warnings are errors on every toolchain, mirroring
# Sub0MemPage's own tests/CMakeLists.txt exactly (STYLE_GUIDE.md).
function(sub0tieredcache_add_test name source)
    add_executable(${name} ${source} allocation_counter.cpp)
    target_link_libraries(${name} PRIVATE Sub0TieredCache::Sub0TieredCache Threads::Threads)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    string(REPLACE "sub0tieredcache-" "sub0tieredcache." test_name ${name})
    string(REGEX REPLACE "-tests$" "" test_name ${test_name})
    add_test(NAME ${test_name} COMMAND ${name})
    # State-machine defects can surface as a hang (a waiter never woken); fail instead of stalling CI.
    set_tests_properties(${test_name} PROPERTIES TIMEOUT 60)
endfunction()

sub0tieredcache_add_test(sub0tieredcache-row-cache-tests row_cache_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-concurrency-tests concurrency_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-allocation-tests allocation_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-lifecycle-tests lifecycle_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-remote-transport-tests remote_transport_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-remote-chunk-store-tests remote_chunk_store_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-remote-mirror-tests remote_mirror_tests.cpp)
