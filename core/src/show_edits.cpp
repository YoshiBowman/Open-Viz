#include "vizcore/show_edits.h"

#include <cstdio>
#include <random>

#include "pugixml.hpp"
#include "text.h"

namespace viz {

namespace {

std::string number(double value) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    for (char* c = buffer; *c; ++c) {
        if (*c == ',') *c = '.';    // a host application may have set a comma locale
    }
    return buffer;
}

std::string escaped(std::string_view text) {
    std::string out;
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default:
                // Control characters cannot be written in XML 1.0, escaped or not.
                out += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
        }
    }
    return out;
}

std::string matrix_text(const Transform& transform) {
    std::string out;
    for (const double* row : {transform.u, transform.v, transform.w, transform.o}) {
        out += "{" + number(row[0]) + "," + number(row[1]) + "," + number(row[2]) + "}";
    }
    return out;
}

bool parse_matrix(const char* written, Transform& out) {
    double values[12];
    if (!text::parse_braced_rows(written, 4, 3, values)) return false;
    for (int k = 0; k < 3; ++k) {
        out.u[k] = values[k];
        out.v[k] = values[3 + k];
        out.w[k] = values[6 + k];
        out.o[k] = values[9 + k];
    }
    return true;
}

// A name that is one file in a folder and nothing else: no way up or out of it.
bool plain_file_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") return false;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    return true;
}

}  // namespace

const AddedFixture* ShowEdits::find_added(std::string_view uuid) const {
    for (const AddedFixture& entry : added) {
        if (text::iequals(entry.uuid, uuid)) return &entry;
    }
    return nullptr;
}

bool ShowEdits::is_removed(std::string_view uuid) const {
    for (const std::string& entry : removed) {
        if (text::iequals(entry, uuid)) return true;
    }
    return false;
}

std::string new_uuid() {
    static std::mt19937_64 engine{std::random_device{}()};
    static const char digits[] = "0123456789ABCDEF";
    uint64_t bits[2] = {engine(), engine()};
    unsigned char bytes[16];
    for (int i = 0; i < 16; ++i) bytes[i] = static_cast<unsigned char>(bits[i / 8] >> (8 * (i % 8)));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);    // version 4: random
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);    // the RFC 4122 variant
    std::string out;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out += '-';
        out += digits[bytes[i] >> 4];
        out += digits[bytes[i] & 0x0F];
    }
    return out;
}

std::vector<std::string> add_fixtures(ShowEdits& edits, const AddRequest& request) {
    std::vector<std::string> made;
    const int count = request.count < 1 ? 0 : (request.count > 512 ? 512 : request.count);
    const int footprint = request.footprint < 1 ? 1 : (request.footprint > 512 ? 512 : request.footprint);
    int universe = request.universe > 0 && request.universe <= 32767 ? request.universe : 0;
    int address = request.address >= 1 && request.address <= 512 ? request.address : 1;
    for (int i = 0; i < count; ++i) {
        AddedFixture fixture;
        fixture.uuid = new_uuid();
        fixture.name = (request.name.empty() ? std::string("Fixture") : request.name) + " " + std::to_string(i + 1);
        if (request.first_id > 0) fixture.fixture_id = std::to_string(request.first_id + i);
        fixture.gdtf = request.gdtf;
        fixture.mode = request.mode;
        // Rows of twelve, 75 cm apart, a metre and a half downstage of the origin and a metre up:
        // out in the open, where they are easy to see and to pick up. Each further row sits further down.
        const size_t place = edits.added.size();
        fixture.world.o[0] = (static_cast<double>(place % 12) - 5.5) * 750.0;
        fixture.world.o[1] = -1500.0 - static_cast<double>(place / 12) * 750.0;
        fixture.world.o[2] = 1000.0;
        if (universe > 0) {
            if (address + footprint - 1 > 512) {    // it would run off the end: the next universe
                address = 1;
                universe = universe < 32767 ? universe + 1 : 0;
            }
            fixture.universe = universe;
            fixture.address = universe > 0 ? address : 1;
            address += footprint;
        }
        made.push_back(fixture.uuid);
        edits.added.push_back(std::move(fixture));
    }
    return made;
}

