# Minimal offline CTest infrastructure for this package's own tests (docs/integration-plan.md:
# "Default CTest needs C++23 ... only, never an engine checkout or network"). T0's own test harness
# is being added concurrently in a sibling worktree; this file only wires up what T3's own tests need
# and is deliberately small so the two can be reconciled at merge time.

find_package(Threads REQUIRED)

function(sub0tieredcache_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Sub0TieredCache::Sub0TieredCache Threads::Threads)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(WIN32)
        target_link_libraries(${name} PRIVATE ws2_32)
    endif()
    add_test(NAME ${name} COMMAND ${name})
    # A coalescing/backoff defect can surface as a hang; fail instead of stalling CI.
    set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

sub0tieredcache_add_test(sub0tieredcache-remote-transport-tests remote_transport_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-remote-chunk-store-tests remote_chunk_store_tests.cpp)
sub0tieredcache_add_test(sub0tieredcache-remote-mirror-tests remote_mirror_tests.cpp)
