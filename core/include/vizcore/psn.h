// PosiStageNet: where tracked things are on stage, as a tracking system tells consoles and
// visualizers (PSN 2.03, VYV and MA Lighting, open and free of royalties).
//
// Packets go by UDP, to the multicast group 236.10.10.10 port 56565 unless the tracking system is
// set otherwise. Everything in them is a chunk: a 32-bit header (little-endian: the id in the low
// 16 bits, the length of what follows in the next 15, and the top bit set when what follows is
// more chunks) and its data. A data packet (0x6755) holds a packet header and a list of trackers,
// each a chunk whose id is the tracker's number, holding its position and whatever else the
// system knows; an info packet (0x6756), sent about once a second, holds the system's name and
// each tracker's name. A frame with many trackers may come in several packets. Chunks of a kind
// not known are passed over, as the protocol asks.
//
// Positions are in metres: x to the right, y up, z into depth. Taken as seen from the house (the
// protocol does not say from where), that is the rig file's x, z and y.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vizcore/types.h"

namespace viz {

constexpr uint16_t kPsnPort = 56565;
constexpr const char* kPsnGroup = "236.10.10.10";

struct PsnTracker {
    uint16_t id = 0;
    std::string name;                  // info packets only
    bool has_position = false;
    float position[3] = {0, 0, 0};     // metres, PSN's axes
    bool has_speed = false;
    float speed[3] = {0, 0, 0};        // metres a second
    bool has_orientation = false;
    float orientation[3] = {0, 0, 0};  // an axis, its length the angle turned about it in radians
    bool has_validity = false;
    float validity = 0;                // how sure the system is; what the number means is its own
    bool has_acceleration = false;
    float acceleration[3] = {0, 0, 0};
    bool has_target = false;
    float target[3] = {0, 0, 0};       // where it is going
    bool has_timestamp = false;
    uint64_t timestamp = 0;            // microseconds since the system started, when this was measured
};

struct PsnPacket {
    enum class Kind { None, Data, Info };
    Kind kind = Kind::None;            // None: not a PSN packet, or not one that could be read
    uint64_t timestamp = 0;            // microseconds since the system started
    uint8_t version_high = 0;
    uint8_t version_low = 0;
    uint8_t frame = 0;                 // the frame this packet is part of
    uint8_t frame_packets = 0;         // how many packets that frame came in
    std::string system_name;           // info packets only
    std::vector<PsnTracker> trackers;
};

// Reads one datagram. A chunk that claims more than the datagram holds is read as far as it goes
// and ends the reading: a tracker cut off before its position is listed without one.
VIZCORE_API PsnPacket parse_psn(const uint8_t* data, size_t size);

// One packet as a tracking system would send it, for tests and the test sender: the trackers'
// fields that are marked as present. It is not split, so keep it under 1500 bytes (about 30
// trackers with a position each).
VIZCORE_API std::vector<uint8_t> psn_data_packet(const std::vector<PsnTracker>& trackers, uint64_t timestamp, uint8_t frame);
VIZCORE_API std::vector<uint8_t> psn_info_packet(const std::string& system_name, const std::vector<PsnTracker>& trackers,
                                                 uint64_t timestamp, uint8_t frame);

// A PSN position in the rig file's terms: millimetres, x to stage left (the house's right), y
// from the house toward upstage, z up.
VIZCORE_API void psn_to_mvr(const float psn[3], double mvr[3]);

}  // namespace viz