bool remove_fixture(ShowEdits& edits, std::string_view uuid) {
    for (size_t i = 0; i < edits.added.size(); ++i) {
        if (text::iequals(edits.added[i].uuid, uuid)) {
            edits.added.erase(edits.added.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    if (uuid.empty() || edits.is_removed(uuid)) return false;
    for (size_t i = 0; i < edits.fixtures.size(); ++i) {
        if (text::iequals(edits.fixtures[i].uuid, uuid)) {
            edits.fixtures.erase(edits.fixtures.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    edits.removed.emplace_back(uuid);
    return true;
}

bool restore_fixture(ShowEdits& edits, std::string_view uuid) {
    for (size_t i = 0; i < edits.removed.size(); ++i) {
        if (text::iequals(edits.removed[i], uuid)) {
            edits.removed.erase(edits.removed.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

const FixtureEdit* ShowEdits::find(std::string_view uuid) const {
    for (const FixtureEdit& edit : fixtures) {
        if (text::iequals(edit.uuid, uuid)) return &edit;
    }
    return nullptr;
}

FixtureEdit& ShowEdits::edit_of(std::string_view uuid) {
    for (FixtureEdit& edit : fixtures) {
        if (text::iequals(edit.uuid, uuid)) return edit;
    }
    fixtures.emplace_back();
    fixtures.back().uuid = std::string(uuid);
    return fixtures.back();
}

AddedFixture* ShowEdits::added_of(std::string_view uuid) {
    for (AddedFixture& entry : added) {
        if (text::iequals(entry.uuid, uuid)) return &entry;
    }
    return nullptr;
}

ShowEditsResult parse_show_edits(std::string_view xml) {
    ShowEditsResult result;
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        result.diagnostics.error("ShowEdits", "not well-formed XML");
        return result;
    }
    pugi::xml_node root = document.child("ShowEdits");
    if (!root) {
        result.diagnostics.error("ShowEdits", "no <ShowEdits> element");
        return result;
    }
    result.ok = true;

    for (pugi::xml_node node : root.children("Fixture")) {
        FixtureEdit edit;
        edit.uuid = std::string(text::trim(node.attribute("UUID").value()));
        const std::string where = "Fixture " + edit.uuid;
        if (edit.uuid.empty()) {
            result.diagnostics.warn("Fixture", "no UUID; entry ignored");
            continue;
        }
        if (result.edits.find(edit.uuid)) {
            result.diagnostics.warn(where, "listed twice; the first entry is used");
            continue;
        }

        if (pugi::xml_attribute matrix = node.attribute("Matrix")) {
            double values[12];
            if (text::parse_braced_rows(matrix.value(), 4, 3, values)) {
                Transform transform;
                for (int k = 0; k < 3; ++k) {
                    transform.u[k] = values[k];
                    transform.v[k] = values[3 + k];
                    transform.w[k] = values[6 + k];
                    transform.o[k] = values[9 + k];
                }
                edit.world = transform;
            } else {
                result.diagnostics.warn(where, "Matrix is not four rows of three numbers; position not changed");
            }
        }

        pugi::xml_attribute universe = node.attribute("Universe");
        pugi::xml_attribute address = node.attribute("Address");
        if (universe || address) {
            long long u = 0;
            long long a = 0;
            if (universe && address && text::parse_int(universe.value(), u) && text::parse_int(address.value(), a) &&
                u >= 0 && u <= 32767 && a >= 1 && a <= 512) {
                edit.universe = static_cast<int>(u);
                edit.address = static_cast<int>(a);
            } else {
                result.diagnostics.warn(where, "Universe and Address must both be given, 0-32767 and 1-512; patch not changed");
            }
        }

        if (pugi::xml_attribute mode = node.attribute("Mode")) {
            if (*mode.value()) {
                edit.mode = std::string(mode.value());
            } else {
                result.diagnostics.warn(where, "Mode is empty; mode not changed");
            }
        }
        if (pugi::xml_attribute name = node.attribute("Name")) edit.name = std::string(name.value());
        if (pugi::xml_attribute id = node.attribute("FixtureID")) edit.fixture_id = std::string(text::trim(id.value()));
        if (pugi::xml_attribute type = node.attribute("GDTF")) {
            const std::string file(text::trim(type.value()));
            if (plain_file_name(file)) {
                edit.gdtf = file;
            } else {
                result.diagnostics.warn(where, "GDTF must be the name of one file in the fixture library; the type is left as the file has it");
            }
        }
        for (const char* which : {"InvertPan", "InvertTilt"}) {
            if (pugi::xml_attribute flag = node.attribute(which)) {
                bool value = false;
                if (text::parse_bool(flag.value(), value)) {
                    (which[6] == 'P' ? edit.invert_pan : edit.invert_tilt) = value;
                } else {
                    result.diagnostics.warn(where, std::string(which) + " is not true or false; left as the file has it");
                }
            }
        }

        if (!edit.empty()) result.edits.fixtures.push_back(std::move(edit));
    }

    for (pugi::xml_node node : root.children("Removed")) {
        const std::string uuid(text::trim(node.attribute("UUID").value()));
        if (uuid.empty()) {
            result.diagnostics.warn("Removed", "no UUID; entry ignored");
        } else if (!result.edits.is_removed(uuid)) {
            result.edits.removed.push_back(uuid);
        }
    }

    for (pugi::xml_node node : root.children("Added")) {
        AddedFixture fixture;
        fixture.uuid = std::string(text::trim(node.attribute("UUID").value()));
        const std::string where = "Added " + fixture.uuid;
        if (fixture.uuid.empty()) {
            result.diagnostics.warn("Added", "no UUID; entry ignored");
            continue;
        }
        if (result.edits.find_added(fixture.uuid)) {
            result.diagnostics.warn(where, "listed twice; the first entry is used");
            continue;
        }
        fixture.gdtf = std::string(text::trim(node.attribute("GDTF").value()));
        if (!plain_file_name(fixture.gdtf)) {
            result.diagnostics.warn(where, "GDTF must be the name of one file in the fixture library; entry ignored");
            continue;
        }
        fixture.name = node.attribute("Name").value();
        fixture.fixture_id = node.attribute("FixtureID").value();
        fixture.mode = node.attribute("Mode").value();
        if (pugi::xml_attribute matrix = node.attribute("Matrix")) {
            if (!parse_matrix(matrix.value(), fixture.world)) {
                fixture.world = Transform();
                result.diagnostics.warn(where, "Matrix is not four rows of three numbers; it is put at the origin");
            }
        }
        long long u = 0;
        long long a = 1;
        if (node.attribute("Universe") || node.attribute("Address")) {
            if (text::parse_int(node.attribute("Universe").value(), u) && text::parse_int(node.attribute("Address").value(), a) && u >= 0 &&
                u <= 32767 && a >= 1 && a <= 512) {
                fixture.universe = static_cast<int>(u);
                fixture.address = static_cast<int>(a);
            } else {
                result.diagnostics.warn(where, "Universe and Address must both be given, 0-32767 and 1-512; it is left unpatched");
            }
        }
        text::parse_bool(node.attribute("InvertPan").value(), fixture.invert_pan);
        text::parse_bool(node.attribute("InvertTilt").value(), fixture.invert_tilt);
        result.edits.added.push_back(std::move(fixture));
    }

    // A fixture cannot be both taken out and changed: taken out wins.
    for (size_t i = result.edits.fixtures.size(); i-- > 0;) {
        if (result.edits.is_removed(result.edits.fixtures[i].uuid)) {
            result.edits.fixtures.erase(result.edits.fixtures.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    return result;
}

std::string write_show_edits(const ShowEdits& edits) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<ShowEdits Version=\"1\">\n";
    for (const FixtureEdit& edit : edits.fixtures) {
        if (edit.uuid.empty() || edit.empty() || edits.is_removed(edit.uuid)) continue;
        out += "  <Fixture UUID=\"" + escaped(edit.uuid) + "\"";
        if (edit.world) {
            out += " Matrix=\"";
            for (const double* row : {edit.world->u, edit.world->v, edit.world->w, edit.world->o}) {
                out += "{" + number(row[0]) + "," + number(row[1]) + "," + number(row[2]) + "}";
            }
            out += "\"";
        }
        if (edit.universe && edit.address) {
            out += " Universe=\"" + std::to_string(*edit.universe) + "\" Address=\"" + std::to_string(*edit.address) + "\"";
        }
        if (edit.mode) out += " Mode=\"" + escaped(*edit.mode) + "\"";
        if (edit.name) out += " Name=\"" + escaped(*edit.name) + "\"";
        if (edit.fixture_id) out += " FixtureID=\"" + escaped(*edit.fixture_id) + "\"";
        if (edit.invert_pan) out += std::string(" InvertPan=\"") + (*edit.invert_pan ? "true" : "false") + "\"";
        if (edit.invert_tilt) out += std::string(" InvertTilt=\"") + (*edit.invert_tilt ? "true" : "false") + "\"";
        if (edit.gdtf && plain_file_name(*edit.gdtf)) out += " GDTF=\"" + escaped(*edit.gdtf) + "\"";
        out += "/>\n";
    }
    for (const AddedFixture& fixture : edits.added) {
        if (fixture.uuid.empty() || !plain_file_name(fixture.gdtf)) continue;
        out += "  <Added UUID=\"" + escaped(fixture.uuid) + "\" Name=\"" + escaped(fixture.name) + "\"";
        if (!fixture.fixture_id.empty()) out += " FixtureID=\"" + escaped(fixture.fixture_id) + "\"";
        out += " GDTF=\"" + escaped(fixture.gdtf) + "\"";
        if (!fixture.mode.empty()) out += " Mode=\"" + escaped(fixture.mode) + "\"";
        out += " Matrix=\"" + matrix_text(fixture.world) + "\"";
        out += " Universe=\"" + std::to_string(fixture.universe) + "\" Address=\"" + std::to_string(fixture.address) + "\"";
        if (fixture.invert_pan) out += " InvertPan=\"true\"";
        if (fixture.invert_tilt) out += " InvertTilt=\"true\"";
        out += "/>\n";
    }
    for (const std::string& uuid : edits.removed) {
        if (!uuid.empty()) out += "  <Removed UUID=\"" + escaped(uuid) + "\"/>\n";
    }
    out += "</ShowEdits>\n";
    return out;
}

std::string show_edits_path(std::string_view mvr_path) {
    std::string path(mvr_path);
    if (path.size() >= 4 && text::iequals(std::string_view(path).substr(path.size() - 4), ".mvr")) {
        path.resize(path.size() - 4);
    }
    return path + ".edits.xml";
}

}  // namespace viz
