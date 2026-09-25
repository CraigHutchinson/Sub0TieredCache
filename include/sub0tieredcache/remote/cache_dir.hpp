#pragma once

/** @file cache_dir.hpp
 *  @brief The platform-appropriate default *user*-level cache directory (README.md sec 6,
 *         docs/tiered-storage-design.md sec 2f): "$XDG_CACHE_HOME or ~/.cache on Linux,
 *         %LOCALAPPDATA% on Windows, ~/Library/Caches on macOS ... Sub0TieredCache's own default when
 *         a caller does not specify a local_disk_cache_dir explicitly." Deliberately never a Sub0Llm
 *         path (`out/build/.../generated/`) or a system-level path (`/var/cache/...`) -- sec 2f is
 *         explicit that those conventions belong to the caller, not to this library's own default.
 */

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>

namespace sub0tieredcache::remote {

namespace detail {

/// std::getenv wrapped so callers don't repeat the null-check; administrative/setup-time only.
[[nodiscard]] inline std::string env_or_empty(const char* name) {
    if (const char* value = std::getenv(name); value != nullptr) {
        return std::string(value);
    }
    return {};
}

} // namespace detail

/// The platform default user cache directory, namespaced under `<subdir>` so multiple unrelated
/// callers on one machine don't collide (README.md sec 6). Does not create the directory -- callers
/// (e.g. `ChunkStore`) are responsible for `create_directories` at open time, same as any other
/// filesystem path this library is handed.
[[nodiscard]] inline std::filesystem::path default_user_cache_dir(std::string_view subdir = "Sub0TieredCache") {
#if defined(_WIN32)
    // Windows: %LOCALAPPDATA% is the documented per-user, non-roaming cache location.
    const std::string local_app_data = detail::env_or_empty("LOCALAPPDATA");
    std::filesystem::path base = local_app_data.empty() ? std::filesystem::path(".") : std::filesystem::path(local_app_data);
    return base / subdir;
#elif defined(__APPLE__)
    const std::string home = detail::env_or_empty("HOME");
    std::filesystem::path base = home.empty() ? std::filesystem::path(".") : std::filesystem::path(home) / "Library" / "Caches";
    return base / subdir;
#else
    // Linux and other POSIX: XDG Base Directory spec, falling back to ~/.cache.
    if (const std::string xdg = detail::env_or_empty("XDG_CACHE_HOME"); !xdg.empty()) {
        return std::filesystem::path(xdg) / subdir;
    }
    const std::string home = detail::env_or_empty("HOME");
    std::filesystem::path base = home.empty() ? std::filesystem::path(".") : std::filesystem::path(home) / ".cache";
    return base / subdir;
#endif
}

} // namespace sub0tieredcache::remote
