#include "vizcore/movie.h"

#include <cstring>
#include <string_view>

#include "vizcore/files.h"

namespace viz {

namespace {

// QuickTime keeps everything in boxes ("atoms"): a 32-bit length, a four-letter kind, the contents.
struct Box {
    std::vector<uint8_t> bytes;

    void u8(uint8_t value) { bytes.push_back(value); }
    void u16(uint32_t value) {
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value));
    }
    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<uint8_t>(value >> shift));
    }
    void kind(const char* four) { bytes.insert(bytes.end(), four, four + 4); }
    void zeros(size_t count) { bytes.insert(bytes.end(), count, 0); }
    void append(const Box& inner) { bytes.insert(bytes.end(), inner.bytes.begin(), inner.bytes.end()); }
    // The identity in QuickTime's 3x3 fixed-point matrix: 16.16 for a, b, c, d, x, y; 2.30 for u, v, w.
    void identity() {
        for (uint32_t value : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) u32(value);
    }
};

Box box(const char* kind, const Box& contents) {
    Box out;
    out.u32(static_cast<uint32_t>(8 + contents.bytes.size()));
    out.kind(kind);
    out.append(contents);
    return out;
}

Box boxes(const char* kind, std::initializer_list<Box> inside) {
    Box contents;
    for (const Box& inner : inside) contents.append(inner);
    return box(kind, contents);
}

// A handler reference: what kind of data a track carries, and who reads it.
Box handler(const char* component, const char* subtype, std::string_view name) {
    Box b;
    b.u32(0);    // version and flags
    b.kind(component);
    b.kind(subtype);
    b.zeros(12);    // manufacturer, flags, flags mask
    b.u8(static_cast<uint8_t>(name.size()));    // QuickTime's names are counted, not ended by a zero
    b.bytes.insert(b.bytes.end(), name.begin(), name.end());
    return box("hdlr", b);
}

}  // namespace

MovieWriter::~MovieWriter() {
    if (file_) std::fclose(file_);
}

bool MovieWriter::open(const std::string& path, int width, int height, int frames_per_second, std::string& error) {
    if (file_) {
        error = "a movie is being written already";
        return false;
    }
    if (width < 1 || height < 1 || width > 16384 || height > 16384 || frames_per_second < 1 || frames_per_second > 240) {
        error = "a movie is 1 to 16384 pixels each way, at 1 to 240 frames a second";
        return false;
    }
    path_ = path;
    file_ = open_file(path + ".part", "wb");
    if (!file_) {
        error = "cannot write " + path;
        return false;
    }
    width_ = width;
    height_ = height;
    rate_ = frames_per_second;
    failed_ = false;
    written_ = 0;
    sizes_.clear();
    offsets_.clear();
    // The kind of file, then the box the pictures go in, with a 64-bit length to be filled in at the end.
    Box head;
    Box ftyp;
    ftyp.kind("qt  ");
    ftyp.u32(0x20050300);
    ftyp.kind("qt  ");
    head.append(box("ftyp", ftyp));
    head.u32(1);    // a length of 1: the real one follows, 64 bits long
    head.kind("mdat");
    head.u64(0);
    data_start_ = head.bytes.size();
    if (std::fwrite(head.bytes.data(), 1, head.bytes.size(), file_) != head.bytes.size()) {
        error = "cannot write " + path;
        std::fclose(file_);
        file_ = nullptr;
        remove_file(path + ".part");
        return false;
    }
    return true;
}

bool MovieWriter::add(const uint8_t* jpeg, size_t size) {
    if (!file_ || failed_ || !jpeg || size == 0 || size > 0xFFFFFFFFu) return false;    // refused; the movie goes on
    if (std::fwrite(jpeg, 1, size, file_) != size) {
        failed_ = true;
        return false;
    }
    offsets_.push_back(data_start_ + written_);
    sizes_.push_back(static_cast<uint32_t>(size));
    written_ += size;
    return true;
}

