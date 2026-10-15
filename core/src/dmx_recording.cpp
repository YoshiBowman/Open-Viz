#include "vizcore/dmx_recording.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "vizcore/files.h"

namespace viz {

namespace {

const char kMagic[] = "OPENVIZ-DMX 1\n";
const size_t kMagicLength = sizeof(kMagic) - 1;

double little_double(const uint8_t* bytes) {
    uint64_t bits = 0;
    for (int k = 7; k >= 0; --k) bits = (bits << 8) | bytes[k];
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

}  // namespace

DmxRecordingReader::~DmxRecordingReader() {
    if (file_) std::fclose(file_);
}

bool DmxRecordingReader::next(double& time, int& universe, std::vector<uint8_t>& levels) {
    uint8_t head[12];
    if (!file_ || std::fread(head, 1, sizeof head, file_) != sizeof head) return false;
    time = little_double(head);
    universe = head[8] | (head[9] << 8);
    const size_t count = static_cast<size_t>(head[10] | (head[11] << 8));
    levels.resize(count);
    if (count && std::fread(levels.data(), 1, count, file_) != count) return false;
    return std::isfinite(time);
}

bool DmxRecordingReader::open(const std::string& path, std::string& error) {
    if (file_) std::fclose(file_);
    file_ = open_file(path, "rb");
    length_ = 0.0;
    records_ = given_ = 0;
    universes_.clear();
    pending_ = false;
    char magic[sizeof kMagic] = {};
    if (!file_ || std::fread(magic, 1, kMagicLength, file_) != kMagicLength || std::memcmp(magic, kMagic, kMagicLength) != 0) {
        error = file_ ? "not a DMX recording of Openviz's" : "cannot read " + path;
        if (file_) std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    std::set<int> seen;
    double time = 0.0;
    int universe = 0;
    std::vector<uint8_t> levels;
    while (next(time, universe, levels)) {
        ++records_;
        length_ = std::max(length_, time);
        seen.insert(universe);
    }
    universes_.assign(seen.begin(), seen.end());
    std::fseek(file_, static_cast<long>(kMagicLength), SEEK_SET);
    return true;
}

void DmxRecordingReader::play_until(double time, const std::function<void(int universe, const uint8_t* levels, size_t count)>& apply) {
    while (given_ < records_) {
        if (!pending_) {
            if (!next(pending_time_, pending_universe_, pending_levels_)) {
                given_ = records_;    // the file changed under us: nothing more to give
                return;
            }
            pending_ = true;
        }
        if (pending_time_ > time) return;
        apply(pending_universe_, pending_levels_.data(), pending_levels_.size());
        pending_ = false;
        ++given_;
    }
}

}  // namespace viz
