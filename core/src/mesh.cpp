#include "vizcore/mesh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_map>

#include "vizcore/evaluate.h"

#include "json.h"
#include "text.h"
#include "vizcore/files.h"

namespace viz {

namespace {

const size_t kMaxVertices = 4000000;
const size_t kMaxImageBytes = 32u << 20;
const size_t kMaxMaterials = 4096;
const double kCreaseCos = 0.70710678;    // cos 45 degrees

// What a reader gathers before it is checked and finished: any units, no normals required.
struct Raw {
    std::vector<double> positions;
    std::vector<double> normals;    // empty, or one per position
    std::vector<double> uvs;        // empty, or two per position
    std::vector<uint32_t> triangles;
    std::vector<uint32_t> triangle_material;    // empty, or one per triangle
    std::vector<MeshMaterial> materials;
};

// A colour as a file stores it (sRGB bytes) into the linear light a renderer works in.
float srgb_to_linear(double encoded) {
    encoded = encoded < 0.0 ? 0.0 : encoded > 1.0 ? 1.0 : encoded;
    return static_cast<float>(encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4));
}

// The bytes a data URI carries, if `uri` is one with base64 in it; else nothing.
std::vector<uint8_t> data_uri_bytes(const std::string& uri, std::string& type) {
    std::vector<uint8_t> out;
    if (uri.compare(0, 5, "data:") != 0) return out;
    const size_t comma = uri.find(',');
    const size_t base64 = uri.find(";base64");
    if (comma == std::string::npos || base64 == std::string::npos || base64 > comma) return out;
    type = uri.substr(5, base64 - 5);
    uint32_t bits = 0;
    int count = 0;
    for (size_t i = comma + 1; i < uri.size() && out.size() < kMaxImageBytes; ++i) {
        const char c = uri[i];
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+' || c == '-') value = 62;
        else if (c == '/' || c == '_') value = 63;
        else if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        else return std::vector<uint8_t>();
        bits = (bits << 6) | static_cast<uint32_t>(value);
        if (++count == 4) {
            out.push_back(static_cast<uint8_t>(bits >> 16));
            out.push_back(static_cast<uint8_t>(bits >> 8));
            out.push_back(static_cast<uint8_t>(bits));
            bits = 0;
            count = 0;
        }
    }
    if (count == 3) { out.push_back(static_cast<uint8_t>(bits >> 10)); out.push_back(static_cast<uint8_t>(bits >> 2)); }
    else if (count == 2) out.push_back(static_cast<uint8_t>(bits >> 4));
    return out;
}

std::string lower_ascii(std::string text) {
    for (char& c : text) c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    return text;
}

std::string base_name(const std::string& path) {
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? path : path.substr(cut + 1);
}

// The picture a material names, from an archive: under the name as written, under one of
// `folders`, or failing both as the one entry whose own name matches without regard to case.
void resolve_textures(const Archive& archive, Mesh& mesh, const std::vector<std::string>& folders) {
    std::vector<std::string> listed;
    for (MeshMaterial& material : mesh.materials) {
        if (material.texture_file.empty() || !material.image.empty()) continue;
        std::vector<uint8_t> bytes;
        bool found = false;
        for (const std::string& folder : folders) {
            if (archive.read(folder + material.texture_file, bytes)) { found = true; break; }
        }
        if (!found) {
            if (listed.empty()) listed = archive.entries();
            const std::string wanted = lower_ascii(base_name(material.texture_file));
            for (const std::string& entry : listed) {
                if (lower_ascii(base_name(entry)) == wanted && archive.read(entry, bytes)) { found = true; break; }
            }
        }
        if (!found || bytes.empty() || bytes.size() > kMaxImageBytes) continue;
        material.image = std::move(bytes);
        if (material.image_type.empty()) {
            const std::string ending = lower_ascii(material.texture_file.size() > 4 ? material.texture_file.substr(material.texture_file.size() - 4) : "");
            material.image_type = ending == ".png" ? "image/png" : ending == ".jpg" || ending == "jpeg" ? "image/jpeg" : ending == ".bmp" ? "image/bmp" : "";
        }
    }
}

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
float f32(const uint8_t* p) {
    const uint32_t bits = u32(p);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void cross(const double* a, const double* b, double* out) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize(double* v) {
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-20) || !std::isfinite(length)) return false;
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

void set_bounds(Mesh& mesh) {
    for (int k = 0; k < 3; ++k) mesh.min[k] = mesh.max[k] = mesh.positions.empty() ? 0.0f : mesh.positions[static_cast<size_t>(k)];
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
        for (size_t k = 0; k < 3; ++k) {
            if (mesh.positions[i + k] < mesh.min[k]) mesh.min[k] = mesh.positions[i + k];
            if (mesh.positions[i + k] > mesh.max[k]) mesh.max[k] = mesh.positions[i + k];
        }
    }
}

