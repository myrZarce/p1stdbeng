#include "file/BlockFile.h"
#include "bufpool/BufferManager.h"
#include "config/Config.h"
#include "part/PartCsv.h"
#include "part/PartGenerator.h"
#include "part/PartSerializer.h"
#include "policy/FIFOPolicy.h"
#include "policy/LFUPolicy.h"
#include "policy/LRUPolicy.h"
#include "policy/LRUv2Policy.h"
#include "policy/MRUPolicy.h"
#include "policy/PolicyFactory.h"
#include "policy/ReplacementPolicy.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "PASS: " << message << "\n";
    } else {
        std::cout << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string temp_path(const char* suffix) {
    return std::string("ex09-bufman-") + std::to_string(getpid()) + suffix;
}

// Seeds `count` blocks; block i is filled with the byte 'a' + i, so a frame's
// buffer[0] tells which block it holds.
void seed_file(const std::string& path, int count) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_append(path, file, error)) {
        std::cerr << "seed: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    for (int i = 0; i < count; ++i) {
        std::memset(block.data(), static_cast<char>('a' + i), block.size());
        if (!bufman::write_block(file, static_cast<std::uint64_t>(i),
                                    block.data(), block.size(), error)) {
            std::cerr << "seed: " << error << '\n';
            std::exit(1);
        }
    }
    bufman::close(file);
}

