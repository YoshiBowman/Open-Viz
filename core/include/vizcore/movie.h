// Movies of the visualizer's picture: a QuickTime file of JPEG pictures ("Photo - JPEG"), one
// after another at a steady rate. JPEG because every engine can make it and every player can show
// it; the file is large, and macOS's avconvert, or any editing program, makes H.264 of it.
//
// The pictures go into the file as they come; the index that says where each one is goes at the
// end, when the movie is closed. The file is written beside its name, as "<name>.part", and takes
// its name only once it is whole, so a movie whose writer stopped half-way never looks finished.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "vizcore/types.h"

namespace viz {

class VIZCORE_API MovieWriter {
public:
    MovieWriter() = default;
    ~MovieWriter();    // a movie not closed is left as its ".part"
    MovieWriter(const MovieWriter&) = delete;
    MovieWriter& operator=(const MovieWriter&) = delete;

    // Width and height in pixels (1 to 16384), and how many pictures make a second (1 to 240).
    bool open(const std::string& path, int width, int height, int frames_per_second, std::string& error);
    // One picture, a whole JPEG file, shown for one frame. False once writing has failed.
    bool add(const uint8_t* jpeg, size_t size);
    // Writes the index and gives the file its name. False, with the reason, if that cannot be done.
    bool close(std::string& error);

    bool is_open() const { return file_ != nullptr; }
    size_t frames() const { return sizes_.size(); }
    double seconds() const { return rate_ > 0 ? static_cast<double>(sizes_.size()) / rate_ : 0.0; }

private:
    std::FILE* file_ = nullptr;
    std::string path_;
    int width_ = 0, height_ = 0, rate_ = 0;
    bool failed_ = false;
    uint64_t data_start_ = 0;         // where the pictures begin, past the header of the box that holds them
    uint64_t written_ = 0;            // bytes of pictures so far
    std::vector<uint32_t> sizes_;     // each picture's length
    std::vector<uint64_t> offsets_;   // and where in the file it starts
};

// Reads a QuickTime file's sample table back, for tests and checks: the frame rate and size, and
// where each picture is. False if it is not a movie as MovieWriter writes them.
struct MovieFacts {
    int width = 0, height = 0;
    uint32_t timescale = 0;     // ticks a second; each picture lasts one tick when written here
    uint64_t duration = 0;      // in those ticks
    std::string codec;          // "jpeg"
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> sizes;
};
VIZCORE_API bool read_movie_facts(const std::vector<uint8_t>& file, MovieFacts& out);

}  // namespace viz