// Checks what a reader gathered, drops triangles that cannot be drawn, works out normals where
// the file gave none, and fills in the result.
void finish(Raw& raw, MeshResult& result, const char* where) {
    const size_t vertices = raw.positions.size() / 3;
    for (double value : raw.positions) {
        if (!std::isfinite(value)) {
            result.diagnostics.error(where, "a vertex position is not a number");
            return;
        }
    }
    const bool has_normals = raw.normals.size() == raw.positions.size() && !raw.normals.empty();
    const bool has_uvs = raw.uvs.size() == vertices * 2 && !raw.uvs.empty();
    for (double value : raw.uvs) {
        if (!std::isfinite(value)) {
            result.diagnostics.error(where, "a texture coordinate is not a number");
            return;
        }
    }
    const bool has_materials = raw.triangle_material.size() == raw.triangles.size() / 3 && !raw.triangle_material.empty();

    // Face normals, and with them the triangles worth keeping.
    std::vector<uint32_t> kept;
    std::vector<uint32_t> kept_material;
    std::vector<double> face_normals;
    kept.reserve(raw.triangles.size());
    size_t dropped = 0;
    for (size_t t = 0; t + 2 < raw.triangles.size(); t += 3) {
        const uint32_t a = raw.triangles[t], b = raw.triangles[t + 1], c = raw.triangles[t + 2];
        if (a >= vertices || b >= vertices || c >= vertices) {
            result.diagnostics.error(where, "a triangle names a vertex the file does not have");
            return;
        }
        const double* pa = &raw.positions[a * 3];
        const double* pb = &raw.positions[b * 3];
        const double* pc = &raw.positions[c * 3];
        const double ab[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
        const double ac[3] = {pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]};
        double normal[3];
        cross(ab, ac, normal);
        if (!normalize(normal)) {
            ++dropped;    // no area: two corners in one place, or all three in a line
            continue;
        }
        kept.insert(kept.end(), {a, b, c});
        if (has_materials) kept_material.push_back(raw.triangle_material[t / 3]);
        face_normals.insert(face_normals.end(), {normal[0], normal[1], normal[2]});
    }
    if (kept.empty()) {
        result.diagnostics.error(where, "no triangles");
        return;
    }
    if (dropped > 0) result.diagnostics.warn(where, std::to_string(dropped) + " triangles without area were left out");

    Mesh& mesh = result.mesh;
    mesh.materials = std::move(raw.materials);
    if (has_materials) {
        // Only if any triangle wears a material at all, and never one the file does not have.
        bool any = false;
        for (uint32_t& index : kept_material) {
            if (index != kNoMaterial && index >= mesh.materials.size()) index = kNoMaterial;
            any = any || index != kNoMaterial;
        }
        if (any) mesh.triangle_material = std::move(kept_material);
    }
    if (has_normals) {
        mesh.positions.assign(raw.positions.begin(), raw.positions.end());
        if (has_uvs) mesh.uvs.assign(raw.uvs.begin(), raw.uvs.end());
        mesh.normals.resize(raw.normals.size());
        for (size_t v = 0; v < vertices; ++v) {
            double n[3] = {raw.normals[v * 3], raw.normals[v * 3 + 1], raw.normals[v * 3 + 2]};
            if (!normalize(n)) { n[0] = 0; n[1] = 0; n[2] = 1; }
            for (size_t k = 0; k < 3; ++k) mesh.normals[v * 3 + k] = static_cast<float>(n[k]);
        }
        mesh.triangles = std::move(kept);
    } else {
        // Which triangle corners meet at each vertex, and the angle each corner takes up there. A
        // face counts toward a vertex's normal by that angle, so that a flat side cut into two
        // triangles weighs the same from whichever of its corners it is seen.
        const size_t triangles = kept.size() / 3;
        std::vector<uint32_t> first(vertices + 1, 0);
        for (uint32_t index : kept) ++first[index + 1];
        for (size_t v = 0; v < vertices; ++v) first[v + 1] += first[v];
        std::vector<uint32_t> around(kept.size());
        std::vector<uint32_t> fill(first.begin(), first.end() - 1);
        std::vector<double> angle(kept.size());
        for (size_t t = 0; t < triangles; ++t) {
            for (size_t corner = 0; corner < 3; ++corner) {
                around[fill[kept[t * 3 + corner]]++] = static_cast<uint32_t>(t * 3 + corner);
                const double* here = &raw.positions[kept[t * 3 + corner] * 3];
                const double* next = &raw.positions[kept[t * 3 + (corner + 1) % 3] * 3];
                const double* last = &raw.positions[kept[t * 3 + (corner + 2) % 3] * 3];
                double to_next[3] = {next[0] - here[0], next[1] - here[1], next[2] - here[2]};
                double to_last[3] = {last[0] - here[0], last[1] - here[1], last[2] - here[2]};
                double cosine = normalize(to_next) && normalize(to_last) ? to_next[0] * to_last[0] + to_next[1] * to_last[1] + to_next[2] * to_last[2] : 1.0;
                cosine = cosine > 1.0 ? 1.0 : cosine < -1.0 ? -1.0 : cosine;
                angle[t * 3 + corner] = std::acos(cosine);
            }
        }
        // Every corner gets its own vertex, with the average of the faces around it that lie
        // within the crease angle of its own face: curved surfaces come out smooth, edges sharp.
        mesh.positions.reserve(kept.size() * 3);
        mesh.normals.reserve(kept.size() * 3);
        mesh.triangles.reserve(kept.size());
        for (size_t t = 0; t < triangles; ++t) {
            const double* own = &face_normals[t * 3];
            for (size_t corner = 0; corner < 3; ++corner) {
                const uint32_t v = kept[t * 3 + corner];
                double sum[3] = {0, 0, 0};
                const uint32_t count = first[v + 1] - first[v];
                // A vertex shared by a great many triangles is a pole; its own face is fair enough.
                for (uint32_t i = first[v]; i < first[v + 1] && count <= 256; ++i) {
                    const double* other = &face_normals[(around[i] / 3) * 3];
                    if (own[0] * other[0] + own[1] * other[1] + own[2] * other[2] >= kCreaseCos) {
                        sum[0] += other[0] * angle[around[i]];
                        sum[1] += other[1] * angle[around[i]];
                        sum[2] += other[2] * angle[around[i]];
                    }
                }
                if (!normalize(sum)) { sum[0] = own[0]; sum[1] = own[1]; sum[2] = own[2]; }
                for (size_t k = 0; k < 3; ++k) {
                    mesh.positions.push_back(static_cast<float>(raw.positions[v * 3 + k]));
                    mesh.normals.push_back(static_cast<float>(sum[k]));
                }
                if (has_uvs) {
                    mesh.uvs.push_back(static_cast<float>(raw.uvs[v * 2]));
                    mesh.uvs.push_back(static_cast<float>(raw.uvs[v * 2 + 1]));
                }
                mesh.triangles.push_back(static_cast<uint32_t>(mesh.triangles.size()));
            }
        }
    }
    set_bounds(mesh);
    for (size_t k = 0; k < 3; ++k) result.native_size[k] = static_cast<double>(mesh.max[k]) - static_cast<double>(mesh.min[k]);
    result.ok = true;
}

// ------------------------------------------------------------------------------------------ 3DS

// A 3DS file is chunks within chunks: a two-byte id, a four-byte length that counts the six
// header bytes, then the payload.
struct Chunk {
    uint16_t id = 0;
    size_t begin = 0;
    size_t end = 0;
};

bool next_chunk(const uint8_t* data, size_t limit, size_t& at, Chunk& chunk) {
    if (at + 6 > limit) return false;
    const uint32_t length = u32(data + at + 2);
    if (length < 6 || length > limit - at) return false;
    chunk.id = u16(data + at);
    chunk.begin = at + 6;
    chunk.end = at + length;
    at += length;
    return true;
}

// A string as 3DS stores one: bytes up to a NUL, within the chunk.
std::string chunk_string(const uint8_t* data, size_t at, size_t end) {
    std::string out;
    while (at < end && data[at] != 0 && out.size() < 256) out.push_back(static_cast<char>(data[at++]));
    return out;
}

// Which faces of an object wear which material, by the material's name: resolved once every
// material block has been read, since a file may put them after the objects.
struct FaceMaterial {
    std::string name;
    std::vector<uint32_t> triangles;    // numbered through the whole file
};

bool read_trimesh(const uint8_t* data, const Chunk& trimesh, Raw& raw, std::vector<FaceMaterial>& worn) {
    const size_t base = raw.positions.size() / 3;
    const size_t first_triangle = raw.triangles.size() / 3;
    size_t count_here = 0;
    size_t at = trimesh.begin;
    Chunk chunk;
    while (next_chunk(data, trimesh.end, at, chunk)) {
        if (chunk.id == 0x4110) {    // vertices
            if (chunk.end - chunk.begin < 2) return false;
            const size_t count = u16(data + chunk.begin);
            if (chunk.end - chunk.begin - 2 < count * 12 || base + count > kMaxVertices) return false;
            for (size_t i = 0; i < count * 3; ++i) raw.positions.push_back(static_cast<double>(f32(data + chunk.begin + 2 + i * 4)) * 0.001);
            count_here = count;
        } else if (chunk.id == 0x4140) {    // texture coordinates, one pair per vertex; 3DS counts v from the bottom
            if (chunk.end - chunk.begin < 2) return false;
            const size_t count = u16(data + chunk.begin);
            if (chunk.end - chunk.begin - 2 < count * 8) return false;
            if (raw.uvs.size() < base * 2) raw.uvs.resize(base * 2, 0.0);
            for (size_t i = 0; i < count; ++i) {
                raw.uvs.push_back(static_cast<double>(f32(data + chunk.begin + 2 + i * 8)));
                raw.uvs.push_back(1.0 - static_cast<double>(f32(data + chunk.begin + 6 + i * 8)));
            }
        } else if (chunk.id == 0x4120) {    // faces: three vertex numbers and a flags word each
            if (chunk.end - chunk.begin < 2) return false;
            const size_t count = u16(data + chunk.begin);
            if (chunk.end - chunk.begin - 2 < count * 8) return false;
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* face = data + chunk.begin + 2 + i * 8;
                raw.triangles.insert(raw.triangles.end(), {static_cast<uint32_t>(base + u16(face)), static_cast<uint32_t>(base + u16(face + 2)),
                                                           static_cast<uint32_t>(base + u16(face + 4))});
            }
            // What hangs inside the face list: which faces wear which material.
            size_t inside = chunk.begin + 2 + count * 8;
            Chunk sub;
            while (next_chunk(data, chunk.end, inside, sub)) {
                if (sub.id != 0x4130) continue;
                FaceMaterial entry;
                entry.name = chunk_string(data, sub.begin, sub.end);
                size_t list = sub.begin + entry.name.size() + 1;
                if (list + 2 > sub.end) continue;
                const size_t faces = u16(data + list);
                if (sub.end - list - 2 < faces * 2) continue;
                for (size_t i = 0; i < faces; ++i) {
                    const size_t face = u16(data + list + 2 + i * 2);
                    if (face < count) entry.triangles.push_back(static_cast<uint32_t>(first_triangle + face));
                }
                worn.push_back(std::move(entry));
            }
        }
    }
    // An object with no texture coordinates of its own keeps the lists aligned.
    if (!raw.uvs.empty() && raw.uvs.size() < (base + count_here) * 2) raw.uvs.resize((base + count_here) * 2, 0.0);
    return true;
}