char read_disk_byte(const std::string& path, std::uint64_t block_number) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_read(path, file, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    if (!bufman::read_block(file, block_number, block, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    bufman::close(file);
    return block[0];
}

std::array<char, bufman::kBlockSize> read_disk_block(const std::string& path,
                                                     std::uint64_t block_number) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_read(path, file, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    if (!bufman::read_block(file, block_number, block, error)) {
        std::cerr << "verify: " << error << '\n';
        std::exit(1);
    }
    bufman::close(file);
    return block;
}

Person make_person(int pid, const char* name, int age, const char* city) {
    Person value{};
    value.pid = pid;
    value.age = age;
    std::strncpy(value.name, name, sizeof(value.name) - 1);
    std::strncpy(value.city, city, sizeof(value.city) - 1);
    return value;
}

Part make_part(int part_id, const char* name, float weight, int color,
               float price, const char* material) {
    Part value{};
    value.part_id = part_id;
    value.part_weight = weight;
    value.part_color = color;
    value.part_price = price;
    std::strncpy(value.part_name, name, sizeof(value.part_name) - 1);
    std::strncpy(value.part_material, material, sizeof(value.part_material) - 1);
    return value;
}

// Writes one block in the Person record layout (serialize_block zero-fills
// the free slots), so on-disk bytes match what the pool will read.
void seed_person_block(const std::string& path, std::uint64_t block_number,
                       const std::vector<Person>& people) {
    bufman::File file;
    std::string error;
    if (!bufman::open_for_append(path, file, error)) {
        std::cerr << "seed: " << error << '\n';
        std::exit(1);
    }
    std::array<char, bufman::kBlockSize> block{};
    bufman::serialize_block(people, block.data());
    if (!bufman::write_block(file, block_number, block.data(), block.size(), error)) {
        std::cerr << "seed: " << error << '\n';
        std::exit(1);
    }
    bufman::close(file);
}

// Records every hook call so tests can assert the manager -> policy contract.
class RecordingPolicy final : public bufman::ReplacementPolicy {
public:
    void init(std::size_t) override {
        events.clear();
        last_candidates.clear();
    }
    void on_access(std::size_t frame) override {
        events.push_back("access:" + std::to_string(frame));
    }
    void on_load(std::size_t frame) override {
        events.push_back("load:" + std::to_string(frame));
    }
    void on_remove(std::size_t frame) override {
        events.push_back("remove:" + std::to_string(frame));
    }
    std::optional<std::size_t> pick_victim(
            const std::vector<std::size_t>& candidates) const override {
        last_candidates = candidates;
        if (candidates.empty()) {
            return std::nullopt;
        }
        return candidates.front();
    }

    mutable std::vector<std::string> events;
    mutable std::vector<std::size_t> last_candidates;
};

// A broken policy: returns a fixed answer regardless of the candidates, so
// tests can assert the manager refuses out-of-range or pinned victims.
class BadPolicy final : public bufman::ReplacementPolicy {
public:
    explicit BadPolicy(std::optional<std::size_t> answer) : answer_(answer) {}
    void init(std::size_t) override {}
    void on_access(std::size_t) override {}
    void on_load(std::size_t) override {}
    void on_remove(std::size_t) override {}
    std::optional<std::size_t> pick_victim(
            const std::vector<std::size_t>&) const override {
        return answer_;
    }

private:
    std::optional<std::size_t> answer_;
};

}

int main() {
    const std::string seeded_path = temp_path("-seed.bin");
    std::remove(seeded_path.c_str());
    seed_file(seeded_path, 6);

    // Session configuration: the suite honors a conf.json in the working
    // directory for the session-shaped scan demo near the end. Every other
    // block keeps explicit pool sizes and policies so eviction-order
    // assertions stay deterministic no matter what conf.json says. No
    // conf.json? Default to lru/8.
    bufman::Config session_config;
    session_config.policy = "lru";
    session_config.pool_size = 8;
    {
        std::ifstream probe("conf.json");
        if (probe) {
            std::string config_error;
            if (!bufman::load_config("conf.json", session_config, std::cerr,
                                     config_error)) {
                check(false, "conf.json parses: " + config_error);
                session_config.policy = "lru";
                session_config.pool_size = 8;
            } else {
                check(session_config.pool_size >= 1, "conf.json pool_size is >= 1");
            }
        }
    }

    // --- miss loads from disk; hit reuses the frame and stacks pins ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(3, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        check(mgr.open_file(seeded_path, file, error), "open_file succeeds");

        std::size_t f = 999;
        check(mgr.pin(*file, 0, f, error), "pin block 0 (miss) succeeds");
        check(f == 0, "block 0 lands in never-used frame 0");
        check(mgr.frame(f).buffer[0] == 'a', "block 0 bytes loaded from disk");
        check(mgr.frame(f).pin_count == 1, "pin gives pin_count 1");

        std::size_t f_again = 999;
        check(mgr.pin(*file, 0, f_again, error) && f_again == f,
              "re-pinning block 0 hits the same frame");
        check(mgr.frame(f).pin_count == 2, "second pin gives pin_count 2");
        check(mgr.resident(*file, 0), "resident reports a loaded block");
        check(!mgr.resident(*file, 5), "resident reports an absent block");
        mgr.unpin(f, false, error);
        mgr.unpin(f, false, error);
        check(mgr.frame(f).pin_count == 0, "two unpins give pin_count 0");

        // --- free frames are consumed in order; then LRU decides ---
        std::size_t f1 = 999, f2 = 999;
        check(mgr.pin(*file, 1, f1, error) && f1 == 1, "block 1 lands in never-used frame 1");
        check(mgr.pin(*file, 2, f2, error) && f2 == 2, "block 2 lands in never-used frame 2");
        // Frame 0 was already unpinned in the hit test above.
        mgr.unpin(1, false, error);
        mgr.unpin(2, false, error);

        // LRU order (MRU -> LRU) is now 0, 2, 1.
        mgr.pin(*file, 0, f, error);
        mgr.unpin(f, false, error);
        std::size_t f3 = 999;
        check(mgr.pin(*file, 3, f3, error) && f3 == 1,
              "block 3 evicts the LRU frame (block 1's), not block 0's");
        check(mgr.frame(f3).buffer[0] == 'd', "block 3 bytes loaded");
        mgr.unpin(f3, false, error);

        std::size_t f1b = 999;
        check(mgr.pin(*file, 1, f1b, error) && f1b == 2,
              "re-pinning block 1 evicts the next LRU frame (block 2's)");
        check(mgr.frame(f1b).buffer[0] == 'b', "block 1 bytes reloaded from disk");
        mgr.unpin(f1b, false, error);
        mgr.close_all(error);
    }

    // --- dirty write-back on eviction; modified bytes survive on disk ---
    const std::string dirty_path = temp_path("-dirty.bin");
    std::remove(dirty_path.c_str());
    seed_file(dirty_path, 3);
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(dirty_path, file, error);

        std::size_t f0 = 999, f1 = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);

        std::size_t fz = 999;
        check(mgr.pin(*file, 2, fz, error) && fz == 1,
              "block 2 evicts the LRU frame");
        mgr.frame(fz).buffer[0] = 'Z';
        mgr.unpin(fz, true, error);

        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        std::size_t fre = 999;
        check(mgr.pin(*file, 1, fre, error) && fre == fz,
              "pinning another block evicts the dirty frame");
        check(read_disk_byte(dirty_path, 2) == 'Z',
              "dirty frame was written back to disk before reuse");
        check(mgr.frame(fre).buffer[0] == 'b', "fresh bytes loaded after eviction");

        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        check(mgr.pin(*file, 2, fz, error), "re-pin block 2 after eviction");
        check(mgr.frame(fz).buffer[0] == 'Z',
              "modified bytes survive write-back and reload");
        mgr.unpin(fz, false, error);
        mgr.close_all(error);
    }

    // --- flush writes through immediately and is a no-op when clean ---
    const std::string flush_path = temp_path("-flush.bin");
    std::remove(flush_path.c_str());
    seed_file(flush_path, 1);
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(flush_path, file, error);

        std::size_t f = 999;
        mgr.pin(*file, 0, f, error);
        mgr.frame(f).buffer[0] = 'F';
        mgr.unpin(f, true, error);
        check(mgr.flush(f, error), "flush succeeds");
        check(read_disk_byte(flush_path, 0) == 'F', "flush wrote the dirty frame");
        check(mgr.flush(f, error) && error.empty(), "flush on a clean frame is a no-op");
        check(mgr.close_all(error), "close_all succeeds and reports its status");

        bufman::File closed;
        std::size_t f2 = 999;
        check(!mgr.pin(closed, 0, f2, error), "pin after close_all fails");
        check(!error.empty(), "pin after close_all reports an error");
    }

    // --- pinned frames are never evicted ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);

        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        check(!mgr.pin(*file, 2, fx, error), "pin with all frames pinned fails");
        check(error == "all frames pinned", "all-frames-pinned error message");
        mgr.unpin(f1, false, error);
        check(mgr.pin(*file, 2, fx, error) && fx == f1,
              "after unpin, block 2 takes the LRU frame");
        mgr.unpin(f0, false, error);
        mgr.unpin(fx, false, error);
        mgr.close_all(error);
    }

    // --- unpin at pin count 0 throws ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);

        std::size_t f = 999;
        mgr.pin(*file, 0, f, error);
        mgr.unpin(f, false, error);
        bool threw = false;
        try {
            mgr.unpin(f, false, error);
        } catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "unpin at pin count 0 throws logic_error");
        mgr.close_all(error);
    }

    // --- manager -> policy contract, observed through a recording fake ---
    {
        bufman::BufferManager mgr;
        std::string error;
        auto policy = std::make_unique<RecordingPolicy>();
        RecordingPolicy* policy_ptr = policy.get();
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);

        std::size_t f0 = 999, f1 = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f0, false, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f1, false, error);
        check(policy_ptr->events ==
                  std::vector<std::string>({"load:0", "access:0", "load:1"}),
              "policy sees load/access hooks in order");

        std::size_t fx = 999;
        mgr.pin(*file, 2, fx, error);
        check(policy_ptr->last_candidates == std::vector<std::size_t>({0, 1}),
              "pick_victim sees the unpinned page-holding frames");
        check(policy_ptr->events.size() == 5 && policy_ptr->events[3] == "remove:0" &&
                  policy_ptr->events[4] == "load:0",
              "eviction reports remove then load");
        mgr.unpin(fx, false, error);

        mgr.pin(*file, 1, f1, error);
        check(policy_ptr->events.back() == "access:1", "hit is reported as access");
        mgr.pin(*file, 3, fx, error);
        check(policy_ptr->last_candidates == std::vector<std::size_t>({0}),
              "pinned frames are excluded from candidates");
        mgr.unpin(f1, false, error);
        mgr.unpin(fx, false, error);
    }

    // --- a broken replacement policy cannot corrupt the pool ---
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<BadPolicy>(std::optional<std::size_t>(7)));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);

        std::size_t f0 = 999;
        check(mgr.pin(*file, 0, f0, error), "pin with a broken policy still works");
        mgr.unpin(f0, false, error);
        std::size_t fx = 999;
        check(!mgr.pin(*file, 1, fx, error), "out-of-range victim is rejected");
        check(error == "replacement policy returned an invalid victim",
              "invalid-victim error message");
        check(mgr.resident(*file, 0), "rejected eviction leaves the resident page intact");
        check(mgr.pin(*file, 0, fx, error), "pool still usable after the refusal");
        mgr.unpin(fx, false, error);
    }
    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<BadPolicy>(std::optional<std::size_t>(0)));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);

        std::size_t f0 = 999, f1 = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f1, false, error);
        std::size_t fx = 999;
        check(!mgr.pin(*file, 2, fx, error), "pinned victim is rejected");
        check(error == "replacement policy returned an invalid victim",
              "pinned-victim error message");
        check(mgr.frame(f0).pin_count == 1, "pinned page was not evicted");
        check(mgr.frame(f1).pin_count == 0, "the unpinned frame was not evicted either");
        mgr.unpin(f0, false, error);
    }

    // --- person records read and written through frame buffers ---
    const std::string people_path = temp_path("-people.bin");
    std::remove(people_path.c_str());
    seed_person_block(people_path, 0,
                      {make_person(1, "Ana", 20, "PR"), make_person(2, "Bob", 31, "NY")});
    seed_person_block(people_path, 1, {});
    seed_person_block(people_path, 2, {});

    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(2, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        check(mgr.open_file(people_path, file, error), "open person file");

        std::size_t f0 = 999;
        check(mgr.pin(*file, 0, f0, error) && f0 == 0, "person block 0 pins into frame 0");
        check(bufman::record_count(mgr.frame(f0).buffer) == 2, "seeded block 0 holds 2 records");
        Person p{};
        check(bufman::get_record(mgr.frame(f0).buffer, 0, p) && p.pid == 1 &&
                  std::string(p.name) == "Ana" && p.age == 20 && std::string(p.city) == "PR",
              "slot 0 reads back Ana through the frame buffer");
        check(bufman::get_record(mgr.frame(f0).buffer, 1, p) && p.pid == 2 &&
                  std::string(p.name) == "Bob",
              "slot 1 reads back Bob");
        check(!bufman::get_record(mgr.frame(f0).buffer, 2, p), "empty slot 2 reports no record");

        // Update in place, then march the dirty frame to the LRU position so
        // the next miss forces a write-back.
        check(bufman::put_record(mgr.frame(f0).buffer, 0, make_person(1, "Ana", 21, "PR")),
              "put_record overwrites slot 0 in the frame buffer");
        mgr.unpin(f0, true, error);
        std::size_t f1 = 999;
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f1, false, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f1, false, error);
        std::size_t f2 = 999;
        check(mgr.pin(*file, 2, f2, error) && f2 == 0,
              "block 2 evicts the LRU frame (the dirty block 0)");
        mgr.unpin(f2, false, error);

        std::size_t fre = 999;
        check(mgr.pin(*file, 0, fre, error) && fre == 1, "re-pinning block 0 loads from disk");
        check(bufman::get_record(mgr.frame(fre).buffer, 0, p) && p.age == 21,
              "updated age survives write-back and reload");
        mgr.unpin(fre, false, error);
        const auto disk0 = read_disk_block(people_path, 0);
        check(bufman::get_record(disk0.data(), 0, p) && p.age == 21,
              "disk block 0 holds the updated record");
        mgr.close_all(error);
    }

    {
        // Pool of one: every pin after a dirty unpin evicts and writes back.
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(people_path, file, error);
        Person p{};

        std::size_t f = 999;
        check(mgr.pin(*file, 1, f, error), "pin zeroed block 1");
        auto slot = bufman::first_free_slot(mgr.frame(f).buffer);
        check(slot && *slot == 0, "first free slot of a zeroed block is 0");
        check(bufman::put_record(mgr.frame(f).buffer, *slot, make_person(3, "Cid", 44, "LA")),
              "append Cid into the free slot");
        mgr.unpin(f, true, error);
        mgr.pin(*file, 2, f, error);
        mgr.unpin(f, false, error);
        auto disk1 = read_disk_block(people_path, 1);
        check(bufman::record_count(disk1.data()) == 1 && bufman::get_record(disk1.data(), 0, p) &&
                  p.pid == 3,
              "appended record was written back to disk");

        check(mgr.pin(*file, 1, f, error), "re-pin block 1");
        slot = bufman::first_free_slot(mgr.frame(f).buffer);
        check(slot && *slot == 1, "next free slot is 1");
        check(bufman::put_record(mgr.frame(f).buffer, *slot, make_person(5, "Eve", 33, "OH")),
              "append Eve into the next free slot");
        mgr.unpin(f, true, error);
        mgr.pin(*file, 2, f, error);
        mgr.unpin(f, false, error);
        disk1 = read_disk_block(people_path, 1);
        check(bufman::record_count(disk1.data()) == 2, "block 1 now holds 2 records");
        mgr.close_all(error);
    }

    {
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(people_path, file, error);

        std::size_t f = 999;
        const bool pinned = mgr.pin(*file, 2, f, error);
        check(pinned, "pin block 2 for full-block test");
        if (!pinned) {
            std::cerr << "pin block 2: " << error << '\n';
            mgr.close_all(error);
            return failures == 0 ? 1 : failures;
        }
        bool all_put = true;
        for (std::size_t i = 0; i < bufman::kRecordsPerBlock; ++i) {
            all_put = all_put && bufman::put_record(mgr.frame(f).buffer, i,
                                                    make_person(static_cast<int>(100 + i), "Px", 40, "TX"));
        }
        check(all_put, "put_record fills all 195 slots");
        check(!bufman::first_free_slot(mgr.frame(f).buffer), "full block has no free slot");
        mgr.unpin(f, true, error);
        mgr.pin(*file, 0, f, error);
        mgr.unpin(f, false, error);
        const auto disk2 = read_disk_block(people_path, 2);
        check(bufman::record_count(disk2.data()) == bufman::kRecordsPerBlock,
              "full block persisted with 195 records");

        std::size_t f3 = 999;
        check(!mgr.pin(*file, 3, f3, error), "pin past EOF fails: new blocks must be seeded first");
        mgr.close_all(error);
    }

    {
        std::array<char, bufman::kBlockSize> block{};
        bufman::initialize_block(block.data());
        Person zoe = make_person(9, "Zoe", 30, "PR");
        check(!bufman::get_record(block.data(), bufman::kRecordsPerBlock, zoe),
              "get_record rejects an out-of-range slot");
        check(!bufman::put_record(block.data(), bufman::kRecordsPerBlock, zoe),
              "put_record rejects an out-of-range slot");
        Person nobody = make_person(0, "Nobody", 1, "PR");
        check(!bufman::put_record(block.data(), 0, nobody),
              "put_record rejects pid 0 (free-slot sentinel)");
    }

    {
        bufman::BufferManager mgr;
        std::string error;
        // This block is session-shaped, so it uses the conf.json policy and
        // pool size (with the lru/8 default when there is no conf.json).
        auto policy = bufman::PolicyFactory::instance().create(
            session_config.policy, error);
        check(policy != nullptr,
              "conf.json policy '" + session_config.policy + "' resolves through the factory");
        if (!policy) {
            policy = std::make_unique<bufman::LRUPolicy>();
        }
        mgr.init(session_config.pool_size, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(people_path, file, error);

        std::size_t first_pass[3] = {999, 999, 999};
        std::cout << "\nscan of " << people_path << " through the buffer pool:\n";
        std::uint64_t total = 0;
        for (std::uint64_t b = 0; b < 3; ++b) {
            std::size_t f = 999;
            mgr.pin(*file, b, f, error);
            first_pass[b] = f;
            const std::size_t count = bufman::record_count(mgr.frame(f).buffer);
            total += count;
            std::cout << "  block " << b << " (frame " << f << "): " << count << " record(s)\n";
            if (b < 2) {
                for (std::size_t s = 0; s < count; ++s) {
                    Person r{};
                    bufman::get_record(mgr.frame(f).buffer, s, r);
                    std::cout << "    slot " << s << ": pid=" << r.pid << ", name=" << r.name
                              << ", age=" << r.age << ", city=" << r.city << '\n';
                }
            }
            mgr.unpin(f, false, error);
        }
        std::cout << "  scanned 3 block(s), " << total << " record(s)\n";

        // "Same frames on the second pass" only means something when the
        // pool can hold all three blocks at once.
        if (session_config.pool_size >= 3) {
            bool same_frames = true;
            for (std::uint64_t b = 0; b < 3; ++b) {
                std::size_t f = 999;
                mgr.pin(*file, b, f, error);
                same_frames = same_frames && f == first_pass[b];
                mgr.unpin(f, false, error);
            }
            check(same_frames, "second scan pass hits the same frames (pool hits)");
        }
        mgr.close_all(error);
    }

    // --- alloc_page creates zeroed, dirty, pinned pages through the pool ---
    const std::string alloc_path = temp_path("-alloc.bin");
    std::remove(alloc_path.c_str());
    {
        // Pool of one: the second alloc_page must evict the dirty page.
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(alloc_path, file, error);
        Person p{};

        std::size_t f = 999;
        check(mgr.alloc_page(*file, 0, f, error) && f == 0, "alloc_page creates block 0 in frame 0");
        check(mgr.frame(f).dirty, "new page starts dirty");
        check(mgr.frame(f).pin_count == 1, "new page starts pinned");
        check(bufman::record_count(mgr.frame(f).buffer) == 0, "new page buffer is zeroed");

        std::size_t fdup = 999;
        check(!mgr.alloc_page(*file, 0, fdup, error),
              "alloc_page refuses an already resident block");

        check(bufman::put_record(mgr.frame(f).buffer, 0, make_person(7, "Gil", 28, "WA")),
              "record written into the new page");
        mgr.unpin(f, true, error);

        std::size_t fre = 999;
        check(mgr.alloc_page(*file, 1, fre, error) && fre == 0,
              "alloc_page evicts the dirty page and reuses its frame");
        check(!mgr.alloc_page(*file, 0, fre, error),
              "alloc_page refuses a block that already exists on disk");
        check(bufman::put_record(mgr.frame(fre).buffer, 0, make_person(8, "Hal", 35, "OH")),
              "record written into the second new page");
        mgr.unpin(fre, true, error);

        std::size_t fnext = 999;
        check(mgr.alloc_page(*file, 2, fnext, error) && fnext == 0,
              "alloc_page continues densely while earlier new pages sit in the pool");
        mgr.unpin(fnext, true, error);
        check(mgr.flush(fnext, error), "flush the third page so the disk grows to 3 blocks");

        std::size_t fgap = 999;
        check(!mgr.alloc_page(*file, 5, fgap, error),
              "alloc_page refuses to skip blocks (no gap past the end of file)");
        check(error == "block is not the next block after the end of file",
              "gap-allocation error message");

        std::size_t f3 = 999;
        check(mgr.alloc_page(*file, 3, f3, error) && f3 == 0,
              "alloc_page allows exactly the next block");
        mgr.unpin(f3, true, error);
        check(mgr.close_all(error), "close_all flushes the new pages and reports success");

        const auto disk0 = read_disk_block(alloc_path, 0);
        check(bufman::get_record(disk0.data(), 0, p) && p.pid == 7,
              "first new page was written back to disk");
        const auto disk1 = read_disk_block(alloc_path, 1);
        check(bufman::get_record(disk1.data(), 0, p) && p.pid == 8,
              "second new page was written back to disk");
        const auto disk3 = read_disk_block(alloc_path, 3);
        check(bufman::record_count(disk3.data()) == 0,
              "densely allocated block 3 exists on disk");
    }

    // --- Part records: 36-byte layout, slots, pool round-trips, CSV ---
    {
        check(bufman::kPartRecordSize == 36 && bufman::kPartsPerBlock == 113,
              "part layout math: 36-byte records, 113 per block");
        std::array<char, bufman::kPartBlockSize> block{};
        bufman::initialize_part_block(block.data());
        check(bufman::part_record_count(block.data()) == 0, "zeroed part block is empty");
        check(bufman::first_free_part_slot(block.data()) == std::optional<std::size_t>(0),
              "first free part slot of a zeroed block is 0");

        const Part bolt = make_part(1, "Bolt", 0.25f, 0, 1.99f, "steel");
        check(bufman::put_part_record(block.data(), 0, bolt), "put_part_record stores Bolt");
        check(!bufman::put_part_record(block.data(), 1,
                                       make_part(0, "Nobody", 1.0f, 0, 1.0f, "air")),
              "put_part_record rejects part_id 0 (free-slot sentinel)");
        check(!bufman::put_part_record(block.data(), bufman::kPartsPerBlock, bolt),
              "put_part_record rejects an out-of-range slot");

        Part read_back{};
        check(bufman::get_part_record(block.data(), 0, read_back) && read_back.part_id == 1 &&
                  std::string(read_back.part_name) == "Bolt" && read_back.part_weight == 0.25f &&
                  read_back.part_color == 0 && read_back.part_price == 1.99f &&
                  std::string(read_back.part_material) == "steel",
              "slot 0 reads back Bolt through the gap-free part layout");
        check(!bufman::get_part_record(block.data(), 1, read_back),
              "empty part slot 1 reports no record");
        check(bufman::part_record_count(block.data()) == 1, "part block holds 1 record");
    }
    {
        const std::string parts_path = temp_path("-parts.bin");
        std::remove(parts_path.c_str());
        bufman::BufferManager mgr;
        std::string error;
        mgr.init(1, std::make_unique<bufman::LRUPolicy>());
        bufman::File* file = nullptr;
        mgr.open_file(parts_path, file, error);

        std::size_t f = 999;
        check(mgr.alloc_page(*file, 0, f, error) && f == 0, "part block 0 allocated in frame 0");
        check(bufman::put_part_record(mgr.frame(f).buffer, 0,
                                      make_part(1, "Bolt", 0.25f, 0, 1.99f, "steel")) &&
                  bufman::put_part_record(mgr.frame(f).buffer, 1,
                                          make_part(2, "Nut", 0.5f, 3, 0.75f, "brass")),
              "two parts written into the new part block");
        mgr.unpin(f, true, error);
        std::size_t f2 = 999;
        check(mgr.alloc_page(*file, 1, f2, error), "part block 1 allocated");
        mgr.unpin(f2, true, error);
        check(mgr.close_all(error), "part pages flushed by close_all");

        bufman::File* reopened = nullptr;
        check(mgr.open_file_read(parts_path, reopened, error),
              "reopen the parts file after close_all (the old pointer is invalid now)");
        std::size_t fr = 999;
        check(mgr.pin(*reopened, 0, fr, error), "part block 0 re-pinned from disk");
        Part p{};
        check(bufman::get_part_record(mgr.frame(fr).buffer, 1, p) && p.part_id == 2 &&
                  std::string(p.part_name) == "Nut" && p.part_color == 3 &&
                  p.part_price == 0.75f && std::string(p.part_material) == "brass",
              "Nut survives write-back and reload through the pool");
        check(bufman::part_record_count(mgr.frame(fr).buffer) == 2,
              "reloaded part block holds 2 records");
        mgr.unpin(fr, false, error);
    }
    {
        const std::string csv_path = temp_path("-parts.csv");
        {
            std::ofstream out(csv_path);
            out << "part_id,part_name,part_weight,part_color,part_price,part_material\n";
            out << "1,Bolt,0.25,0,1.99,steel\n";
            out << "2,VeryLongName,0.5,1,2.5,brass\n";
            out << "3,Nut,0.5,9,2.5,brass\n";
            out << "4,Nut,-1,1,2.5,brass\n";
            out << "5,Nut,0.5,1,2.5\n";
        }
        std::ostringstream diagnostics;
        const bufman::PartLoadResult loaded = bufman::load_parts(csv_path, diagnostics);
        check(loaded.parts.size() == 1 && loaded.parts[0].part_id == 1 && loaded.skipped == 5,
              "part CSV keeps the one valid row and skips the header and four bad ones");
        check(!loaded.parts.empty() && loaded.parts[0].part_weight == 0.25f &&
                  loaded.parts[0].part_price == 1.99f,
              "part CSV parses the float fields");
        std::remove(csv_path.c_str());
    }

    // --- conf.json parsing and the policy factory ---
    {
        const std::string conf_path = temp_path("-conf.json");
        bufman::Config config;
        std::string error;
        const auto write_conf = [&conf_path](const std::string& text) {
            std::ofstream out(conf_path);
            out << text;
        };

        write_conf("{\n  \"policy\": \"lru\",\n  \"pool_size\": 10\n}\n");
        check(bufman::load_config(conf_path, config, std::cerr, error) &&
                  config.policy == "lru" && config.pool_size == 10,
              "conf.json parses policy and pool_size");

        write_conf("{ \"policy\": \"lru\", \"pool_size\": 0 }");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "'pool_size' must be >= 1",
              "pool_size 0 is rejected");

        write_conf("{ \"policy\": \"lru\", \"pool_size\": -2 }");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "expected a nonnegative integer",
              "negative pool_size is rejected");

        write_conf("{ \"policy\": \"lru\", \"pool_size\": 1.5 }");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "expected ',' or '}' after entry 'pool_size'",
              "non-integer pool_size is rejected");

        write_conf("{ \"pool_size\": 4 }");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "missing 'policy' entry",
              "missing 'policy' is rejected");

        write_conf("{ \"policy\": \"lru\" }");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "missing 'pool_size' entry",
              "missing 'pool_size' is rejected");

        write_conf("{ \"policy\": \"lru\", \"pool_size\": 2, \"pool_size\": 3 }");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "duplicate key 'pool_size'",
              "duplicate keys are rejected");

        write_conf("{ \"policy\": \"lru\", ");
        check(!bufman::load_config(conf_path, config, std::cerr, error),
              "truncated JSON is rejected");

        write_conf("{ \"policy\": \"lru\", \"pool_size\": 2 } extra\n");
        check(!bufman::load_config(conf_path, config, std::cerr, error) &&
                  error == "unexpected content after the closing '}'",
              "trailing garbage is rejected");

        {
            std::ostringstream diagnostics;
            write_conf("{ \"policy\": \"lru\", \"pool_size\": 2, \"mru_hint\": 5 }");
            check(bufman::load_config(conf_path, config, diagnostics, error) &&
                      config.pool_size == 2 &&
                      diagnostics.str().find("ignoring unknown key 'mru_hint'") !=
                          std::string::npos,
                  "unknown keys are skipped with a warning");
        }

        check(!bufman::PolicyFactory::instance().create("rand", error) &&
                  error ==
                      "unknown replacement policy 'rand' (available: fifo, lfu, lru, lruv2, mru)",
              "factory rejects an unknown policy by name");

        auto policy = bufman::PolicyFactory::instance().create("lru", error);
        check(policy != nullptr && error.empty(), "factory creates an LRU policy");

        // The created policy really evicts in LRU order through a manager.
        bufman::BufferManager mgr;
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);
        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        check(mgr.pin(*file, 2, fx, error) && fx == f1,
              "factory-built policy evicts the LRU frame");
        mgr.unpin(fx, false, error);
        mgr.close_all(error);

        const auto known = bufman::PolicyFactory::instance().known();
        check(known.size() == 5 && known[0] == "fifo" && known[1] == "lfu" &&
                  known[2] == "lru" && known[3] == "lruv2" && known[4] == "mru",
              "factory knows fifo, lfu, lru, lruv2 and mru");
        std::remove(conf_path.c_str());
    }

    // --- LRUv2Policy: the list + hash-map idiom ---
    {
        // Unit level: drive the hooks and read the victims straight off.
        bufman::LRUv2Policy policy;
        policy.init(4);
        policy.on_load(0);
        policy.on_load(1);
        policy.on_load(2);
        policy.on_access(0);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(1),
              "lruv2 picks the least recently accessed candidate");
        check(policy.pick_victim({2}) == std::optional<std::size_t>(2),
              "lruv2 picks the only candidate");
        check(policy.pick_victim({}) == std::nullopt,
              "lruv2 with no candidates reports no victim");
        policy.on_remove(1);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(2),
              "lruv2 forgets removed frames");
    }
    {
        // Equivalence: the same scripted hook sequence through both
        // implementations must produce identical victim choices — the hash
        // map is a drop-in for the vector-indexed variant.
        struct Step {
            char action;  // 'l' load, 'a' access, 'r' remove
            std::size_t frame;
            std::vector<std::size_t> candidates;  // pick after this step
        };
        const std::vector<Step> script = {
            {'l', 0, {}},
            {'l', 1, {}},
            {'l', 2, {}},
            {'a', 0, {}},
            {'l', 3, {1, 2, 3}},
            {'r', 1, {}},
            {'a', 2, {0, 2, 3}},
            {'l', 1, {0, 1, 3}},
            {'a', 3, {}},
            {'r', 0, {}},
            {'a', 1, {1, 2, 3}},
        };
        bufman::LRUPolicy lru;
        bufman::LRUv2Policy lruv2;
        lru.init(4);
        lruv2.init(4);
        std::vector<std::optional<std::size_t>> lru_picks;
        std::vector<std::optional<std::size_t>> lruv2_picks;
        for (const Step& step : script) {
            switch (step.action) {
                case 'l':
                    lru.on_load(step.frame);
                    lruv2.on_load(step.frame);
                    break;
                case 'a':
                    lru.on_access(step.frame);
                    lruv2.on_access(step.frame);
                    break;
                case 'r':
                    lru.on_remove(step.frame);
                    lruv2.on_remove(step.frame);
                    break;
            }
            if (!step.candidates.empty()) {
                lru_picks.push_back(lru.pick_victim(step.candidates));
                lruv2_picks.push_back(lruv2.pick_victim(step.candidates));
            }
        }
        check(lru_picks == lruv2_picks && lru_picks.size() == 4 &&
                  lru_picks[0] == std::optional<std::size_t>(1) &&
                  lru_picks[3] == std::optional<std::size_t>(2),
              "lru and lruv2 make identical victim choices");
    }
    {
        // Manager level: a factory-built lruv2 evicts in LRU order.
        std::string error;
        auto policy = bufman::PolicyFactory::instance().create("lruv2", error);
        check(policy != nullptr, "factory creates an LRUv2 policy");
        bufman::BufferManager mgr;
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);
        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        check(mgr.pin(*file, 2, fx, error) && fx == f1,
              "factory-built lruv2 evicts the LRU frame");
        mgr.unpin(fx, false, error);
        mgr.close_all(error);
    }

    // --- MRUPolicy: same recency list as LRU, victim from the other end ---
    {
        // Unit level: recency (MRU -> LRU) is 0, 2, 1 after these hooks —
        // the state where LRU picks 1, MRU picks 0.
        bufman::MRUPolicy policy;
        policy.init(4);
        policy.on_load(0);
        policy.on_load(1);
        policy.on_load(2);
        policy.on_access(0);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(0),
              "mru picks the most recently accessed candidate");
        check(policy.pick_victim({2}) == std::optional<std::size_t>(2),
              "mru picks the only candidate");
        check(policy.pick_victim({}) == std::nullopt,
              "mru with no candidates reports no victim");
        policy.on_remove(0);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(2),
              "mru forgets removed frames");
    }
    {
        // Manager level: same script as the lru/lruv2 checks, opposite
        // victim — after re-pinning block 0, mru evicts block 0's frame.
        std::string error;
        auto policy = bufman::PolicyFactory::instance().create("mru", error);
        check(policy != nullptr, "factory creates an MRU policy");
        bufman::BufferManager mgr;
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);
        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        check(mgr.pin(*file, 2, fx, error) && fx == f0,
              "factory-built mru evicts the most recently used frame");
        mgr.unpin(fx, false, error);
        mgr.close_all(error);
    }
    {
        // The reason MRU exists: two cycles of a scan over blocks 0..2 in
        // a two-frame pool. mru keeps pages cached across the cycle; lru
        // evicts each page just before the next cycle needs it.
        const auto cyclic_scan_hits =
            [&seeded_path](std::unique_ptr<bufman::ReplacementPolicy> policy) {
                bufman::BufferManager mgr;
                std::string error;
                mgr.init(2, std::move(policy));
                bufman::File* file = nullptr;
                mgr.open_file(seeded_path, file, error);
                int hits = 0;
                for (int cycle = 0; cycle < 2; ++cycle) {
                    for (std::uint64_t b = 0; b < 3; ++b) {
                        if (mgr.resident(*file, b)) {
                            ++hits;
                        }
                        std::size_t f = 999;
                        mgr.pin(*file, b, f, error);
                        mgr.unpin(f, false, error);
                    }
                }
                mgr.close_all(error);
                return hits;
            };
        const int mru_hits = cyclic_scan_hits(std::make_unique<bufman::MRUPolicy>());
        const int lru_hits = cyclic_scan_hits(std::make_unique<bufman::LRUPolicy>());
        check(mru_hits == 2 && lru_hits == 0,
              "cyclic scan: mru gets hits where lru gets none");
    }

    // --- LFUPolicy: frequency buckets, recency as the tie-break ---
    {
        // Unit level: counts after the script are 0 -> 3, 1 -> 2, 2 -> 1.
        bufman::LFUPolicy policy;
        policy.init(4);
        policy.on_load(0);
        policy.on_load(1);
        policy.on_load(2);
        policy.on_access(0);
        policy.on_access(0);
        policy.on_access(1);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(2),
              "lfu picks the least frequently accessed candidate");
        policy.on_remove(2);
        check(policy.pick_victim({0, 1}) == std::optional<std::size_t>(1),
              "lfu prefers the lower count among the survivors");
    }
    {
        // Tie-break: equal counts evict least-recently-used first, and a
        // second access lifts a frame out of the minimum bucket.
        bufman::LFUPolicy policy;
        policy.init(4);
        policy.on_load(5);
        policy.on_load(6);
        check(policy.pick_victim({5, 6}) == std::optional<std::size_t>(5),
              "lfu breaks count ties by least recent use");
        policy.on_access(5);
        check(policy.pick_victim({5, 6}) == std::optional<std::size_t>(6),
              "lfu promotes a twice-used frame out of the minimum bucket");
        check(policy.pick_victim({}) == std::nullopt,
              "lfu with no candidates reports no victim");
        policy.on_remove(6);
        check(policy.pick_victim({5, 6}) == std::optional<std::size_t>(5),
              "lfu forgets removed frames");
    }
    {
        // Manager level: block 0 pinned twice (count 2), block 1 once —
        // lfu evicts block 1's frame; plain lru would evict block 0's.
        std::string error;
        auto policy = bufman::PolicyFactory::instance().create("lfu", error);
        check(policy != nullptr, "factory creates an LFU policy");
        bufman::BufferManager mgr;
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);
        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        check(mgr.pin(*file, 2, fx, error) && fx == f1,
              "factory-built lfu evicts the least frequently used frame");
        mgr.unpin(fx, false, error);
        mgr.close_all(error);
    }
    {
        // Hot-page stability: a five-time-accessed page survives a scan
        // that flushes count-1 pages through the second frame.
        std::string error;
        auto policy = bufman::PolicyFactory::instance().create("lfu", error);
        bufman::BufferManager mgr;
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);
        std::size_t f = 999;
        for (int i = 0; i < 5; ++i) {
            mgr.pin(*file, 0, f, error);
            mgr.unpin(f, false, error);
        }
        mgr.pin(*file, 1, f, error);
        mgr.unpin(f, false, error);
        mgr.pin(*file, 2, f, error);
        mgr.unpin(f, false, error);
        check(mgr.resident(*file, 0), "hot page stays resident after the scan flood");
        mgr.pin(*file, 0, f, error);
        mgr.unpin(f, false, error);
        check(mgr.pin(*file, 1, f, error), "cold page reloads after eviction");
        mgr.unpin(f, false, error);
        mgr.close_all(error);
    }

    // --- FIFOPolicy: arrival queue, accesses change nothing ---
    {
        bufman::FIFOPolicy policy;
        policy.init(4);
        policy.on_load(0);
        policy.on_load(1);
        policy.on_load(2);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(0),
              "fifo picks the earliest arrival");
        policy.on_access(0);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(0),
              "fifo ignores accesses: a hit does not refresh arrival order");
        policy.on_remove(0);
        check(policy.pick_victim({0, 1, 2}) == std::optional<std::size_t>(1),
              "fifo forgets removed frames");
        policy.on_load(3);
        check(policy.pick_victim({0, 1, 2, 3}) == std::optional<std::size_t>(1),
              "fifo sends fresh arrivals to the back of the queue");
        check(policy.pick_victim({}) == std::nullopt,
              "fifo with no candidates reports no victim");
    }
    {
        // Manager level: same script as the lru check, opposite victim —
        // the re-pinned block 0 is still the oldest arrival, so fifo
        // evicts it while lru keeps it.
        std::string error;
        auto policy = bufman::PolicyFactory::instance().create("fifo", error);
        check(policy != nullptr, "factory creates a FIFO policy");
        bufman::BufferManager mgr;
        mgr.init(2, std::move(policy));
        bufman::File* file = nullptr;
        mgr.open_file(seeded_path, file, error);
        std::size_t f0 = 999, f1 = 999, fx = 999;
        mgr.pin(*file, 0, f0, error);
        mgr.pin(*file, 1, f1, error);
        mgr.unpin(f0, false, error);
        mgr.unpin(f1, false, error);
        mgr.pin(*file, 0, f0, error);
        mgr.unpin(f0, false, error);
        check(mgr.pin(*file, 2, fx, error) && fx == f0,
              "factory-built fifo evicts the oldest arrival despite the hit");
        mgr.unpin(fx, false, error);
        mgr.close_all(error);
    }
    {
        // Belady's anomaly: the classic reference string misses more with
        // four frames than with three. Only FIFO behaves this way.
        const auto fifo_misses = [&seeded_path](std::size_t pool_size) {
            const std::vector<std::uint64_t> refs = {0, 1, 2, 3,
                                                     0, 1, 4, 0, 1, 2, 3, 4};
            bufman::BufferManager mgr;
            std::string error;
            mgr.init(pool_size, std::make_unique<bufman::FIFOPolicy>());
            bufman::File* file = nullptr;
            mgr.open_file(seeded_path, file, error);
            int misses = 0;
            for (const std::uint64_t b : refs) {
                if (!mgr.resident(*file, b)) {
                    ++misses;
                }
                std::size_t f = 999;
                mgr.pin(*file, b, f, error);
                mgr.unpin(f, false, error);
            }
            mgr.close_all(error);
            return misses;
        };
        check(fifo_misses(3) == 9 && fifo_misses(4) == 10,
              "Belady's anomaly: fifo misses more with 4 frames than with 3");
    }

    std::remove(seeded_path.c_str());
    std::remove(dirty_path.c_str());
    std::remove(flush_path.c_str());
    std::remove(people_path.c_str());

    std::cout << (failures == 0 ? "\nAll checks passed." : "\nSome checks FAILED.")
              << "\n";
    return failures == 0 ? 0 : 1;
}
