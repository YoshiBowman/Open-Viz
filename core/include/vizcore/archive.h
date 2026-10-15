// Read-only access to the ZIP containers GDTF (.gdtf) and MVR (.mvr) files are stored in.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vizcore/types.h"

namespace viz {

class VIZCORE_API Archive {
public:
    Archive();
    ~Archive();
    Archive(Archive&&) noexcept;
    Archive& operator=(Archive&&) noexcept;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;

    // Both return false and fill `error` if the data is not a readable ZIP archive.
    bool open_file(const std::string& path, std::string& error);
    bool open_memory(std::vector<uint8_t> bytes, std::string& error);

    bool is_open() const;
    std::vector<std::string> entries() const;

    // Exact name first, then a case-insensitive match: files in the wild are not consistent
    // about the case of names such as description.xml.
    bool contains(std::string_view name) const;
    bool read(std::string_view name, std::vector<uint8_t>& out) const;

    // The archive again, as bytes, with the named entries replaced by new content (or added, if it
    // has none of that name). Everything else is carried over exactly as it is stored, without
    // being unpacked. False, with a reason in `error`, if that cannot be done.
    bool copy_with(const std::vector<std::pair<std::string, std::string>>& replacements, std::vector<uint8_t>& out, std::string& error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace viz
