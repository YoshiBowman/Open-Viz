#include "vizcore/movie.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <string_view>

#include "vizcore/files.h"

namespace viz {

namespace {

// Seeks past 2 GB too, which a long cannot on Windows; movies get that large.
bool seek_to(std::FILE* file, uint64_t at) {
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(at), SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}

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

void MovieWriter::set_sound(const Sound& sound, double start_seconds) {
    sound_ = sound;
    sound_start_ = std::isfinite(start_seconds) ? start_seconds : 0.0;
}

bool MovieWriter::close(std::string& error) {
    if (!file_) {
        error = "no movie is being written";
        return false;
    }
    const uint32_t frames = static_cast<uint32_t>(sizes_.size());
    const uint32_t movie_scale = 1000;
    const uint64_t movie_duration = static_cast<uint64_t>(frames) * movie_scale / static_cast<uint64_t>(rate_);

    // The sound, after the pictures: as long as the movie, in chunks of a second.
    const bool with_sound = frames > 0 && sound_.rate > 0 && sound_.rate <= 65535 && (sound_.channels == 1 || sound_.channels == 2);
    const uint32_t sound_frames = with_sound ? static_cast<uint32_t>(std::llround(static_cast<double>(frames) / rate_ * sound_.rate)) : 0;
    std::vector<uint64_t> sound_offsets;
    if (with_sound) {
        const int64_t first = std::llround(sound_start_ * sound_.rate);    // the sound's frame at the movie's start
        const int64_t available = static_cast<int64_t>(sound_.frames());
        const size_t channels = static_cast<size_t>(sound_.channels);
        std::vector<uint8_t> chunk;
        for (uint32_t done = 0; done < sound_frames && !failed_;) {
            const uint32_t count = std::min<uint32_t>(sound_frames - done, static_cast<uint32_t>(sound_.rate));
            chunk.assign(static_cast<size_t>(count) * channels * 2, 0);
            for (uint32_t f = 0; f < count; ++f) {
                const int64_t from = first + done + f;
                if (from < 0 || from >= available) continue;    // silence
                for (size_t c = 0; c < channels; ++c) {
                    const uint16_t value = static_cast<uint16_t>(sound_.samples[static_cast<size_t>(from) * channels + c]);
                    chunk[(static_cast<size_t>(f) * channels + c) * 2] = static_cast<uint8_t>(value);    // little-endian: "sowt"
                    chunk[(static_cast<size_t>(f) * channels + c) * 2 + 1] = static_cast<uint8_t>(value >> 8);
                }
            }
            if (std::fwrite(chunk.data(), 1, chunk.size(), file_) != chunk.size()) failed_ = true;
            sound_offsets.push_back(data_start_ + written_);
            written_ += chunk.size();
            done += count;
        }
    }

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
    mvhd.u32(with_sound ? 3 : 2);    // the next track's number

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
    Box moov_contents;
    moov_contents.append(box("mvhd", mvhd));
    moov_contents.append(trak);

    if (with_sound) {
        // Track 2: the sound, uncompressed 16-bit samples, little-endian ("sowt"), one sample a frame.
        Box sound_tkhd;
        sound_tkhd.u32(0x00000007);    // enabled, in the movie and its preview
        sound_tkhd.u32(0);
        sound_tkhd.u32(0);
        sound_tkhd.u32(2);
        sound_tkhd.u32(0);
        sound_tkhd.u32(static_cast<uint32_t>(movie_duration));
        sound_tkhd.zeros(8);
        sound_tkhd.u16(0);
        sound_tkhd.u16(0);
        sound_tkhd.u16(0x0100);        // full volume
        sound_tkhd.u16(0);
        sound_tkhd.identity();
        sound_tkhd.u32(0);
        sound_tkhd.u32(0);
        Box sound_mdhd;
        sound_mdhd.u32(0);
        sound_mdhd.u32(0);
        sound_mdhd.u32(0);
        sound_mdhd.u32(static_cast<uint32_t>(sound_.rate));
        sound_mdhd.u32(sound_frames);
        sound_mdhd.u16(0x7FFF);
        sound_mdhd.u16(0);
        Box smhd;
        smhd.u32(0);
        smhd.u16(0);    // balance: the middle
        smhd.u16(0);
        Box sowt;
        sowt.zeros(6);
        sowt.u16(1);    // data reference 1: this file
        sowt.u16(0);    // version 0
        sowt.u16(0);
        sowt.u32(0);
        sowt.u16(static_cast<uint32_t>(sound_.channels));
        sowt.u16(16);   // bits a sample
        sowt.u16(0);    // compression id
        sowt.u16(0);    // packet size
        sowt.u32(static_cast<uint32_t>(sound_.rate) << 16);
        Box sound_stsd;
        sound_stsd.u32(0);
        sound_stsd.u32(1);
        sound_stsd.append(box("sowt", sowt));
        Box sound_stts;
        sound_stts.u32(0);
        sound_stts.u32(1);
        sound_stts.u32(sound_frames);
        sound_stts.u32(1);
        const uint32_t chunks = static_cast<uint32_t>(sound_offsets.size());
        const uint32_t per_chunk = static_cast<uint32_t>(sound_.rate);
        const uint32_t last = sound_frames - per_chunk * (chunks - 1);
        Box sound_stsc;
        sound_stsc.u32(0);
        sound_stsc.u32(chunks > 1 && last != per_chunk ? 2 : 1);
        sound_stsc.u32(1);
        sound_stsc.u32(chunks > 1 ? per_chunk : last);
        sound_stsc.u32(1);
        if (chunks > 1 && last != per_chunk) {
            sound_stsc.u32(chunks);
            sound_stsc.u32(last);
            sound_stsc.u32(1);
        }
        Box sound_stsz;
        sound_stsz.u32(0);
        sound_stsz.u32(static_cast<uint32_t>(sound_.channels) * 2);    // every frame the same size
        sound_stsz.u32(sound_frames);
        Box sound_co64;
        sound_co64.u32(0);
        sound_co64.u32(chunks);
        for (uint64_t offset : sound_offsets) sound_co64.u64(offset);
        Box sound_stbl = boxes("stbl", {box("stsd", sound_stsd), box("stts", sound_stts), box("stsc", sound_stsc), box("stsz", sound_stsz), box("co64", sound_co64)});
        Box sound_minf = boxes("minf", {box("smhd", smhd), handler("dhlr", "alis", "Openviz data"), boxes("dinf", {box("dref", dref)}), sound_stbl});
        Box sound_mdia = boxes("mdia", {box("mdhd", sound_mdhd), handler("mhlr", "soun", "Openviz sound"), sound_minf});
        moov_contents.append(boxes("trak", {box("tkhd", sound_tkhd), sound_mdia}));
    }
    Box moov = box("moov", moov_contents);

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

    struct Track {
        std::string handler;    // "vide", "soun"
        uint32_t timescale = 0;
        uint64_t duration = 0;
        std::string codec;
        int width = 0, height = 0, channels = 0, rate = 0;
        uint32_t sample_size = 0, sample_count = 0;
        std::vector<uint32_t> sizes;
        std::vector<uint64_t> offsets;
    };

    // Walks the boxes from `from` to `to`, going into the ones that hold others; each track's facts go into `track`.
    bool walk(size_t from, size_t to, MovieFacts& out, Track* track, int depth) const {
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
            if (kind == "trak") {
                Track found;
                if (!walk(inside, end, out, &found, depth + 1)) return false;
                if (found.handler == "soun") {
                    out.sound_codec = found.codec;
                    out.sound_rate = found.rate;
                    out.sound_channels = found.channels;
                    out.sound_frames = found.sample_count;
                    out.sound_offsets = found.offsets;
                } else {
                    if (found.sample_size != 0) return false;    // pictures are listed one by one
                    out.timescale = found.timescale;
                    out.duration = found.duration;
                    out.codec = found.codec;
                    out.width = found.width;
                    out.height = found.height;
                    out.sizes = found.sizes;
                    out.offsets = found.offsets;
                }
            } else if (kind == "moov" || kind == "mdia" || kind == "minf" || kind == "stbl") {
                if (!walk(inside, end, out, track, depth + 1)) return false;
            } else if (!track) {
                // outside a track: nothing to read
            } else if (kind == "mdhd") {
                track->timescale = u32(inside + 12);
                track->duration = u32(inside + 16);
            } else if (kind == "hdlr" && inside + 12 <= end) {
                const std::string subtype(reinterpret_cast<const char*>(&data[inside + 8]), 4);
                if (subtype == "vide" || subtype == "soun") track->handler = subtype;
            } else if (kind == "stsd" && u32(inside + 4) >= 1 && inside + 8 + 36 <= end) {
                const size_t first = inside + 8;
                track->codec = std::string(reinterpret_cast<const char*>(&data[first + 4]), 4);
                track->width = static_cast<int>(u16(first + 8 + 24));
                track->height = static_cast<int>(u16(first + 8 + 26));
                track->channels = static_cast<int>(u16(first + 8 + 16));    // where a sound's description has them
                track->rate = static_cast<int>(u32(first + 8 + 24) >> 16);
            } else if (kind == "stsz") {
                track->sample_size = u32(inside + 4);
                track->sample_count = u32(inside + 8);
                if (track->sample_size == 0) {
                    if (inside + 12 + static_cast<size_t>(track->sample_count) * 4 > end) return false;
                    for (uint32_t n = 0; n < track->sample_count; ++n) track->sizes.push_back(u32(inside + 12 + n * 4));
                }
            } else if (kind == "co64") {
                const uint32_t count = u32(inside + 4);
                if (inside + 8 + static_cast<size_t>(count) * 8 > end) return false;
                for (uint32_t n = 0; n < count; ++n) track->offsets.push_back(u64(inside + 8 + static_cast<size_t>(n) * 8));
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
    return reader.walk(0, file.size(), out, nullptr, 0) && !out.codec.empty() && out.sizes.size() == out.offsets.size();
}

bool read_wav(const std::string& path, Sound& out, std::string& error) {
    out = Sound();
    std::ifstream stream = read_stream(path);
    if (!stream) {
        error = "cannot read " + path;
        return false;
    }
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    auto u16 = [&file](size_t at) { return at + 2 <= file.size() ? static_cast<uint32_t>(file[at] | (file[at + 1] << 8)) : 0u; };
    auto u32 = [&file](size_t at) {
        return at + 4 <= file.size() ? static_cast<uint32_t>(file[at]) | (static_cast<uint32_t>(file[at + 1]) << 8) | (static_cast<uint32_t>(file[at + 2]) << 16) |
                                           (static_cast<uint32_t>(file[at + 3]) << 24)
                                     : 0u;
    };
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
        error = "not a WAV file";
        return false;
    }
    uint32_t format = 0, channels = 0, rate = 0, align = 0, bits = 0;
    size_t data_at = 0, data_size = 0;
    for (size_t at = 12; at + 8 <= file.size();) {
        const uint32_t size = u32(at + 4);
        if (std::memcmp(&file[at], "fmt ", 4) == 0 && size >= 16) {
            format = u16(at + 8);
            channels = u16(at + 10);
            rate = u32(at + 12);
            align = u16(at + 20);
            bits = u16(at + 22);
            if (format == 0xFFFE && size >= 26) format = u16(at + 8 + 24);    // extensible: the real format starts its sub-format
        } else if (std::memcmp(&file[at], "data", 4) == 0) {
            data_at = at + 8;
            data_size = std::min<size_t>(size, file.size() - data_at);    // a streamed file may say more than it has
            break;
        }
        at += 8 + static_cast<size_t>(size) + (size & 1);    // chunks are padded to an even length
    }
    const bool pcm = format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32);
    const bool floats = format == 3 && bits == 32;
    if (!(pcm || floats) || channels < 1 || channels > 32 || rate < 1 || align != channels * bits / 8 || data_at == 0) {
        error = "the WAV file is not plain PCM or floating point sound";
        return false;
    }
    if (rate > 65535) {
        error = "the WAV file has more than 65,535 samples a second";
        return false;
    }
    out.rate = static_cast<int>(rate);
    out.channels = static_cast<int>(std::min<uint32_t>(channels, 2));
    const size_t frames = data_size / align;
    out.samples.resize(frames * static_cast<size_t>(out.channels));
    for (size_t f = 0; f < frames; ++f) {
        for (int c = 0; c < out.channels; ++c) {
            const size_t at = data_at + f * align + static_cast<size_t>(c) * (bits / 8);
            int32_t value = 0;
            if (floats) {
                uint32_t raw = u32(at);
                float sample = 0.0f;
                std::memcpy(&sample, &raw, 4);
                const float held = std::isfinite(sample) ? std::min(std::max(sample, -1.0f), 1.0f) : 0.0f;
                value = static_cast<int32_t>(std::lround(held * 32767.0f));
            } else if (bits == 8) {
                value = (static_cast<int32_t>(file[at]) - 128) * 256;    // eight-bit WAV is unsigned
            } else if (bits == 16) {
                value = static_cast<int16_t>(u16(at));
            } else if (bits == 24) {
                value = static_cast<int32_t>((u32(at) & 0xFFFFFF) << 8) >> 16;
            } else {
                value = static_cast<int32_t>(u32(at)) >> 16;
            }
            out.samples[f * static_cast<size_t>(out.channels) + static_cast<size_t>(c)] = static_cast<int16_t>(value);
        }
    }
    return true;
}

bool movie_with_sound(const std::string& movie_path, const std::string& wav_path, double start_seconds, const std::string& out_path, std::string& error) {
    Sound sound;
    if (!read_wav(wav_path, sound, error)) return false;
    std::FILE* in = open_file(movie_path, "rb");
    if (!in) {
        error = "cannot read " + movie_path;
        return false;
    }
    // The index is in the box "moov", at the end of a movie written here: found by stepping from box to box.
    std::vector<uint8_t> moov;
    uint64_t at = 0;
    for (int boxes_seen = 0; boxes_seen < 64 && moov.empty(); ++boxes_seen) {
        uint8_t head[16];
        if (!seek_to(in, at) || std::fread(head, 1, 8, in) != 8) break;
        uint64_t length = (static_cast<uint64_t>(head[0]) << 24) | (static_cast<uint64_t>(head[1]) << 16) | (static_cast<uint64_t>(head[2]) << 8) | head[3];
        if (length == 1) {
            if (std::fread(head + 8, 1, 8, in) != 8) break;
            length = 0;
            for (int k = 8; k < 16; ++k) length = (length << 8) | head[k];
        }
        if (length < 8) break;
        if (std::memcmp(head + 4, "moov", 4) == 0) {
            if (length > (64u << 20)) break;    // an index that large is not one of ours
            moov.resize(static_cast<size_t>(length));
            if (!seek_to(in, at) || std::fread(moov.data(), 1, moov.size(), in) != moov.size()) moov.clear();
            break;
        }
        at += length;
    }
    MovieFacts facts;
    if (moov.empty() || !read_movie_facts(moov, facts) || facts.codec != "jpeg") {
        std::fclose(in);
        error = movie_path + " is not a movie the visualizer made";
        return false;
    }
    auto writer = std::make_unique<MovieWriter>();
    if (!writer->open(out_path, facts.width, facts.height, static_cast<int>(facts.timescale), error)) {
        std::fclose(in);
        return false;
    }
    std::vector<uint8_t> picture;
    for (size_t n = 0; n < facts.sizes.size(); ++n) {
        picture.resize(facts.sizes[n]);
        if (!seek_to(in, facts.offsets[n]) || std::fread(picture.data(), 1, picture.size(), in) != picture.size() || !writer->add(picture.data(), picture.size())) {
            std::fclose(in);
            writer.reset();    // closes what it wrote, which is then thrown away
            remove_file(out_path + ".part");
            error = "cannot copy the pictures of " + movie_path;
            return false;
        }
    }
    std::fclose(in);
    writer->set_sound(sound, start_seconds);
    return writer->close(error);
}

}  // namespace viz