bool MovieWriter::close(std::string& error) {
    if (!file_) {
        error = "no movie is being written";
        return false;
    }
    const uint32_t frames = static_cast<uint32_t>(sizes_.size());
    const uint32_t movie_scale = 1000;
    const uint64_t movie_duration = static_cast<uint64_t>(frames) * movie_scale / static_cast<uint64_t>(rate_);

    // The movie as a whole, then its one track of pictures.
    Box mvhd;
    mvhd.u32(0);    // version 0, no flags
    mvhd.u32(0);    // made
    mvhd.u32(0);    // changed
    mvhd.u32(movie_scale);
    mvhd.u32(static_cast<uint32_t>(movie_duration));
    mvhd.u32(0x00010000);    // played at normal speed
    mvhd.u16(0x0100);        // at full volume
    mvhd.zeros(10);
    mvhd.identity();
    mvhd.zeros(24);          // preview, poster, selection and current time
    mvhd.u32(2);             // the next track's number

    Box tkhd;
    tkhd.u32(0x0000000F);    // enabled, in the movie, in its preview and poster
    tkhd.u32(0);
    tkhd.u32(0);
    tkhd.u32(1);             // track 1
    tkhd.u32(0);
    tkhd.u32(static_cast<uint32_t>(movie_duration));
    tkhd.zeros(8);
    tkhd.u16(0);             // layer
    tkhd.u16(0);             // alternate group
    tkhd.u16(0);             // volume: a picture track has none
    tkhd.u16(0);
    tkhd.identity();
    tkhd.u32(static_cast<uint32_t>(width_) << 16);
    tkhd.u32(static_cast<uint32_t>(height_) << 16);

    Box mdhd;
    mdhd.u32(0);
    mdhd.u32(0);
    mdhd.u32(0);
    mdhd.u32(static_cast<uint32_t>(rate_));    // a tick a picture
    mdhd.u32(frames);
    mdhd.u16(0x7FFF);    // no language
    mdhd.u16(0);         // quality

    Box vmhd;
    vmhd.u32(0x00000001);
    vmhd.u16(0x0040);    // drawn by dithered copy
    vmhd.u16(0x8000);
    vmhd.u16(0x8000);
    vmhd.u16(0x8000);

    Box alis;
    alis.u32(0x00000001);    // the data is in this file
    Box dref;
    dref.u32(0);
    dref.u32(1);
    dref.append(box("alis", alis));

    // How the pictures are made: JPEG, 72 dots an inch, 24 bits.
    Box entry;
    entry.zeros(6);
    entry.u16(1);            // data reference 1: this file
    entry.u16(0);            // version
    entry.u16(0);            // revision
    entry.kind("appl");
    entry.u32(0);            // temporal quality
    entry.u32(0x00000200);   // spatial quality: normal
    entry.u16(static_cast<uint32_t>(width_));
    entry.u16(static_cast<uint32_t>(height_));
    entry.u32(0x00480000);
    entry.u32(0x00480000);
    entry.u32(0);            // data size
    entry.u16(1);            // one picture a sample
    const char name[] = "Photo - JPEG";
    entry.u8(static_cast<uint8_t>(sizeof(name) - 1));
    entry.bytes.insert(entry.bytes.end(), name, name + sizeof(name) - 1);
    entry.zeros(31 - (sizeof(name) - 1));
    entry.u16(24);           // colour depth
    entry.u16(0xFFFF);       // no colour table
    Box stsd;
    stsd.u32(0);
    stsd.u32(1);
    stsd.append(box("jpeg", entry));

    Box stts;    // every picture lasts one tick
    stts.u32(0);
    stts.u32(frames ? 1 : 0);
    if (frames) {
        stts.u32(frames);
        stts.u32(1);
    }
    Box stss;    // every picture stands on its own
    stss.u32(0);
    stss.u32(frames);
    for (uint32_t n = 1; n <= frames; ++n) stss.u32(n);
    Box stsc;    // one picture to each chunk
    stsc.u32(0);
    stsc.u32(frames ? 1 : 0);
    if (frames) {
        stsc.u32(1);
        stsc.u32(1);
        stsc.u32(1);
    }
    Box stsz;
    stsz.u32(0);
    stsz.u32(0);    // sizes vary: each is listed
    stsz.u32(frames);
    for (uint32_t size : sizes_) stsz.u32(size);
    Box co64;
    co64.u32(0);
    co64.u32(frames);
    for (uint64_t offset : offsets_) co64.u64(offset);

    Box stbl = boxes("stbl", {box("stsd", stsd), box("stts", stts), box("stss", stss), box("stsc", stsc), box("stsz", stsz), box("co64", co64)});
    Box minf = boxes("minf", {box("vmhd", vmhd), handler("dhlr", "alis", "Openviz data"), boxes("dinf", {box("dref", dref)}), stbl});
    Box mdia = boxes("mdia", {box("mdhd", mdhd), handler("mhlr", "vide", "Openviz pictures"), minf});
    Box trak = boxes("trak", {box("tkhd", tkhd), mdia});
    Box moov = boxes("moov", {box("mvhd", mvhd), trak});

    bool ok = !failed_ && std::fwrite(moov.bytes.data(), 1, moov.bytes.size(), file_) == moov.bytes.size();
    // The pictures' box is as long as its header and the pictures: written into its header now.
    if (ok) {
        Box length;
        length.u64(16 + written_);
        ok = std::fseek(file_, static_cast<long>(data_start_ - 8), SEEK_SET) == 0 &&
             std::fwrite(length.bytes.data(), 1, 8, file_) == 8;
    }
    ok = std::fclose(file_) == 0 && ok;
    file_ = nullptr;
    const std::string partial = path_ + ".part";
    if (ok) {
        ok = rename_file(partial, path_);
    }
    if (!ok) {
        error = "cannot write " + path_;
        remove_file(partial);
        return false;
    }
    return true;
}

