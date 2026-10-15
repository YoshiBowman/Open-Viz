// A small JSON reader, for the scene description inside a GLB model. Not a general library: it
// reads well-formed JSON into a tree and refuses anything deeper than 64 levels.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viz {
namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    // All of these are safe on any value: a missing key or a wrong type gives null or the fallback.
    const Value* get(std::string_view key) const;
    const Value* at(size_t index) const;
    double num(double fallback = 0.0) const { return type == Type::Number ? number : fallback; }
    long long integer(long long fallback = -1) const;
    size_t size() const { return type == Type::Array ? array.size() : 0; }
};

bool parse(std::string_view text, Value& out);

}  // namespace json
}  // namespace viz
