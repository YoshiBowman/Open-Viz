"""Reading and writing the two kinds of DMX-over-network packet Openviz deals in.

Read the way a fixture would read them, not the way a conformance checker would: a console whose
packets bend the standard somewhere (an oddly formed sender id, say) is still a console. The strict
checkers in tools/dmx_sender/verify_capture.py are for testing our own sender.
"""
from __future__ import annotations

import struct

SACN_PORT = 5568
ARTNET_PORT = 6454
ACN_ID = b"ASC-E1.17\x00\x00\x00"


def parse_sacn(data):
    """(universe, priority, source id, levels, terminated, source name) of an E1.31 data packet carrying DMX levels, else None."""
    if len(data) < 126 or data[4:16] != ACN_ID:
        return None
    if struct.unpack_from(">I", data, 18)[0] != 0x00000004 or struct.unpack_from(">I", data, 40)[0] != 0x00000002:
        return None    # not a data packet: synchronisation, discovery and the like
    if data[117] != 0x02 or data[125] != 0x00:
        return None    # not a DMP "set property", or an alternate START code (RDM, text): not levels
    options = data[112]
    if options & 0x80:
        return None    # preview data: for visualizers that asked for it, not for the rig
    count = struct.unpack_from(">H", data, 123)[0] - 1    # the count includes the START code
    if count < 0 or count > 512 or len(data) < 126 + count:
        return None
    universe = struct.unpack_from(">H", data, 113)[0]
    if not 1 <= universe <= 63999:
        return None
    name = bytes(data[44:108]).split(b"\x00", 1)[0].decode("utf-8", "replace")
    return universe, data[108], bytes(data[22:38]), data[126:126 + count], bool(options & 0x40), name


def parse_artdmx(data):
    """(universe, levels) of an ArtDmx packet, else None: the name, the opcode, and as many levels as its length field says."""
    if len(data) < 18 or data[:8] != b"Art-Net\x00" or data[8] | data[9] << 8 != 0x5000:
        return None
    count = data[16] << 8 | data[17]
    if not 1 <= count <= 512 or len(data) < 18 + count:
        return None
    return data[14] | (data[15] & 0x7F) << 8, bytes(data[18:18 + count])


def whole(levels):
    """A universe's levels as all 512 slots, the rest at zero. Unreal's Art-Net reader takes
    512 slots out of every packet whatever length it states: a shorter one fails to read and is
    then a universe of zeros. So what is passed to Unreal is always passed whole."""
    return levels if len(levels) >= 512 else levels + bytes(512 - len(levels))


def artdmx(universe, sequence, levels):
    """An ArtDmx packet. Art-Net asks for an even number of slots, at least two."""
    if len(levels) % 2 or len(levels) < 2:
        levels = levels + bytes(max(2 - len(levels), len(levels) % 2))
    return (b"Art-Net\x00" + struct.pack("<H", 0x5000) + struct.pack(">HBBBBH", 14, sequence, 0, universe & 0xFF, universe >> 8, len(levels))
            + levels)
