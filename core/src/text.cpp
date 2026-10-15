#include "text.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace viz {
namespace text {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Powers of ten a double represents exactly.
const double kExactPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                              1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

}  // namespace

std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

std::vector<std::string_view> split(std::string_view s, char separator) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        size_t end = s.find(separator, start);
        if (end == std::string_view::npos) {
            parts.push_back(s.substr(start));
            return parts;
        }
        parts.push_back(s.substr(start, end - start));
        start = end + 1;
    }
}

bool parse_int(std::string_view s, long long& out) {
    s = trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    if (s.empty()) return false;
    const char* end = s.data() + s.size();
    auto result = std::from_chars(s.data(), end, out);
    return result.ec == std::errc() && result.ptr == end;
}

bool parse_double(std::string_view s, double& out) {
    s = trim(s);
    size_t i = 0;
    bool negative = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        negative = s[i] == '-';
        ++i;
    }
    // Up to 19 significant digits fit a uint64; anything beyond only shifts the exponent.
    uint64_t mantissa = 0;
    int significant = 0;
    int exponent = 0;
    bool any_digit = false;
    for (; i < s.size() && is_digit(s[i]); ++i) {
        any_digit = true;
        if (significant < 19) {
            mantissa = mantissa * 10 + static_cast<uint64_t>(s[i] - '0');
            if (mantissa != 0) ++significant;
        } else {
            ++exponent;
        }
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        for (; i < s.size() && is_digit(s[i]); ++i) {
            any_digit = true;
            if (significant < 19) {
                mantissa = mantissa * 10 + static_cast<uint64_t>(s[i] - '0');
                if (mantissa != 0) ++significant;
                --exponent;
            }
        }
    }
    if (!any_digit) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool exponent_negative = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
            exponent_negative = s[i] == '-';
            ++i;
        }
        if (i >= s.size() || !is_digit(s[i])) return false;
        int written = 0;
        for (; i < s.size() && is_digit(s[i]); ++i) {
            if (written < 10000) written = written * 10 + (s[i] - '0');
        }
        exponent += exponent_negative ? -written : written;
    }
    if (i != s.size()) return false;

    double value = static_cast<double>(mantissa);
    if (exponent < 0) {
        value = (-exponent <= 22) ? value / kExactPow10[-exponent] : value * std::pow(10.0, exponent);
    } else if (exponent > 0) {
        value = (exponent <= 22) ? value * kExactPow10[exponent] : value * std::pow(10.0, exponent);
    }
    out = negative ? -value : value;
    return true;
}

bool parse_bool(std::string_view s, bool& out) {
    s = trim(s);
    if (iequals(s, "true") || s == "1") {
        out = true;
        return true;
    }
    if (iequals(s, "false") || s == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parse_numbers(std::string_view s, size_t count, double* out) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '{' && s.back() == '}') s = s.substr(1, s.size() - 2);
    std::vector<std::string_view> parts = split(s, ',');
    if (parts.size() != count) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!parse_double(parts[i], out[i])) return false;
    }
    return true;
}

bool parse_braced_rows(std::string_view s, size_t rows, size_t cols, double* out) {
    s = trim(s);
    for (size_t row = 0; row < rows; ++row) {
        s = trim(s);
        if (s.empty() || s.front() != '{') return false;
        size_t close = s.find('}');
        if (close == std::string_view::npos) return false;
        if (!parse_numbers(s.substr(1, close - 1), cols, out + row * cols)) return false;
        s.remove_prefix(close + 1);
    }
    return trim(s).empty();
}

bool parse_color(std::string_view s, ColorCIE& out) {
    double values[3];
    if (!parse_numbers(s, 3, values)) return false;
    out.x = values[0];
    out.y = values[1];
    out.Y = values[2];
    return true;
}

}  // namespace text
}  // namespace viz
