// Text parsing helpers shared by the GDTF and MVR parsers. Internal to the library.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "vizcore/types.h"

namespace viz {
namespace text {

std::string_view trim(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
std::vector<std::string_view> split(std::string_view s, char separator);

bool parse_int(std::string_view s, long long& out);

// Always uses '.' as the decimal separator, whatever locale the host application has set.
bool parse_double(std::string_view s, double& out);

// "true"/"false" in any case, or "1"/"0".
bool parse_bool(std::string_view s, bool& out);

// Comma-separated numbers, optionally wrapped in one pair of braces. Fails unless exactly
// `count` numbers are present.
bool parse_numbers(std::string_view s, size_t count, double* out);

// "{a,b,..}{c,d,..}..." as GDTF and MVR write matrices: `rows` groups of `cols` numbers, row by row.
bool parse_braced_rows(std::string_view s, size_t rows, size_t cols, double* out);

bool parse_color(std::string_view s, ColorCIE& out);

}  // namespace text
}  // namespace viz