// A material block: its name, its diffuse colour, its first texture map, and whether it is drawn from both sides.
void read_material(const uint8_t* data, const Chunk& block, Raw& raw) {
    if (raw.materials.size() >= kMaxMaterials) return;
    MeshMaterial material;
    size_t at = block.begin;
    Chunk chunk;
    bool coloured = false;
    while (next_chunk(data, block.end, at, chunk)) {
        if (chunk.id == 0xA000) {
            material.name = chunk_string(data, chunk.begin, chunk.end);
        } else if (chunk.id == 0xA020) {    // diffuse: as bytes (0x0011, or 0x0012 before gamma) or as floats (0x0010, 0x0013)
            size_t inside = chunk.begin;
            Chunk colour;
            while (next_chunk(data, chunk.end, inside, colour)) {
                if ((colour.id == 0x0011 || (colour.id == 0x0012 && !coloured)) && colour.end - colour.begin >= 3) {
                    for (int k = 0; k < 3; ++k) material.color[k] = srgb_to_linear(data[colour.begin + static_cast<size_t>(k)] / 255.0);
                    coloured = true;
                } else if ((colour.id == 0x0010 || colour.id == 0x0013) && !coloured && colour.end - colour.begin >= 12) {
                    for (int k = 0; k < 3; ++k) material.color[k] = srgb_to_linear(static_cast<double>(f32(data + colour.begin + static_cast<size_t>(k) * 4)));
                    coloured = true;
                }
            }
        } else if (chunk.id == 0xA200) {    // texture map 1
            size_t inside = chunk.begin;
            Chunk map;
            while (next_chunk(data, chunk.end, inside, map)) {
                if (map.id == 0xA300) material.texture_file = chunk_string(data, map.begin, map.end);
            }
        } else if (chunk.id == 0xA081) {
            material.double_sided = true;
        }
    }
    raw.materials.push_back(std::move(material));
}

// ------------------------------------------------------------------------------------------ GLB

struct Matrix {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};    // column-major, as glTF writes it
};

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix out;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            double sum = 0;
            for (int k = 0; k < 4; ++k) sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            out.m[column * 4 + row] = sum;
        }
    }
    return out;
}

Matrix node_matrix(const json::Value& node) {
    Matrix out;
    const json::Value* matrix = node.get("matrix");
    if (matrix && matrix->size() == 16) {
        for (size_t k = 0; k < 16; ++k) out.m[k] = matrix->array[k].num(out.m[k]);
        return out;
    }
    double t[3] = {0, 0, 0}, r[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
    if (const json::Value* v = node.get("translation")) for (size_t k = 0; k < 3 && k < v->size(); ++k) t[k] = v->array[k].num(0);
    if (const json::Value* v = node.get("rotation")) for (size_t k = 0; k < 4 && k < v->size(); ++k) r[k] = v->array[k].num(k == 3 ? 1 : 0);
    if (const json::Value* v = node.get("scale")) for (size_t k = 0; k < 3 && k < v->size(); ++k) s[k] = v->array[k].num(1);
    const double x = r[0], y = r[1], z = r[2], w = r[3];
    const double rotation[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),     // column 0
                                2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),     // column 1
                                2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};    // column 2
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) out.m[column * 4 + row] = rotation[column * 3 + row] * s[column];
    }
    out.m[12] = t[0];
    out.m[13] = t[1];
    out.m[14] = t[2];
    return out;
}

struct Glb {
    const json::Value* root = nullptr;
    const uint8_t* bin = nullptr;
    size_t bin_size = 0;
    Raw raw;
    std::string problem;
    size_t visited = 0;

    // The numbers of an accessor, `components` to an element. False if it cannot be read as such.
    bool read(const json::Value* index, size_t components, std::vector<double>& out) {
        const json::Value* accessors = root->get("accessors");
        const json::Value* views = root->get("bufferViews");
        const long long number = index ? index->integer() : -1;
        if (!accessors || !views || number < 0 || static_cast<size_t>(number) >= accessors->size()) return false;
        const json::Value& accessor = accessors->array[static_cast<size_t>(number)];
        const json::Value* type = accessor.get("type");
        const size_t in_type = !type ? 0 : type->string == "SCALAR" ? 1 : type->string == "VEC2" ? 2 : type->string == "VEC3" ? 3 : type->string == "VEC4" ? 4 : 0;
        const json::Value* view_number = accessor.get("bufferView");
        const long long view_index = view_number ? view_number->integer() : -1;
        if (in_type != components || view_index < 0 || static_cast<size_t>(view_index) >= views->size()) return false;
        const json::Value& view = views->array[static_cast<size_t>(view_index)];
        if (const json::Value* buffer = view.get("buffer")) {
            if (buffer->integer() != 0) return false;    // a GLB carries one buffer
        }
        const long long component = accessor.get("componentType") ? accessor.get("componentType")->integer() : -1;
        const size_t width = component == 5120 || component == 5121 ? 1 : component == 5122 || component == 5123 ? 2 : component == 5125 || component == 5126 ? 4 : 0;
        const long long count = accessor.get("count") ? accessor.get("count")->integer() : -1;
        const long long view_offset = view.get("byteOffset") ? view.get("byteOffset")->integer(0) : 0;
        const long long view_length = view.get("byteLength") ? view.get("byteLength")->integer() : -1;
        const long long offset = accessor.get("byteOffset") ? accessor.get("byteOffset")->integer(0) : 0;
        long long stride = view.get("byteStride") ? view.get("byteStride")->integer(0) : 0;
        if (stride == 0) stride = static_cast<long long>(width * components);
        if (width == 0 || count < 0 || view_offset < 0 || view_length < 0 || offset < 0 || stride < static_cast<long long>(width * components) ||
            static_cast<unsigned long long>(count) > kMaxVertices * 3) {
            return false;
        }
        if (static_cast<unsigned long long>(view_offset) + static_cast<unsigned long long>(view_length) > bin_size) return false;
        if (count > 0 && static_cast<unsigned long long>(offset) + static_cast<unsigned long long>(stride) * static_cast<unsigned long long>(count - 1) +
                             width * components > static_cast<unsigned long long>(view_length)) {
            return false;
        }
        const bool normalized = accessor.get("normalized") && accessor.get("normalized")->boolean;
        out.clear();
        out.reserve(static_cast<size_t>(count) * components);
        for (long long i = 0; i < count; ++i) {
            const uint8_t* element = bin + view_offset + offset + stride * i;
            for (size_t k = 0; k < components; ++k) {
                const uint8_t* p = element + k * width;
                double value = 0;
                switch (component) {
                    case 5120: value = static_cast<int8_t>(p[0]); if (normalized) value = value / 127.0 < -1 ? -1 : value / 127.0; break;
                    case 5121: value = p[0]; if (normalized) value /= 255.0; break;
                    case 5122: value = static_cast<int16_t>(u16(p)); if (normalized) value = value / 32767.0 < -1 ? -1 : value / 32767.0; break;
                    case 5123: value = u16(p); if (normalized) value /= 65535.0; break;
                    case 5125: value = u32(p); break;
                    default: value = static_cast<double>(f32(p)); break;
                }
                out.push_back(value);
            }
        }
        return true;
    }

    // The materials of the file, in its order, each with its colour and its picture.
    void read_materials() {
        const json::Value* materials = root->get("materials");
        if (!materials) return;
        for (size_t i = 0; i < materials->size() && i < kMaxMaterials; ++i) {
            const json::Value& described = materials->array[i];
            MeshMaterial material;
            if (const json::Value* name = described.get("name")) material.name = name->string;
            material.double_sided = described.get("doubleSided") && described.get("doubleSided")->boolean;
            const json::Value* pbr = described.get("pbrMetallicRoughness");
            const json::Value* gloss = described.get("extensions") ? described.get("extensions")->get("KHR_materials_pbrSpecularGlossiness") : nullptr;
            const json::Value* factor = pbr ? pbr->get("baseColorFactor") : gloss ? gloss->get("diffuseFactor") : nullptr;
            if (factor && factor->size() == 4) {
                for (size_t k = 0; k < 4; ++k) {
                    const double value = factor->array[k].num(1.0);
                    material.color[k] = static_cast<float>(value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value);
                }
            }
            const json::Value* texture = pbr ? pbr->get("baseColorTexture") : gloss ? gloss->get("diffuseTexture") : nullptr;
            if (texture && texture->get("index")) read_image(texture->get("index")->integer(), material);
            raw.materials.push_back(std::move(material));
        }
    }

