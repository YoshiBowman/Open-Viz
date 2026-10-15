// Files by a path written in UTF-8, on every system. No Unreal headers, no exceptions.
//
// Every path the core is given is UTF-8: Unreal hands it over as such, and so do the command-line
// tools (on Windows their manifest makes the program's code page UTF-8). Windows' narrow file
// functions read a path in the computer's own code page instead, so there a folder called "Bühne"
// would not open; these go through the wide functions on Windows and are the plain ones elsewhere.
#pragma once

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <share.h>
#endif

namespace viz {

// A path from UTF-8 text.
inline std::filesystem::path utf8_path(std::string_view utf8) {
#if defined(__cpp_lib_char8_t)
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return std::filesystem::u8path(utf8.begin(), utf8.end());
#endif
}

// std::fopen by a UTF-8 path. Others may read the file while it is open, as on a Mac.
inline std::FILE* open_file(const std::string& utf8, const char* mode) {
#if defined(_WIN32)
    const std::wstring wide_mode(mode, mode + std::strlen(mode));
    return _wfsopen(utf8_path(utf8).c_str(), wide_mode.c_str(), _SH_DENYNO);
#else
    return std::fopen(utf8.c_str(), mode);
#endif
}

inline std::ifstream read_stream(const std::string& utf8) { return std::ifstream(utf8_path(utf8), std::ios::binary); }

inline std::ofstream write_stream(const std::string& utf8) { return std::ofstream(utf8_path(utf8), std::ios::binary | std::ios::trunc); }

// Whether a file was there and is gone.
inline bool remove_file(const std::string& utf8) {
    std::error_code ignored;
    return std::filesystem::remove(utf8_path(utf8), ignored);
}

// Moves a file into place, over whatever had that name.
inline bool rename_file(const std::string& from, const std::string& to) {
    std::error_code failed;
    std::filesystem::rename(utf8_path(from), utf8_path(to), failed);
    return !failed;
}

}  // namespace viz
