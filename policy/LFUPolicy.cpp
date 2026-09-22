#include "LFUPolicy.h"

#include <algorithm>

namespace bufman {

void LFUPolicy::init(std::size_t pool_size) {
    buckets_.clear();
    slots_.clear();
    min_freq_ = 0;
}
void LFUPolicy::on_load(std::size_t frame) {
    buckets_[1].push_front(frame);

    slots_[frame] = {
        1,
        buckets_[1].begin()
    };

    min_freq_ = 1;
}

void LFUPolicy::on_access(std::size_t frame) {
    auto it = slots_.find(frame);

    if (it == slots_.end()) {
        return;
    }

    std::size_t old_count = it->second.count;
    auto& old_bucket = buckets_[old_count];

    old_bucket.erase(it->second.pos);

    if (old_bucket.empty()) {
        buckets_.erase(old_count);

        if (min_freq_ == old_count) {
            min_freq_ = old_count + 1;
        }
    }

    std::size_t new_count = old_count + 1;

    buckets_[new_count].push_front(frame);

    it->second.count = new_count;
    it->second.pos = buckets_[new_count].begin();
}
void LFUPolicy::on_remove(std::size_t frame) {
    auto it = slots_.find(frame);

    if (it == slots_.end()) {
        return;
    }

    std::size_t count = it->second.count;
    auto& bucket = buckets_[count];

    bucket.erase(it->second.pos);

    if (bucket.empty()) {
        buckets_.erase(count);
    }

    slots_.erase(it);

    if (slots_.empty()) {
        min_freq_ = 0;
    } else if (count == min_freq_ && buckets_.find(count) == buckets_.end()) {
        min_freq_ = slots_.begin()->second.count;

        for (const auto& entry : slots_) {
            if (entry.second.count < min_freq_) {
                min_freq_ = entry.second.count;
            }
        }
    }
}

std::optional<std::size_t> LFUPolicy::pick_victim(
    const std::vector<std::size_t>& candidates) const {

    std::optional<std::size_t> victim;
    std::size_t lowest_freq = 0;

    for (std::size_t frame : candidates) {
        auto it = slots_.find(frame);

        if (it == slots_.end()) {
            continue;
        }

        std::size_t freq = it->second.count;

        if (!victim || freq < lowest_freq) {
            victim = frame;
            lowest_freq = freq;
        } 
        else if (freq == lowest_freq) {
            const auto& bucket = buckets_.at(freq);

            for (auto rit = bucket.rbegin(); rit != bucket.rend(); ++rit) {
                if (*rit == frame) {
                    victim = frame;
                    break;
                }

                if (*rit == *victim) {
                    break;
                }
            }
        }
    }

    return victim;
}
}
