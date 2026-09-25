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
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <filesystem>
#include <string>
#include <string_view>

namespace sub0tieredcache::remote {

namespace detail {

/// Environment lookup wrapped so callers don't repeat the null-check; administrative/setup-time only.
/// Windows reads the wide variable (non-ASCII profile paths) via GetEnvironmentVariableW, avoiding
/// MSVC's deprecated getenv.
[[nodiscard]] inline std::filesystem::path env_path_or_empty(const char* name) {
#if defined(_WIN32)
    const std::wstring wide(name, name + std::char_traits<char>::length(name));
    const DWORD needed = ::GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD written = ::GetEnvironmentVariableW(wide.c_str(), value.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    value.resize(written);
    return std::filesystem::path(value);
#else
    if (const char* value = std::getenv(name); value != nullptr) {
        return std::filesystem::path(value);
    }
    return {};
#endif
}

} // namespace detail

/// The platform default user cache directory, namespaced under `<subdir>` so multiple unrelated
/// callers on one machine don't collide (README.md sec 6). Does not create the directory -- callers
/// (e.g. `ChunkStore`) are responsible for `create_directories` at open time, same as any other
/// filesystem path this library is handed.
[[nodiscard]] inline std::filesystem::path default_user_cache_dir(std::string_view subdir = "Sub0TieredCache") {
#if defined(_WIN32)
    // Windows: %LOCALAPPDATA% is the documented per-user, non-roaming cache location.
    const std::filesystem::path local_app_data = detail::env_path_or_empty("LOCALAPPDATA");
    std::filesystem::path base = local_app_data.empty() ? std::filesystem::path(".") : local_app_data;
    return base / subdir;
#elif defined(__APPLE__)
    const std::filesystem::path home = detail::env_path_or_empty("HOME");
    std::filesystem::path base = home.empty() ? std::filesystem::path(".") : home / "Library" / "Caches";
    return base / subdir;
#else
    // Linux and other POSIX: XDG Base Directory spec, falling back to ~/.cache.
    if (const std::filesystem::path xdg = detail::env_path_or_empty("XDG_CACHE_HOME"); !xdg.empty()) {
        return xdg / subdir;
    }
    const std::filesystem::path home = detail::env_path_or_empty("HOME");
    std::filesystem::path base = home.empty() ? std::filesystem::path(".") : home / ".cache";
    return base / subdir;
#endif
}

} // namespace sub0tieredcache::remote