    // The picture of a texture: embedded in the file's buffer, as a data URI, or named as a file for later.
    void read_image(long long texture_index, MeshMaterial& material) {
        const json::Value* textures = root->get("textures");
        const json::Value* images = root->get("images");
        if (!textures || !images || texture_index < 0 || static_cast<size_t>(texture_index) >= textures->size()) return;
        const json::Value* source = textures->array[static_cast<size_t>(texture_index)].get("source");
        const long long image_index = source ? source->integer() : -1;
        if (image_index < 0 || static_cast<size_t>(image_index) >= images->size()) return;
        const json::Value& image = images->array[static_cast<size_t>(image_index)];
        if (const json::Value* mime = image.get("mimeType")) material.image_type = mime->string;
        if (const json::Value* view_number = image.get("bufferView")) {
            const json::Value* views = root->get("bufferViews");
            const long long view_index = view_number->integer();
            if (!views || view_index < 0 || static_cast<size_t>(view_index) >= views->size()) return;
            const json::Value& view = views->array[static_cast<size_t>(view_index)];
            const long long offset = view.get("byteOffset") ? view.get("byteOffset")->integer(0) : 0;
            const long long length = view.get("byteLength") ? view.get("byteLength")->integer() : -1;
            if (offset < 0 || length <= 0 || static_cast<unsigned long long>(length) > kMaxImageBytes ||
                static_cast<unsigned long long>(offset) + static_cast<unsigned long long>(length) > bin_size) {
                return;
            }
            material.image.assign(bin + offset, bin + offset + length);
        } else if (const json::Value* uri = image.get("uri")) {
            std::string type;
            material.image = data_uri_bytes(uri->string, type);
            if (!material.image.empty()) material.image_type = type;
            else material.texture_file = uri->string;    // a file beside the model, found later
        }
    }

    void add_primitive(const json::Value& primitive, const Matrix& world) {
        const long long mode = primitive.get("mode") ? primitive.get("mode")->integer(4) : 4;
        if (mode != 4 && mode != 5 && mode != 6) return;    // points and lines are not surfaces
        const json::Value* attributes = primitive.get("attributes");
        std::vector<double> positions, normals, indices, texcoords;
        if (!attributes || !read(attributes->get("POSITION"), 3, positions)) {
            problem = "a mesh's vertex positions cannot be read";
            return;
        }
        const size_t count = positions.size() / 3;
        const bool has_normals = attributes->get("NORMAL") && read(attributes->get("NORMAL"), 3, normals) && normals.size() == positions.size();
        const bool has_uvs = attributes->get("TEXCOORD_0") && read(attributes->get("TEXCOORD_0"), 2, texcoords) && texcoords.size() == count * 2;
        const long long material = primitive.get("material") ? primitive.get("material")->integer() : -1;
        if (primitive.get("indices")) {
            if (!read(primitive.get("indices"), 1, indices)) {
                problem = "a mesh's triangles cannot be read";
                return;
            }
        } else {
            for (size_t i = 0; i < count; ++i) indices.push_back(static_cast<double>(i));
        }
        const size_t base = raw.positions.size() / 3;
        if (base + count > kMaxVertices) {
            problem = "more vertices than this reads";
            return;
        }
        // Normals turn with the surface. A node that also stretches unevenly would need more care
        // than this, and fixture models do not do that.
        for (size_t i = 0; i < count; ++i) {
            const double* p = &positions[i * 3];
            const double x = world.m[0] * p[0] + world.m[4] * p[1] + world.m[8] * p[2] + world.m[12];
            const double y = world.m[1] * p[0] + world.m[5] * p[1] + world.m[9] * p[2] + world.m[13];
            const double z = world.m[2] * p[0] + world.m[6] * p[1] + world.m[10] * p[2] + world.m[14];
            raw.positions.insert(raw.positions.end(), {x, -z, y});    // glTF has Y up and Z toward the viewer
        }
        if (has_normals) {
            if (raw.normals.size() != base * 3) raw.normals.resize(base * 3, 0.0);
            for (size_t i = 0; i < count; ++i) {
                const double* n = &normals[i * 3];
                const double x = world.m[0] * n[0] + world.m[4] * n[1] + world.m[8] * n[2];
                const double y = world.m[1] * n[0] + world.m[5] * n[1] + world.m[9] * n[2];
                const double z = world.m[2] * n[0] + world.m[6] * n[1] + world.m[10] * n[2];
                raw.normals.insert(raw.normals.end(), {x, -z, y});
            }
        } else {
            all_have_normals = false;
        }
        // Texture coordinates for every vertex, zero where a primitive has none, so the lists stay aligned.
        raw.uvs.resize(base * 2, 0.0);
        if (has_uvs) {
            raw.uvs.insert(raw.uvs.end(), texcoords.begin(), texcoords.end());
            any_uvs = true;
        } else {
            raw.uvs.resize((base + count) * 2, 0.0);
        }
        const size_t first_triangle = raw.triangles.size() / 3;
        auto vertex = [&](size_t i) { return static_cast<uint32_t>(base + static_cast<size_t>(indices[i])); };
        for (double index : indices) {
            if (index < 0 || index >= static_cast<double>(count)) {
                problem = "a triangle names a vertex its mesh does not have";
                return;
            }
        }
        if (mode == 4) {
            for (size_t i = 0; i + 2 < indices.size(); i += 3) raw.triangles.insert(raw.triangles.end(), {vertex(i), vertex(i + 1), vertex(i + 2)});
        } else if (mode == 5) {    // strip: every other triangle is wound the other way
            for (size_t i = 0; i + 2 < indices.size(); ++i) {
                if (i % 2 == 0) raw.triangles.insert(raw.triangles.end(), {vertex(i), vertex(i + 1), vertex(i + 2)});
                else raw.triangles.insert(raw.triangles.end(), {vertex(i + 1), vertex(i), vertex(i + 2)});
            }
        } else {    // fan
            for (size_t i = 1; i + 1 < indices.size(); ++i) raw.triangles.insert(raw.triangles.end(), {vertex(0), vertex(i), vertex(i + 1)});
        }
        const uint32_t wearing = material >= 0 && static_cast<size_t>(material) < raw.materials.size() ? static_cast<uint32_t>(material) : kNoMaterial;
        raw.triangle_material.resize(raw.triangles.size() / 3, kNoMaterial);
        for (size_t i = first_triangle; i < raw.triangle_material.size(); ++i) raw.triangle_material[i] = wearing;
    }

    void walk(size_t node_index, const Matrix& parent, int depth) {
        const json::Value* nodes = root->get("nodes");
        if (!nodes || node_index >= nodes->size() || depth > 64 || ++visited > 100000 || !problem.empty()) return;
        const json::Value& node = nodes->array[node_index];
        const Matrix world = multiply(parent, node_matrix(node));
        const json::Value* meshes = root->get("meshes");
        const long long mesh = node.get("mesh") ? node.get("mesh")->integer() : -1;
        if (meshes && mesh >= 0 && static_cast<size_t>(mesh) < meshes->size()) {
            if (const json::Value* primitives = meshes->array[static_cast<size_t>(mesh)].get("primitives")) {
                for (const json::Value& primitive : primitives->array) add_primitive(primitive, world);
            }
        }
        if (const json::Value* children = node.get("children")) {
            for (const json::Value& child : children->array) {
                if (child.integer() >= 0) walk(static_cast<size_t>(child.integer()), world, depth + 1);
            }
        }
    }

    bool all_have_normals = true;
    bool any_uvs = false;
};

}  // namespace

