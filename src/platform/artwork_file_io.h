// SPDX-License-Identifier: GPL-3.0-or-later
// File transactions for portable-profile artwork. The platform guarantees
// bounded reads, reparse rejection, durable non-overwriting publication and
// a stable file identity stamp for the read-only digest proof cache.
#pragma once
#include <filesystem>
#include <string>
#include <cstdint>

namespace rta::platform::artwork_io {
struct FileStamp {
    uint32_t flags=0;
    uint64_t bytes=0, created=0, written=0;
};
bool safe(const std::filesystem::path& path);
// Access denial counts as present: absence must be positively established.
bool present(const std::filesystem::path& path);
bool read(const std::filesystem::path& path, size_t maximum, std::string& bytes);
// Publish a new file atomically. Never overwrite an existing destination.
bool publish(const std::filesystem::path& path, const std::string& bytes);
bool capture_stamp(const std::filesystem::path& path, FileStamp& stamp);
inline bool same_stamp(const FileStamp&a,const FileStamp&b) {
    return a.flags==b.flags&&a.bytes==b.bytes&&a.created==b.created&&a.written==b.written;
}
}

#if defined(_WIN32)
#include "platform/windows/artwork_file_io.h"
#else
#error "platform/artwork_file_io.h: this platform must provide bounded non-overwriting artwork transactions"
#endif
