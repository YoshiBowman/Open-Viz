#include "vizcore/archive.h"

#include <fstream>
#include <iterator>
#include <utility>

#include "miniz.h"
#include "text.h"
#include "vizcore/files.h"

namespace viz {

namespace {

// Largest entry read() will unpack. Models and textures in real files are a few tens of megabytes.
const mz_uint64 kMaxEntryBytes = 512ull * 1024 * 1024;

// Entry names as stored, minus a leading "./" and with forward slashes.
std::string normalised(std::string_view name) {
    std::string result(name);
    for (char& c : result) {
        if (c == '\\') c = '/';
    }
    while (result.rfind("./", 0) == 0) result.erase(0, 2);
    return result;
}

}  // namespace

struct Archive::Impl {
    std::vector<uint8_t> bytes;
    mz_zip_archive zip;
    bool open = false;
    std::vector<std::string> names;       // normalised, files only
    std::vector<mz_uint> indices;         // parallel to names

    ~Impl() {
        if (open) mz_zip_reader_end(&zip);
    }

    int locate(std::string_view name) const {
        std::string wanted = normalised(name);
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == wanted) return static_cast<int>(i);
        }
        for (size_t i = 0; i < names.size(); ++i) {
            if (text::iequals(names[i], wanted)) return static_cast<int>(i);
        }
        return -1;
    }
};

Archive::Archive() = default;
Archive::~Archive() = default;
Archive::Archive(Archive&&) noexcept = default;
Archive& Archive::operator=(Archive&&) noexcept = default;

bool Archive::open_file(const std::string& path, std::string& error) {
    std::ifstream file = read_stream(path);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return open_memory(std::move(bytes), error);
}

bool Archive::open_memory(std::vector<uint8_t> bytes, std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->bytes = std::move(bytes);
    mz_zip_zero_struct(&impl->zip);
    if (!mz_zip_reader_init_mem(&impl->zip, impl->bytes.data(), impl->bytes.size(), 0)) {
        error = std::string("not a readable ZIP archive: ") + mz_zip_get_error_string(mz_zip_get_last_error(&impl->zip));
        return false;
    }
    impl->open = true;
    mz_uint count = mz_zip_reader_get_num_files(&impl->zip);
    for (mz_uint i = 0; i < count; ++i) {
        if (mz_zip_reader_is_file_a_directory(&impl->zip, i)) continue;
        mz_uint size = mz_zip_reader_get_filename(&impl->zip, i, nullptr, 0);
        if (size == 0) continue;
        std::string name(size, '\0');
        mz_zip_reader_get_filename(&impl->zip, i, name.data(), size);
        name.resize(size - 1);    // drop the terminator miniz counts
        impl->names.push_back(normalised(name));
        impl->indices.push_back(i);
    }
    impl_ = std::move(impl);
    return true;
}

bool Archive::is_open() const { return impl_ && impl_->open; }

std::vector<std::string> Archive::entries() const { return impl_ ? impl_->names : std::vector<std::string>(); }

bool Archive::contains(std::string_view name) const { return impl_ && impl_->locate(name) >= 0; }

bool Archive::read(std::string_view name, std::vector<uint8_t>& out) const {
    if (!impl_) return false;
    int found = impl_->locate(name);
    if (found < 0) return false;
    const mz_uint index = impl_->indices[static_cast<size_t>(found)];
    // The size is whatever the archive claims, so refuse one that would only exhaust memory.
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&impl_->zip, index, &stat) || stat.m_uncomp_size > kMaxEntryBytes) return false;
    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(&impl_->zip, index, &size, 0);
    if (!data) return false;
    const uint8_t* begin = static_cast<const uint8_t*>(data);
    out.assign(begin, begin + size);
    mz_free(data);
    return true;
}

bool Archive::copy_with(const std::vector<std::pair<std::string, std::string>>& replacements, std::vector<uint8_t>& out, std::string& error) const {
    if (!impl_ || !impl_->open) {
        error = "the archive is not open";
        return false;
    }
    mz_zip_archive writer;
    mz_zip_zero_struct(&writer);
    if (!mz_zip_writer_init_heap(&writer, 0, impl_->bytes.size())) {
        error = "out of memory";
        return false;
    }
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&impl_->zip);
    for (mz_uint i = 0; i < count && ok; ++i) {
        const mz_uint size = mz_zip_reader_get_filename(&impl_->zip, i, nullptr, 0);
        std::string name(size, '\0');
        if (size > 0) {
            mz_zip_reader_get_filename(&impl_->zip, i, name.data(), size);
            name.resize(size - 1);
        }
        bool replaced = false;
        for (const auto& replacement : replacements) {
            replaced = replaced || text::iequals(normalised(name), normalised(replacement.first));
        }
        if (replaced) continue;
        ok = mz_zip_writer_add_from_zip_reader(&writer, &impl_->zip, i) != MZ_FALSE;
    }
    for (size_t r = 0; r < replacements.size() && ok; ++r) {
        ok = mz_zip_writer_add_mem(&writer, replacements[r].first.c_str(), replacements[r].second.data(), replacements[r].second.size(),
                                   static_cast<mz_uint>(MZ_DEFAULT_COMPRESSION)) != MZ_FALSE;
    }
    void* buffer = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size) != MZ_FALSE;
    if (ok) {
        const uint8_t* begin = static_cast<const uint8_t*>(buffer);
        out.assign(begin, begin + size);
    } else {
        error = std::string("the archive could not be written: ") + mz_zip_get_error_string(mz_zip_get_last_error(&writer));
    }
    if (buffer) mz_free(buffer);
    mz_zip_writer_end(&writer);
    return ok;
}

}  // namespace viz
