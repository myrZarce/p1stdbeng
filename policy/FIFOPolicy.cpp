#include "FIFOPolicy.h"

#include <algorithm>

namespace bufman {

void FIFOPolicy::init(std::size_t pool_size) {
    queue_.clear();
    positions_.clear();
}
void FIFOPolicy::on_access(std::size_t frame) {
    // FIFO does not change the order when a frame is accessed.
}

void FIFOPolicy::on_load(std::size_t frame) {
    on_remove(frame);

    queue_.push_back(frame);
    auto it = queue_.end();
    --it;
    positions_[frame] = it;
}

void FIFOPolicy::on_remove(std::size_t frame) {
    auto it = positions_.find(frame);

    if (it != positions_.end()) {
        queue_.erase(it->second);
        positions_.erase(it);
    }
}

std::optional<std::size_t> FIFOPolicy::pick_victim(
    const std::vector<std::size_t>& candidates) const {

    for (std::size_t frame : queue_) {
        if (std::find(candidates.begin(), candidates.end(), frame)
            != candidates.end()) {
            return frame;
        }
    }

    return std::nullopt;
}
}