MeshResult parse_3ds(const uint8_t* data, size_t size) {
    MeshResult result;
    const char* where = "3DS";
    size_t at = 0;
    Chunk main;
    if (!data || !next_chunk(data, size, at, main) || main.id != 0x4D4D) {
        result.diagnostics.error(where, "not a 3DS file");
        return result;
    }
    Raw raw;
    std::vector<FaceMaterial> worn;
    bool sound = true;
    size_t in_main = main.begin;
    Chunk editor;
    while (next_chunk(data, main.end, in_main, editor)) {
        if (editor.id != 0x3D3D) continue;
        size_t in_editor = editor.begin;
        Chunk object;
        while (next_chunk(data, editor.end, in_editor, object)) {
            if (object.id == 0xAFFF) {
                read_material(data, object, raw);
                continue;
            }
            if (object.id != 0x4000) continue;
            size_t in_object = object.begin;
            while (in_object < object.end && data[in_object] != 0) ++in_object;    // the object's name
            ++in_object;
            Chunk part;
            while (in_object < object.end && next_chunk(data, object.end, in_object, part)) {
                if (part.id == 0x4100) sound = read_trimesh(data, part, raw, worn) && sound;
            }
        }
    }
    if (!sound) {
        result.diagnostics.error(where, "a mesh in the file is cut short or too large");
        return result;
    }
    if (!worn.empty()) {
        raw.triangle_material.assign(raw.triangles.size() / 3, kNoMaterial);
        for (const FaceMaterial& entry : worn) {
            uint32_t index = kNoMaterial;
            for (size_t i = 0; i < raw.materials.size(); ++i) {
                if (raw.materials[i].name == entry.name) index = static_cast<uint32_t>(i);
            }
            if (index == kNoMaterial) {    // named but never described: plain white, so that the faces still count as one
                if (raw.materials.size() >= kMaxMaterials) continue;
                MeshMaterial plain;
                plain.name = entry.name;
                raw.materials.push_back(plain);
                index = static_cast<uint32_t>(raw.materials.size() - 1);
            }
            for (uint32_t triangle : entry.triangles) {
                if (triangle < raw.triangle_material.size()) raw.triangle_material[triangle] = index;
            }
        }
    }
    if (!raw.uvs.empty() && raw.uvs.size() != raw.positions.size() / 3 * 2) raw.uvs.clear();
    finish(raw, result, where);
    return result;
}

MeshResult parse_glb(const uint8_t* data, size_t size) {
    MeshResult result;
    const char* where = "GLB";
    if (!data || size < 20 || u32(data) != 0x46546C67 || u32(data + 4) != 2) {
        result.diagnostics.error(where, "not a binary glTF 2 file");
        return result;
    }
    const size_t total = u32(data + 8) < size ? u32(data + 8) : size;
    std::string_view text;
    Glb glb;
    for (size_t at = 12; at + 8 <= total;) {
        const size_t length = u32(data + at);
        const uint32_t type = u32(data + at + 4);
        if (length > total - at - 8) break;
        if (type == 0x4E4F534A && text.empty()) text = std::string_view(reinterpret_cast<const char*>(data + at + 8), length);
        if (type == 0x004E4942 && !glb.bin) {
            glb.bin = data + at + 8;
            glb.bin_size = length;
        }
        at += 8 + length + (4 - length % 4) % 4;
    }
    json::Value root;
    if (text.empty() || !json::parse(text, root)) {
        result.diagnostics.error(where, "its scene description cannot be read");
        return result;
    }
    for (const char* list : {"extensionsRequired", "extensionsUsed"}) {
        if (const json::Value* extensions = root.get(list)) {
            for (const json::Value& name : extensions->array) {
                if (name.string == "KHR_draco_mesh_compression" || name.string == "EXT_meshopt_compression") {
                    result.diagnostics.error(where, "its meshes are compressed (" + name.string + "), which is not read");
                    return result;
                }
            }
        }
    }
    if (!glb.bin) {
        result.diagnostics.error(where, "it carries no mesh data");
        return result;
    }
    glb.root = &root;
    glb.read_materials();

    // The nodes of the scene the file names, or of its first; failing that, every node nothing points to.
    const json::Value* nodes = root.get("nodes");
    std::vector<size_t> roots;
    const json::Value* scenes = root.get("scenes");
    const long long chosen = root.get("scene") ? root.get("scene")->integer(0) : 0;
    if (scenes && chosen >= 0 && static_cast<size_t>(chosen) < scenes->size()) {
        if (const json::Value* list = scenes->array[static_cast<size_t>(chosen)].get("nodes")) {
            for (const json::Value& node : list->array) {
                if (node.integer() >= 0) roots.push_back(static_cast<size_t>(node.integer()));
            }
        }
    } else if (nodes) {
        std::vector<bool> is_child(nodes->size(), false);
        for (const json::Value& node : nodes->array) {
            if (const json::Value* children = node.get("children")) {
                for (const json::Value& child : children->array) {
                    if (child.integer() >= 0 && static_cast<size_t>(child.integer()) < is_child.size()) is_child[static_cast<size_t>(child.integer())] = true;
                }
            }
        }
        for (size_t i = 0; i < is_child.size(); ++i) {
            if (!is_child[i]) roots.push_back(i);
        }
    }
    for (size_t node : roots) glb.walk(node, Matrix(), 0);
    if (!glb.problem.empty()) {
        result.diagnostics.error(where, glb.problem);
        return result;
    }
    // Normals are used only if every part of the model brought its own.
    if (!glb.all_have_normals || glb.raw.normals.size() != glb.raw.positions.size()) glb.raw.normals.clear();
    if (!glb.any_uvs) glb.raw.uvs.clear();
    finish(glb.raw, result, where);
    return result;
}

void fit_mesh(Mesh& mesh, double length, double width, double height) {
    const double wanted[3] = {length, width, height};
    double scale[3] = {1, 1, 1};
    bool changed = false;
    for (size_t k = 0; k < 3; ++k) {
        const double extent = static_cast<double>(mesh.max[k]) - static_cast<double>(mesh.min[k]);
        if (wanted[k] > 0 && extent > 1e-9) {
            scale[k] = wanted[k] / extent;
            changed = changed || std::fabs(scale[k] - 1.0) > 1e-9;
        }
    }
    if (!changed) return;
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
        double n[3];
        for (size_t k = 0; k < 3; ++k) {
            mesh.positions[i + k] = static_cast<float>(static_cast<double>(mesh.positions[i + k]) * scale[k]);
            n[k] = static_cast<double>(mesh.normals[i + k]) / scale[k];    // a surface's normal stretches the opposite way
        }
        if (normalize(n)) {
            for (size_t k = 0; k < 3; ++k) mesh.normals[i + k] = static_cast<float>(n[k]);
        }
    }
    set_bounds(mesh);
}

MeshResult load_model_mesh(const Archive& gdtf, const Model& model) {
    MeshResult result;
    if (model.file.empty()) return result;
    std::vector<uint8_t> bytes;
    if (gdtf.read("models/gltf/" + model.file + ".glb", bytes)) {
        result = parse_glb(bytes.data(), bytes.size());
    } else if (gdtf.read("models/3ds/" + model.file + ".3ds", bytes)) {
        result = parse_3ds(bytes.data(), bytes.size());
    } else {
        result.diagnostics.warn("Model '" + model.name + "'", "names the file '" + model.file + "', which is not in the archive as GLB or 3DS");
        return result;
    }
    if (result.ok) {
        fit_mesh(result.mesh, model.length, model.width, model.height);
        resolve_textures(gdtf, result.mesh, {"models/gltf/", "models/3ds/", "models/", ""});
    }
    return result;
}

