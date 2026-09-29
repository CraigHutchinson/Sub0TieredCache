# S1b packaging validation — 2026-09-29

The public target now always links MemPage transitively. A parent-supplied target is reused; otherwise the same pinned FetchContent dependency is resolved. Two parent-project compile regressions link only TieredCache, covering both paths offline.

Windows Clang 22.1.8 / Ninja / Release: **11/11 CTest tests passed**.
WSL Linux GCC 15 / Ninja / Release: **11/11 CTest tests passed**.
The complete standalone suite ran; consumer tests configure/build real executables rather than
checking target-property strings. No performance measurement or GPU qualification is claimed.

Reproduce with CMake configure, build, then `ctest --test-dir <build> --output-on-failure`.
TieredCache additionally uses `FETCHCONTENT_SOURCE_DIR_SUB0MEMPAGE` to select the audited local
MemPage checkout and `FETCHCONTENT_FULLY_DISCONNECTED=ON`. No dependency pin changed in this package.
These results test the local packaging fixes; prior published CI is not evidence for these new commits.