namespace {

struct Reader {
    const std::vector<uint8_t>& data;
    uint32_t u32(size_t at) const {
        return at + 4 <= data.size() ? (static_cast<uint32_t>(data[at]) << 24) | (static_cast<uint32_t>(data[at + 1]) << 16) |
                                           (static_cast<uint32_t>(data[at + 2]) << 8) | data[at + 3]
                                     : 0;
    }
    uint64_t u64(size_t at) const { return (static_cast<uint64_t>(u32(at)) << 32) | u32(at + 4); }
    uint32_t u16(size_t at) const { return at + 2 <= data.size() ? (static_cast<uint32_t>(data[at]) << 8) | data[at + 1] : 0; }

    // Walks the boxes from `from` to `to`, going into the ones that hold others.
    bool walk(size_t from, size_t to, MovieFacts& out, int depth) const {
        if (depth > 8) return false;
        for (size_t at = from; at + 8 <= to;) {
            uint64_t length = u32(at);
            const std::string kind(reinterpret_cast<const char*>(&data[at + 4]), 4);
            size_t header = 8;
            if (length == 1) {
                length = u64(at + 8);
                header = 16;
            } else if (length == 0) {
                length = to - at;
            }
            if (length < header || length > to - at) return false;
            const size_t inside = at + header, end = at + static_cast<size_t>(length);
            if (kind == "moov" || kind == "trak" || kind == "mdia" || kind == "minf" || kind == "stbl") {
                if (!walk(inside, end, out, depth + 1)) return false;
            } else if (kind == "mdhd") {
                out.timescale = u32(inside + 12);
                out.duration = u32(inside + 16);
            } else if (kind == "stsd" && u32(inside + 4) >= 1) {
                const size_t first = inside + 8;
                out.codec = std::string(reinterpret_cast<const char*>(&data[first + 4]), 4);
                out.width = static_cast<int>(u16(first + 8 + 24));
                out.height = static_cast<int>(u16(first + 8 + 26));
            } else if (kind == "stsz") {
                const uint32_t count = u32(inside + 8);
                if (u32(inside + 4) != 0 || inside + 12 + static_cast<size_t>(count) * 4 > end) return false;
                for (uint32_t n = 0; n < count; ++n) out.sizes.push_back(u32(inside + 12 + n * 4));
            } else if (kind == "co64") {
                const uint32_t count = u32(inside + 4);
                if (inside + 8 + static_cast<size_t>(count) * 8 > end) return false;
                for (uint32_t n = 0; n < count; ++n) out.offsets.push_back(u64(inside + 8 + static_cast<size_t>(n) * 8));
            }
            at = end;
        }
        return true;
    }
};

}  // namespace

bool read_movie_facts(const std::vector<uint8_t>& file, MovieFacts& out) {
    out = MovieFacts();
    const Reader reader{file};
    return reader.walk(0, file.size(), out, 0) && !out.codec.empty() && out.sizes.size() == out.offsets.size();
}

}  // namespace viz
