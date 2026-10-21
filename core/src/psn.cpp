#include "vizcore/psn.h"

#include <cstring>

namespace viz {

namespace {

constexpr uint16_t kDataPacket = 0x6755;
constexpr uint16_t kInfoPacket = 0x6756;
constexpr uint16_t kPacketHeader = 0x0000;
constexpr uint16_t kDataTrackerList = 0x0001;
constexpr uint16_t kInfoSystemName = 0x0001;
constexpr uint16_t kInfoTrackerList = 0x0002;
constexpr uint16_t kInfoTrackerName = 0x0000;
enum : uint16_t { kPos = 0, kSpeed = 1, kOri = 2, kStatus = 3, kAccel = 4, kTarget = 5, kTimestamp = 6 };

struct Chunk {
    uint16_t id = 0;
    bool more = false;          // what it holds is chunks
    const uint8_t* data = nullptr;
    size_t size = 0;
};

uint32_t u32(const uint8_t* at) {
    return static_cast<uint32_t>(at[0]) | static_cast<uint32_t>(at[1]) << 8 | static_cast<uint32_t>(at[2]) << 16 |
           static_cast<uint32_t>(at[3]) << 24;
}

uint64_t u64(const uint8_t* at) { return static_cast<uint64_t>(u32(at)) | static_cast<uint64_t>(u32(at + 4)) << 32; }

float f32(const uint8_t* at) {
    const uint32_t bits = u32(at);
    float value = 0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

// The chunks one after another in [data, data + size). One that claims more than is there is
// read as far as it goes, and is the last.
template <typename Each>
void chunks(const uint8_t* data, size_t size, Each each) {
    size_t at = 0;
    while (at + 4 <= size) {
        const uint32_t head = u32(data + at);
        Chunk chunk;
        chunk.id = static_cast<uint16_t>(head & 0xFFFF);
        chunk.size = (head >> 16) & 0x7FFF;
        chunk.more = (head >> 31) != 0;
        chunk.data = data + at + 4;
        const bool cut = at + 4 + chunk.size > size;
        if (cut) chunk.size = size - at - 4;
        each(chunk);
        if (cut) return;
        at += 4 + chunk.size;
    }
}

bool three(const Chunk& chunk, float out[3]) {
    if (chunk.size < 12) return false;
    for (int k = 0; k < 3; ++k) out[k] = f32(chunk.data + 4 * k);
    return true;
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(value >> (8 * k)));
}

void put64(std::vector<uint8_t>& out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value));
    put32(out, static_cast<uint32_t>(value >> 32));
}

void put_float(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    put32(out, bits);
}

// A chunk header whose length is filled in by close() once what it holds is written.
size_t open_chunk(std::vector<uint8_t>& out, uint16_t id, bool more) {
    const size_t at = out.size();
    put32(out, static_cast<uint32_t>(id) | (more ? 0x80000000u : 0u));
    return at;
}

void close_chunk(std::vector<uint8_t>& out, size_t at) {
    const uint32_t length = static_cast<uint32_t>(out.size() - at - 4) & 0x7FFF;
    const uint32_t head = (u32(out.data() + at) & 0x8000FFFFu) | length << 16;
    for (int k = 0; k < 4; ++k) out[at + static_cast<size_t>(k)] = static_cast<uint8_t>(head >> (8 * k));
}

void put_header(std::vector<uint8_t>& out, uint64_t timestamp, uint8_t frame) {
    const size_t header = open_chunk(out, kPacketHeader, false);
    put64(out, timestamp);
    out.push_back(2);    // version 2.03
    out.push_back(3);
    out.push_back(frame);
    out.push_back(1);    // one packet to the frame
    close_chunk(out, header);
}

void put_three(std::vector<uint8_t>& out, uint16_t id, const float value[3]) {
    const size_t field = open_chunk(out, id, false);
    for (int k = 0; k < 3; ++k) put_float(out, value[k]);
    close_chunk(out, field);
}

}  // namespace

