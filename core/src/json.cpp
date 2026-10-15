#include "json.h"

#include <cmath>

#include "text.h"

namespace viz {
namespace json {

const Value* Value::get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& member : object) {
        if (member.first == key) return &member.second;
    }
    return nullptr;
}

const Value* Value::at(size_t index) const {
    return type == Type::Array && index < array.size() ? &array[index] : nullptr;
}

long long Value::integer(long long fallback) const {
    if (type != Type::Number || number != std::floor(number) || std::fabs(number) > 9e15) return fallback;
    return static_cast<long long>(number);
}

namespace {

struct Reader {
    std::string_view text;
    size_t at = 0;

    void skip_space() {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    }

    bool literal(std::string_view word) {
        if (text.substr(at, word.size()) != word) return false;
        at += word.size();
        return true;
    }

    static void append_utf8(std::string& out, unsigned code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(unsigned& out) {
        if (at + 4 > text.size()) return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = text[at++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (at >= text.size() || text[at] != '"') return false;
        ++at;
        while (at < text.size()) {
            const char c = text[at++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at >= text.size()) return false;
            const char escape = text[at++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    if (!hex4(code)) return false;
                    if (code >= 0xD800 && code <= 0xDBFF) {    // the first half of a surrogate pair
                        unsigned low = 0;
                        if (!literal("\\u") || !hex4(low) || low < 0xDC00 || low > 0xDFFF) return false;
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false;
                    }
                    append_utf8(out, code);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool number(double& out) {
        const size_t start = at;
        if (at < text.size() && text[at] == '-') ++at;
        while (at < text.size() && ((text[at] >= '0' && text[at] <= '9') || text[at] == '.' || text[at] == 'e' || text[at] == 'E' ||
                                    text[at] == '+' || text[at] == '-')) {
            ++at;
        }
        return at > start && text::parse_double(text.substr(start, at - start), out);
    }

    bool value(Value& out, int depth) {
        if (depth > 64) return false;
        skip_space();
        if (at >= text.size()) return false;
        const char c = text[at];
        if (c == '{') {
            ++at;
            out.type = Value::Type::Object;
            skip_space();
            if (at < text.size() && text[at] == '}') { ++at; return true; }
            while (true) {
                skip_space();
                std::string key;
                if (!string(key)) return false;
                skip_space();
                if (at >= text.size() || text[at] != ':') return false;
                ++at;
                out.object.emplace_back(std::move(key), Value());
                if (!value(out.object.back().second, depth + 1)) return false;
                skip_space();
                if (at < text.size() && text[at] == ',') { ++at; continue; }
                if (at < text.size() && text[at] == '}') { ++at; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++at;
            out.type = Value::Type::Array;
            skip_space();
            if (at < text.size() && text[at] == ']') { ++at; return true; }
            while (true) {
                out.array.emplace_back();
                if (!value(out.array.back(), depth + 1)) return false;
                skip_space();
                if (at < text.size() && text[at] == ',') { ++at; continue; }
                if (at < text.size() && text[at] == ']') { ++at; return true; }
                return false;
            }
        }
        if (c == '"') {
            out.type = Value::Type::String;
            return string(out.string);
        }
        if (literal("true")) { out.type = Value::Type::Bool; out.boolean = true; return true; }
        if (literal("false")) { out.type = Value::Type::Bool; return true; }
        if (literal("null")) { return true; }
        out.type = Value::Type::Number;
        return number(out.number);
    }
};

}  // namespace

bool parse(std::string_view text, Value& out) {
    Reader reader{text};
    out = Value();
    if (!reader.value(out, 0)) return false;
    reader.skip_space();
    return reader.at == text.size();
}

}  // namespace json
}  // namespace viz
