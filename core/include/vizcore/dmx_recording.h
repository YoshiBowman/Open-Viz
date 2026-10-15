// DMX recordings as Openviz makes them (launcher/dmx_record.py), read back in time order: for
// rendering a recording to video frame by frame, where the levels must change on the frame they
// were recorded at and not whenever the network delivers them.
//
// The file: the line "OPENVIZ-DMX 1", then records, each a time in seconds from the start (an
// 8-byte float), a universe number and a count of levels (two bytes each, all little-endian),
// and that many levels. A universe that does not change is still written once a second.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "vizcore/types.h"

namespace viz {

class VIZCORE_API DmxRecordingReader {
public:
    DmxRecordingReader() = default;
    ~DmxRecordingReader();
    DmxRecordingReader(const DmxRecordingReader&) = delete;
    DmxRecordingReader& operator=(const DmxRecordingReader&) = delete;

    // Reads the file through once, for its length and its universes, and stands at its start.
    // A record cut short at the end (a recording that was still being written) is left out.
    bool open(const std::string& path, std::string& error);
    double length() const { return length_; }                  // seconds, the time of the last record
    size_t records() const { return records_; }
    const std::vector<int>& universes() const { return universes_; }    // in increasing order

    // Gives every record up to and including `time`, in the order they were written, that has not
    // been given yet: universe, levels and how many. Times only go forward; to go back, open again.
    void play_until(double time, const std::function<void(int universe, const uint8_t* levels, size_t count)>& apply);
    bool finished() const { return given_ >= records_; }

private:
    bool next(double& time, int& universe, std::vector<uint8_t>& levels);

    std::FILE* file_ = nullptr;
    double length_ = 0.0;
    size_t records_ = 0;
    size_t given_ = 0;
    std::vector<int> universes_;
    bool pending_ = false;             // a record read ahead, waiting for its time
    double pending_time_ = 0.0;
    int pending_universe_ = 0;
    std::vector<uint8_t> pending_levels_;
};

}  // namespace viz
