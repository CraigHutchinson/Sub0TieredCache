# Trace-replay benchmark (docs/trace-replay.md). Off by default: a measurement tool, not a test.
add_executable(sub0tieredcache-trace-replay trace_replay.cpp)
target_link_libraries(sub0tieredcache-trace-replay PRIVATE Sub0TieredCache::Sub0TieredCache)
target_compile_features(sub0tieredcache-trace-replay PRIVATE cxx_std_23)