Bounds fixture_bounds(const FixtureType& fixture, std::string_view root_geometry, const Archive* gdtf) {
    Bounds bounds;
    Diagnostics unused;
    const std::vector<GeometryInstance> tree = expand_geometry(fixture, root_geometry, unused);
    std::vector<Matrix4> placed(tree.size());
    std::map<std::string, Bounds> measured;    // by model name: a batten has one model for fifty cells

    for (size_t index = 0; index < tree.size(); ++index) {
        const GeometryInstance& instance = tree[index];
        placed[index] = instance.parent >= 0 ? multiply(placed[static_cast<size_t>(instance.parent)], instance.local) : instance.local;

        const Model* model = nullptr;
        for (const Model& candidate : fixture.models) {
            if (candidate.name == instance.geometry->model) model = &candidate;
        }
        if (!model) continue;

        const bool turns = instance.geometry->type == GeometryType::Axis;
        const std::string key = model->name + (turns ? "|centred" : "");
        auto found = measured.find(key);
        if (found == measured.end()) {
            Bounds part;
            if (gdtf && !model->file.empty()) {
                const MeshResult shape = load_model_mesh(*gdtf, *model);
                if (shape.ok && shape.mesh.vertex_count() > 0) {
                    part.valid = true;
                    for (int axis = 0; axis < 3; ++axis) {
                        part.min[axis] = static_cast<double>(shape.mesh.min[axis]);
                        part.max[axis] = static_cast<double>(shape.mesh.max[axis]);
                    }
                }
            }
            if (!part.valid && model->length > 0.0 && model->width > 0.0 && model->height > 0.0) {
                const bool centred = turns || model->primitive_type == "Cube" || model->primitive_type == "Cylinder" || model->primitive_type == "Sphere";
                part.valid = true;
                part.min[0] = -model->length / 2.0;
                part.max[0] = model->length / 2.0;
                part.min[1] = -model->width / 2.0;
                part.max[1] = model->width / 2.0;
                part.min[2] = centred ? -model->height / 2.0 : -model->height;
                part.max[2] = centred ? model->height / 2.0 : 0.0;
            }
            found = measured.emplace(key, part).first;
        }
        const Bounds& part = found->second;
        if (!part.valid) continue;

        const Matrix4& at = placed[index];
        for (int corner = 0; corner < 8; ++corner) {
            const double local[3] = {corner & 1 ? part.max[0] : part.min[0], corner & 2 ? part.max[1] : part.min[1], corner & 4 ? part.max[2] : part.min[2]};
            for (int axis = 0; axis < 3; ++axis) {
                const double value = at.m[axis][0] * local[0] + at.m[axis][1] * local[1] + at.m[axis][2] * local[2] + at.m[axis][3];
                if (!std::isfinite(value)) continue;
                if (!bounds.valid || value < bounds.min[axis]) bounds.min[axis] = value;
                if (!bounds.valid || value > bounds.max[axis]) bounds.max[axis] = value;
            }
            bounds.valid = true;
        }
    }
    return bounds;
}

MeshResult load_scene_mesh(const Archive& mvr, const std::string& file_name) {
    MeshResult result;
    std::vector<uint8_t> bytes;
    std::string found = file_name;
    if (!mvr.read(found, bytes)) {
        // A name without its ending: seen from writers that leave it off.
        if (mvr.read(file_name + ".glb", bytes)) {
            found = file_name + ".glb";
        } else if (mvr.read(file_name + ".3ds", bytes)) {
            found = file_name + ".3ds";
        } else {
            result.diagnostics.warn("Geometry3D '" + file_name + "'", "is not in the archive");
            return result;
        }
    }
    const bool is_3ds = found.size() >= 4 && text::iequals(std::string_view(found).substr(found.size() - 4), ".3ds");
    result = is_3ds ? parse_3ds(bytes.data(), bytes.size()) : parse_glb(bytes.data(), bytes.size());
    for (Diagnostic& item : result.diagnostics.items) item.where = found;
    if (result.ok) {
        // Pictures the model names as files: beside the model, or at the archive's root, as an MVR keeps them.
        const size_t cut = found.find_last_of('/');
        resolve_textures(mvr, result.mesh, {cut == std::string::npos ? std::string() : found.substr(0, cut + 1), ""});
    }
    return result;
}

namespace {

// A number for the glTF's JSON: '.' whatever locale the host has set, and nothing JSON cannot read.
std::string json_number(double value) {
    if (!std::isfinite(value)) value = 0.0;
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    for (char* c = buffer; *c; ++c) {
        if (*c == ',') *c = '.';    // a host application may have set a comma locale
    }
    return buffer;
}

std::string json_string(std::string_view value) {
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
            out += escaped;
        } else {
            out += c;
        }
    }
    return out + "\"";
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int k = 0; k < 4; ++k) out.push_back(static_cast<uint8_t>(value >> (8 * k)));
}

}  // namespace

