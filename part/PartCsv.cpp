#include "PartCsv.h"

#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>

namespace bufman {
namespace {
bool parse_integer(const std::string& text, int& value) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool parse_float(const std::string& text, float& value) {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}
bool parse_line(const std::string& line, Part& part, std::string& error) {
    std::stringstream input(line);
    std::string id_text, name, weight_text, color_text, price_text, material, extra;
    if (!std::getline(input, id_text, ',') || !std::getline(input, name, ',') ||
        !std::getline(input, weight_text, ',') || !std::getline(input, color_text, ',') ||
        !std::getline(input, price_text, ',') || !std::getline(input, material, ',') ||
        std::getline(input, extra, ',')) {
        error = "expected exactly six comma-separated fields"; return false;
    }
    int id = 0, color = 0; float weight = 0.0f, price = 0.0f;
    if (!parse_integer(id_text, id) || id <= 0) { error = "part_id must be a positive integer"; return false; }
    if (!parse_float(weight_text, weight) || weight < 0.0f) { error = "part_weight must be a nonnegative number"; return false; }
    if (!parse_integer(color_text, color) || color < 0 || color > 5) { error = "part_color must be an integer in [0, 5]"; return false; }
    if (!parse_float(price_text, price) || price < 0.0f) { error = "part_price must be a nonnegative number"; return false; }
    if (name.size() > 9) { error = "part_name must contain at most 9 characters"; return false; }
    if (material.size() > 9) { error = "part_material must contain at most 9 characters"; return false; }
    part = Part{}; part.part_id = id; part.part_weight = weight; part.part_color = color; part.part_price = price;
    name.copy(part.part_name, name.size()); material.copy(part.part_material, material.size()); return true;
}
}
PartLoadResult load_parts(const std::string& path, std::ostream& diagnostics) {
    PartLoadResult result; std::ifstream input(path);
    if (!input) { diagnostics << "cannot open CSV file: " << path << '\n'; result.skipped = 1; return result; }
    std::string line; std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty()) { ++result.skipped; diagnostics << "line " << line_number << ": blank line\n"; continue; }
        Part part{}; std::string error;
        if (!parse_line(line, part, error)) { ++result.skipped; diagnostics << "line " << line_number << ": " << error << '\n'; continue; }
        result.parts.push_back(part);
    }
    return result;
}
}