#include <sub0tieredcache/row_cache.hpp>
#include <sub0mempage/local_file_backend.hpp>
int main() {
    auto backend = sub0mempage::LocalFileBackend::create({.workers = 1, .queue_capacity = 1, .max_sources = 1});
    return backend ? 0 : 1;
}