PsnPacket parse_psn(const uint8_t* data, size_t size) {
    PsnPacket packet;
    if (!data || size < 4) return packet;
    const uint32_t head = u32(data);
    const uint16_t id = static_cast<uint16_t>(head & 0xFFFF);
    if ((id != kDataPacket && id != kInfoPacket) || (head >> 31) == 0) return packet;
    const size_t length = (head >> 16) & 0x7FFF;
    const size_t whole = length + 4 <= size ? length : size - 4;    // a length past the end: read what came
    const bool info = id == kInfoPacket;
    bool header_read = false;
    chunks(data + 4, whole, [&](const Chunk& chunk) {
        if (chunk.id == kPacketHeader && !chunk.more && chunk.size >= 12) {
            packet.timestamp = u64(chunk.data);
            packet.version_high = chunk.data[8];
            packet.version_low = chunk.data[9];
            packet.frame = chunk.data[10];
            packet.frame_packets = chunk.data[11];
            header_read = true;
        } else if (info && chunk.id == kInfoSystemName && !chunk.more) {
            packet.system_name.assign(reinterpret_cast<const char*>(chunk.data), chunk.size);
        } else if (chunk.more && chunk.id == (info ? kInfoTrackerList : kDataTrackerList)) {
            chunks(chunk.data, chunk.size, [&](const Chunk& entry) {
                if (!entry.more) return;
                PsnTracker tracker;
                tracker.id = entry.id;
                chunks(entry.data, entry.size, [&](const Chunk& field) {
                    if (field.more) return;
                    if (info) {
                        if (field.id == kInfoTrackerName) tracker.name.assign(reinterpret_cast<const char*>(field.data), field.size);
                        return;
                    }
                    switch (field.id) {
                        case kPos: tracker.has_position = three(field, tracker.position); break;
                        case kSpeed: tracker.has_speed = three(field, tracker.speed); break;
                        case kOri: tracker.has_orientation = three(field, tracker.orientation); break;
                        case kAccel: tracker.has_acceleration = three(field, tracker.acceleration); break;
                        case kTarget: tracker.has_target = three(field, tracker.target); break;
                        case kStatus:
                            if (field.size >= 4) {
                                tracker.validity = f32(field.data);
                                tracker.has_validity = true;
                            }
                            break;
                        case kTimestamp:
                            if (field.size >= 8) {
                                tracker.timestamp = u64(field.data);
                                tracker.has_timestamp = true;
                            }
                            break;
                        default: break;
                    }
                });
                packet.trackers.push_back(tracker);
            });
        }
    });
    if (header_read) packet.kind = info ? PsnPacket::Kind::Info : PsnPacket::Kind::Data;
    else packet.trackers.clear();
    return packet;
}

std::vector<uint8_t> psn_data_packet(const std::vector<PsnTracker>& trackers, uint64_t timestamp, uint8_t frame) {
    std::vector<uint8_t> out;
    const size_t packet = open_chunk(out, kDataPacket, true);
    put_header(out, timestamp, frame);
    const size_t list = open_chunk(out, kDataTrackerList, true);
    for (const PsnTracker& tracker : trackers) {
        const size_t entry = open_chunk(out, tracker.id, true);
        if (tracker.has_position) put_three(out, kPos, tracker.position);
        if (tracker.has_speed) put_three(out, kSpeed, tracker.speed);
        if (tracker.has_orientation) put_three(out, kOri, tracker.orientation);
        if (tracker.has_validity) {
            const size_t field = open_chunk(out, kStatus, false);
            put_float(out, tracker.validity);
            close_chunk(out, field);
        }
        if (tracker.has_acceleration) put_three(out, kAccel, tracker.acceleration);
        if (tracker.has_target) put_three(out, kTarget, tracker.target);
        if (tracker.has_timestamp) {
            const size_t field = open_chunk(out, kTimestamp, false);
            put64(out, tracker.timestamp);
            close_chunk(out, field);
        }
        close_chunk(out, entry);
    }
    close_chunk(out, list);
    close_chunk(out, packet);
    return out;
}

std::vector<uint8_t> psn_info_packet(const std::string& system_name, const std::vector<PsnTracker>& trackers, uint64_t timestamp,
                                     uint8_t frame) {
    std::vector<uint8_t> out;
    const size_t packet = open_chunk(out, kInfoPacket, true);
    put_header(out, timestamp, frame);
    const size_t name = open_chunk(out, kInfoSystemName, false);
    out.insert(out.end(), system_name.begin(), system_name.end());
    close_chunk(out, name);
    const size_t list = open_chunk(out, kInfoTrackerList, true);
    for (const PsnTracker& tracker : trackers) {
        const size_t entry = open_chunk(out, tracker.id, true);
        const size_t field = open_chunk(out, kInfoTrackerName, false);
        out.insert(out.end(), tracker.name.begin(), tracker.name.end());
        close_chunk(out, field);
        close_chunk(out, entry);
    }
    close_chunk(out, list);
    close_chunk(out, packet);
    return out;
}

void psn_to_mvr(const float psn[3], double mvr[3]) {
    mvr[0] = static_cast<double>(psn[0]) * 1000.0;
    mvr[1] = static_cast<double>(psn[2]) * 1000.0;
    mvr[2] = static_cast<double>(psn[1]) * 1000.0;
}

}  // namespace viz