std::vector<uint8_t> write_glb(const Mesh& mesh, std::string_view name) {
    const size_t count = mesh.vertex_count();
    const bool normals = mesh.normals.size() == mesh.positions.size() && count > 0;
    const bool uvs = mesh.uvs.size() == count * 2 && count > 0;

    // The binary part: each piece four-byte aligned, as glTF wants.
    std::vector<uint8_t> bin;
    struct View {
        size_t offset, length;
        int target;    // 34962 vertex data, 34963 indices, 0 neither
    };
    std::vector<View> views;
    auto add_view = [&](const void* data, size_t length, int target) {
        while (bin.size() % 4) bin.push_back(0);
        views.push_back({bin.size(), length, target});
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        bin.insert(bin.end(), bytes, bytes + length);
        return views.size() - 1;
    };
    std::string accessors;
    size_t accessor_count = 0;
    auto add_accessor = [&](size_t view, int component, size_t items, const char* type, const std::string& bounds) {
        accessors += std::string(accessor_count ? "," : "") + "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(component) +
                     ",\"count\":" + std::to_string(items) + ",\"type\":\"" + type + "\"" + bounds + "}";
        return accessor_count++;
    };

    // One primitive for the triangles of each material, and one for those that wear none, each
    // with only the vertices it uses (a reader takes a primitive's vertex list whole).
    const bool worn = mesh.triangle_material.size() == mesh.triangle_count();
    auto material_of = [&](size_t t) {
        const uint32_t material = worn ? mesh.triangle_material[t] : kNoMaterial;
        return material != kNoMaterial && material >= mesh.materials.size() ? kNoMaterial : material;
    };
    std::vector<uint32_t> groups;    // material numbers in the order first met
    for (size_t t = 0; t < mesh.triangle_count(); ++t) {
        if (std::find(groups.begin(), groups.end(), material_of(t)) == groups.end()) groups.push_back(material_of(t));
    }
    std::string primitives;
    std::vector<uint32_t> renumbered(count, kNoMaterial);
    for (const uint32_t group : groups) {
        std::vector<uint32_t> used, indices;
        for (size_t t = 0; t < mesh.triangle_count(); ++t) {
            if (material_of(t) != group) continue;
            for (int corner = 0; corner < 3; ++corner) {
                const uint32_t vertex = mesh.triangles[t * 3 + static_cast<size_t>(corner)];
                if (vertex >= count) continue;
                if (renumbered[vertex] == kNoMaterial) {
                    renumbered[vertex] = static_cast<uint32_t>(used.size());
                    used.push_back(vertex);
                }
                indices.push_back(renumbered[vertex]);
            }
        }
        for (const uint32_t vertex : used) renumbered[vertex] = kNoMaterial;    // ready for the next group
        if (indices.size() < 3) continue;
        indices.resize(indices.size() - indices.size() % 3);
        // glTF has Y up and Z toward whoever looks from the front: the mesh's (x, y, z) is its (x, z, -y).
        std::vector<float> positions, turned_normals, texcoords;
        float low[3] = {0, 0, 0}, high[3] = {0, 0, 0};
        for (size_t i = 0; i < used.size(); ++i) {
            const size_t v = used[i];
            const float p[3] = {mesh.positions[v * 3], mesh.positions[v * 3 + 2], -mesh.positions[v * 3 + 1]};
            for (int k = 0; k < 3; ++k) {
                positions.push_back(p[k]);
                low[k] = i == 0 ? p[k] : std::min(low[k], p[k]);
                high[k] = i == 0 ? p[k] : std::max(high[k], p[k]);
            }
            if (normals) turned_normals.insert(turned_normals.end(), {mesh.normals[v * 3], mesh.normals[v * 3 + 2], -mesh.normals[v * 3 + 1]});
            if (uvs) texcoords.insert(texcoords.end(), {mesh.uvs[v * 2], mesh.uvs[v * 2 + 1]});
        }
        const std::string bounds = ",\"min\":[" + json_number(low[0]) + "," + json_number(low[1]) + "," + json_number(low[2]) + "],\"max\":[" +
                                   json_number(high[0]) + "," + json_number(high[1]) + "," + json_number(high[2]) + "]";
        std::string attributes = "\"POSITION\":" + std::to_string(add_accessor(add_view(positions.data(), positions.size() * sizeof(float), 34962), 5126,
                                                                                used.size(), "VEC3", bounds));
        if (normals) {
            attributes += ",\"NORMAL\":" + std::to_string(add_accessor(add_view(turned_normals.data(), turned_normals.size() * sizeof(float), 34962), 5126,
                                                                        used.size(), "VEC3", ""));
        }
        if (uvs) {
            attributes += ",\"TEXCOORD_0\":" + std::to_string(add_accessor(add_view(texcoords.data(), texcoords.size() * sizeof(float), 34962), 5126,
                                                                            used.size(), "VEC2", ""));
        }
        const size_t accessor = add_accessor(add_view(indices.data(), indices.size() * sizeof(uint32_t), 34963), 5125, indices.size(), "SCALAR", "");
        primitives += std::string(primitives.empty() ? "" : ",") + "{\"attributes\":{" + attributes + "},\"indices\":" + std::to_string(accessor) +
                      (group == kNoMaterial ? std::string() : ",\"material\":" + std::to_string(group)) + "}";
    }

    // Materials, with their pictures packed in.
    std::string materials, images, textures;
    size_t image_count = 0;
    for (size_t m = 0; m < mesh.materials.size(); ++m) {
        const MeshMaterial& material = mesh.materials[m];
        std::string pbr = "\"baseColorFactor\":[" + json_number(material.color[0]) + "," + json_number(material.color[1]) + "," + json_number(material.color[2]) +
                          "," + json_number(material.color[3]) + "],\"metallicFactor\":0,\"roughnessFactor\":0.8";
        if (material.textured() && uvs) {
            const size_t view = add_view(material.image.data(), material.image.size(), 0);
            const std::string type = material.image_type.empty() ? "image/png" : material.image_type;
            images += std::string(image_count ? "," : "") + "{\"bufferView\":" + std::to_string(view) + ",\"mimeType\":" + json_string(type) + "}";
            textures += std::string(image_count ? "," : "") + "{\"source\":" + std::to_string(image_count) + "}";
            pbr += ",\"baseColorTexture\":{\"index\":" + std::to_string(image_count) + "}";
            ++image_count;
        }
        materials += std::string(m ? "," : "") + "{\"name\":" + json_string(material.name) + ",\"pbrMetallicRoughness\":{" + pbr + "}" +
                     (material.double_sided ? ",\"doubleSided\":true" : "") + "}";
    }
    while (bin.size() % 4) bin.push_back(0);

    std::string views_text;
    for (size_t v = 0; v < views.size(); ++v) {
        views_text += std::string(v ? "," : "") + "{\"buffer\":0,\"byteOffset\":" + std::to_string(views[v].offset) + ",\"byteLength\":" +
                      std::to_string(views[v].length) + (views[v].target ? ",\"target\":" + std::to_string(views[v].target) : std::string()) + "}";
    }
    std::string json = "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Openviz\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":" +
                       json_string(name) + (primitives.empty() ? std::string() : ",\"mesh\":0") + "}]";
    if (!primitives.empty()) json += ",\"meshes\":[{\"primitives\":[" + primitives + "]}]";
    if (!materials.empty()) json += ",\"materials\":[" + materials + "]";
    if (image_count) json += ",\"images\":[" + images + "],\"textures\":[" + textures + "]";
    if (!accessors.empty()) json += ",\"accessors\":[" + accessors + "]";
    if (!views_text.empty()) json += ",\"bufferViews\":[" + views_text + "]";
    if (!bin.empty()) json += ",\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) + "}]";
    json += "}";
    while (json.size() % 4) json += ' ';

    std::vector<uint8_t> out;
    const size_t total = 12 + 8 + json.size() + (bin.empty() ? 0 : 8 + bin.size());
    out.reserve(total);
    put_u32(out, 0x46546C67);    // "glTF"
    put_u32(out, 2);
    put_u32(out, static_cast<uint32_t>(total));
    put_u32(out, static_cast<uint32_t>(json.size()));
    put_u32(out, 0x4E4F534A);    // "JSON"
    out.insert(out.end(), json.begin(), json.end());
    if (!bin.empty()) {
        put_u32(out, static_cast<uint32_t>(bin.size()));
        put_u32(out, 0x004E4942);    // "BIN"
        out.insert(out.end(), bin.begin(), bin.end());
    }
    return out;
}

namespace {

std::vector<std::string_view> words_of(std::string_view line) {
    std::vector<std::string_view> words;
    size_t at = 0;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t' || line[at] == '\r')) ++at;
        const size_t start = at;
        while (at < line.size() && line[at] != ' ' && line[at] != '\t' && line[at] != '\r') ++at;
        if (at > start) words.push_back(line.substr(start, at - start));
    }
    return words;
}

double number_of(std::string_view word, bool& ok) {
    double value = 0.0;
    if (!text::parse_double(word, value)) ok = false;
    return value;
}

// The materials of an MTL file: each one's diffuse colour and picture file.
std::map<std::string, MeshMaterial> read_mtl(std::string_view mtl) {
    std::map<std::string, MeshMaterial> found;
    MeshMaterial* current = nullptr;
    size_t at = 0;
    while (at <= mtl.size()) {
        size_t end = mtl.find('\n', at);
        if (end == std::string_view::npos) end = mtl.size();
        const std::vector<std::string_view> words = words_of(mtl.substr(at, end - at));
        at = end + 1;
        if (words.empty() || words[0][0] == '#') continue;
        if (words[0] == "newmtl" && words.size() >= 2) {
            current = &found[std::string(words[1])];
            current->name = std::string(words[1]);
        } else if (current && words[0] == "Kd" && words.size() >= 4) {
            bool ok = true;
            const double rgb[3] = {number_of(words[1], ok), number_of(words[2], ok), number_of(words[3], ok)};
            if (ok) {
                for (int k = 0; k < 3; ++k) current->color[k] = srgb_to_linear(rgb[k]);
            }
        } else if (current && words[0] == "map_Kd" && words.size() >= 2) {
            current->texture_file = std::string(words.back());    // options such as -s come before the file's name
        }
    }
    return found;
}

