#include "PartGenerator.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace bufman {
std::vector<Part> generate_parts(std::size_t count, int first_part_id) {
    constexpr std::array<const char*, 6> materials = {"steel", "brass", "copper", "plastic", "rubber", "aluminum"};
    std::vector<Part> parts; parts.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        Part part{}; part.part_id = first_part_id + static_cast<int>(i);
        part.part_weight = 0.25f + static_cast<float>(i % 100) * 0.1f;
        part.part_color = static_cast<int>(i % 6); part.part_price = 1.0f + static_cast<float>(i % 1000) * 0.25f;
        const std::string name = "P" + std::to_string(part.part_id);
        name.copy(part.part_name, std::min(name.size(), sizeof(part.part_name) - 1));
        std::strncpy(part.part_material, materials[i % materials.size()], sizeof(part.part_material) - 1);
        parts.push_back(part);
    }
    return parts;
}
}