std::string read_whole(const std::string& path, size_t most, bool& ok) {
    std::ifstream file = read_stream(path);
    ok = static_cast<bool>(file);
    if (!ok) return std::string();
    std::string out((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (out.size() > most) {
        ok = false;
        return std::string();
    }
    return out;
}

}  // namespace

MeshResult parse_obj(std::string_view obj, std::string_view mtl) {
    MeshResult result;
    const char* where = "OBJ";
    std::vector<double> positions, normals, uvs;
    Raw raw;
    std::map<std::string, MeshMaterial> materials = read_mtl(mtl);
    std::map<std::string, uint32_t> worn;    // material name -> its number in raw.materials
    uint32_t current = kNoMaterial;
    bool any_material = false;
    bool every_normal = true;
    std::unordered_map<std::string, uint32_t> corners;    // "v/vt/vn" -> vertex
    size_t unread = 0;
    size_t at = 0;
    while (at <= obj.size()) {
        size_t end = obj.find('\n', at);
        if (end == std::string_view::npos) end = obj.size();
        const std::vector<std::string_view> words = words_of(obj.substr(at, end - at));
        at = end + 1;
        if (words.empty() || words[0][0] == '#') continue;
        bool ok = true;
        if (words[0] == "v" && words.size() >= 4) {
            for (int k = 1; k <= 3; ++k) positions.push_back(number_of(words[static_cast<size_t>(k)], ok));
        } else if (words[0] == "vn" && words.size() >= 4) {
            for (int k = 1; k <= 3; ++k) normals.push_back(number_of(words[static_cast<size_t>(k)], ok));
        } else if (words[0] == "vt" && words.size() >= 3) {
            uvs.push_back(number_of(words[1], ok));
            uvs.push_back(1.0 - number_of(words[2], ok));    // OBJ counts up from the bottom; the core from the top
        } else if (words[0] == "usemtl" && words.size() >= 2) {
            const std::string name(words[1]);
            auto known = worn.find(name);
            if (known == worn.end()) {
                MeshMaterial material = materials.count(name) ? materials[name] : MeshMaterial();
                material.name = name;
                raw.materials.push_back(material);
                known = worn.emplace(name, static_cast<uint32_t>(raw.materials.size() - 1)).first;
            }
            current = known->second;
            any_material = true;
        } else if (words[0] == "f" && words.size() >= 4) {
            std::vector<uint32_t> polygon;
            for (size_t w = 1; w < words.size(); ++w) {
                // v, v/vt, v//vn or v/vt/vn, counted from 1, or from the end if negative.
                long long index[3] = {0, 0, 0};
                size_t part = 0, start = 0;
                const std::string_view corner = words[w];
                for (size_t c = 0; c <= corner.size() && part < 3; ++c) {
                    if (c == corner.size() || corner[c] == '/') {
                        if (c > start && !text::parse_int(corner.substr(start, c - start), index[part])) ok = false;
                        ++part;
                        start = c + 1;
                    }
                }
                const long long counts[3] = {static_cast<long long>(positions.size() / 3), static_cast<long long>(uvs.size() / 2),
                                             static_cast<long long>(normals.size() / 3)};
                for (int k = 0; k < 3; ++k) {
                    if (index[k] < 0) index[k] += counts[k] + 1;
                    if (index[k] < 0 || index[k] > counts[k]) ok = false;
                }
                if (!ok || index[0] == 0) {
                    ok = false;
                    break;
                }
                every_normal = every_normal && index[2] > 0;
                const std::string key = std::to_string(index[0]) + "/" + std::to_string(index[1]) + "/" + std::to_string(index[2]);
                auto found = corners.find(key);
                if (found == corners.end()) {
                    const size_t v = static_cast<size_t>(index[0] - 1);
                    raw.positions.insert(raw.positions.end(), {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]});
                    if (index[2] > 0) {
                        const size_t n = static_cast<size_t>(index[2] - 1);
                        raw.normals.insert(raw.normals.end(), {normals[n * 3], normals[n * 3 + 1], normals[n * 3 + 2]});
                    } else {
                        raw.normals.insert(raw.normals.end(), {0.0, 0.0, 0.0});
                    }
                    if (index[1] > 0) {
                        const size_t u = static_cast<size_t>(index[1] - 1);
                        raw.uvs.insert(raw.uvs.end(), {uvs[u * 2], uvs[u * 2 + 1]});
                    } else {
                        raw.uvs.insert(raw.uvs.end(), {0.0, 0.0});
                    }
                    found = corners.emplace(key, static_cast<uint32_t>(raw.positions.size() / 3 - 1)).first;
                    if (raw.positions.size() / 3 > kMaxVertices) {
                        result.diagnostics.error(where, "more vertices than this reads");
                        return result;
                    }
                }
                polygon.push_back(found->second);
            }
            if (ok) {
                for (size_t k = 1; k + 1 < polygon.size(); ++k) {    // a fan: OBJ's polygons are taken to be flat and convex
                    raw.triangles.insert(raw.triangles.end(), {polygon[0], polygon[k], polygon[k + 1]});
                    raw.triangle_material.push_back(current);
                }
            }
        }
        if (!ok) ++unread;
    }
    if (unread) result.diagnostics.warn(where, std::to_string(unread) + " line(s) that cannot be read were passed over");
    if (raw.triangles.empty()) {
        result.diagnostics.error(where, "it has no faces");
        return result;
    }
    if (!every_normal) raw.normals.clear();    // worked out from the faces instead
    if (uvs.empty()) raw.uvs.clear();
    if (!any_material) raw.triangle_material.clear();
    finish(raw, result, where);
    return result;
}

MeshResult load_model_file(const std::string& path, std::string_view units, std::string_view up) {
    MeshResult result;
    const std::string lower = lower_ascii(path);
    auto ends = [&lower](const char* ending) {
        const size_t length = std::strlen(ending);
        return lower.size() >= length && lower.compare(lower.size() - length, length, ending) == 0;
    };
    const bool glb = ends(".glb"), three_ds = ends(".3ds"), obj = ends(".obj");
    if (!glb && !three_ds && !obj) {
        result.diagnostics.error(path, "is not a model file this reads (GLB, 3DS or OBJ)");
        return result;
    }
    bool ok = false;
    const std::string bytes = read_whole(path, size_t(1) << 30, ok);
    if (!ok) {
        result.diagnostics.error(path, "cannot be read");
        return result;
    }
    const size_t slash = path.find_last_of("/\\");
    const std::string folder = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    // What the file's numbers are taken to be by its reader, and which way up.
    double assumed = 1.0;    // metres
    bool z_up = true;
    if (glb) {
        result = parse_glb(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    } else if (three_ds) {
        result = parse_3ds(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
        assumed = 0.001;
    } else {
        std::string mtl;
        const size_t library = bytes.find("mtllib ");
        if (library != std::string::npos) {
            size_t end = bytes.find_first_of("\r\n", library);
            if (end == std::string::npos) end = bytes.size();
            std::string name(text::trim(std::string_view(bytes).substr(library + 7, end - library - 7)));
            bool found = false;
            mtl = read_whole(folder + name, size_t(16) << 20, found);
        }
        result = parse_obj(bytes, mtl);
        z_up = false;    // as most programs write OBJ
    }
    if (!result.ok) return result;
    // Pictures named as files: beside the model, by name, then by file name alone without regard to case.
    for (MeshMaterial& material : result.mesh.materials) {
        if (material.texture_file.empty() || !material.image.empty()) continue;
        bool found = false;
        std::string picture = read_whole(folder + material.texture_file, kMaxImageBytes, found);
        if (!found) picture = read_whole(folder + base_name(material.texture_file), kMaxImageBytes, found);
        if (!found || picture.empty()) continue;
        material.image.assign(picture.begin(), picture.end());
        const std::string ending = lower_ascii(material.texture_file.size() > 4 ? material.texture_file.substr(material.texture_file.size() - 4) : "");
        material.image_type = ending == ".png" ? "image/png" : ending == ".jpg" || ending == "jpeg" ? "image/jpeg" : ending == ".bmp" ? "image/bmp" : "";
    }
    // Units: what the person says the file's numbers are, against what its reader took them for.
    double scale = 1.0;
    if (!units.empty()) {
        const double metres = units == "mm" ? 0.001 : units == "cm" ? 0.01 : units == "m" ? 1.0 : units == "in" ? 0.0254 : units == "ft" ? 0.3048 : 0.0;
        if (metres <= 0.0) {
            result.ok = false;
            result.diagnostics.error(path, "units are mm, cm, m, in or ft");
            return result;
        }
        scale = metres / assumed;
    }
    // Up: the reader has the file Z up already for GLB (turned from glTF's Y) and 3DS; OBJ is read as it is.
    const bool want_y_up = up == "y" ? true : up == "z" ? false : !z_up;
    Mesh& mesh = result.mesh;
    for (size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
        float* p = &mesh.positions[i];
        float* n = i < mesh.normals.size() ? &mesh.normals[i] : nullptr;
        // GLB was turned from Y up as glTF has it; told it is Z up, it is turned back. 3DS and OBJ are as written; told Y up, they are turned.
        const bool turn_to_z = (obj || three_ds) && want_y_up;
        const bool turn_back = glb && up == "z";
        for (float* v : {p, n}) {
            if (!v) continue;
            if (turn_to_z) {
                const float y = v[1], z = v[2];
                v[1] = -z;
                v[2] = y;
            } else if (turn_back) {
                const float y = v[1], z = v[2];
                v[1] = z;
                v[2] = -y;
            }
        }
        for (int k = 0; k < 3; ++k) p[k] = static_cast<float>(static_cast<double>(p[k]) * scale);
    }
    set_bounds(mesh);
    for (int k = 0; k < 3; ++k) result.native_size[k] = static_cast<double>(mesh.max[k]) - static_cast<double>(mesh.min[k]);
    return result;
}

}  // namespace viz
