#include "harness.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
#include <random>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "faults.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

namespace liquid::sim {
namespace {

using store_detail::fault_event;
using store_detail::fault_injector;
using store_detail::fault_operation;
using store_detail::fault_phase;

class temporary_directory {
public:
    explicit temporary_directory(std::filesystem::path path) : path_(std::move(path)) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~temporary_directory() {
        std::filesystem::remove_all(path_);
    }
    const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class injector_guard {
public:
    explicit injector_guard(fault_injector* injector)
        : previous_(store_detail::set_fault_injector(injector)) {}
    ~injector_guard() {
        store_detail::set_fault_injector(previous_);
    }
    injector_guard(const injector_guard&) = delete;
    injector_guard& operator=(const injector_guard&) = delete;

private:
    fault_injector* previous_{};
};

class counting_injector final : public fault_injector {
public:
    void visit(fault_event) override {
        count_.fetch_add(1, std::memory_order_relaxed);
    }
    std::uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> count_{};
};

class simulated_crash final : public std::exception {
public:
    const char* what() const noexcept override {
        return "simulated store crash";
    }
};

class crashing_injector final : public fault_injector {
public:
    explicit crashing_injector(std::uint64_t target) : target_(target) {}

    void visit(fault_event event) override {
        const auto step = step_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (step != target_)
            return;
        fired_.store(true, std::memory_order_relaxed);
        operation_ = event.operation;
        phase_ = event.phase;
        path_ = event.path.string();
        throw simulated_crash{};
    }

    bool fired() const noexcept {
        return fired_.load(std::memory_order_relaxed);
    }
    std::string description() const {
        const std::string_view operation = operation_ == fault_operation::write    ? "write"
                                           : operation_ == fault_operation::rename ? "rename"
                                                                                   : "remove";
        const std::string_view phase = phase_ == fault_phase::before ? "before" : "after";
        return std::string(phase) + " " + std::string(operation) + " " + path_;
    }

private:
    std::uint64_t target_{};
    std::atomic<std::uint64_t> step_{};
    std::atomic<bool> fired_{};
    fault_operation operation_{};
    fault_phase phase_{};
    std::string path_;
};

std::vector<std::byte> make_bytes(std::size_t size, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    std::vector<std::byte> output(size);
    for (auto& value : output)
        value = std::byte{static_cast<unsigned char>(random() & 0xffU)};
    return output;
}

std::size_t input_size(std::mt19937_64& random) {
    const auto bucket = random() % 20;
    if (bucket == 0) {
        return 256U * 1024U +
               static_cast<std::size_t>(random() % (4U * 1024U * 1024U - 256U * 1024U + 1));
    }
    if (bucket < 5) {
        return 4U * 1024U + static_cast<std::size_t>(random() % (256U * 1024U - 4U * 1024U + 1));
    }
    return 40U + static_cast<std::size_t>(random() % (4U * 1024U - 40U + 1));
}

result<store> open_store(const std::filesystem::path& path) {
    store_options options;
    options.pack_size = 64U * 1024U;
    return runtime::block_on(store::open(path, options));
}

enum class operation_kind {
    put,
    put_orphan,
    get_known,
    get_unknown,
    read,
    reopen,
    collect,
    collect_without_compaction,
    compact,
    renew,
};

struct operation {
    operation_kind kind{};
    std::vector<std::byte> bytes;
    std::uint64_t choice{};
};

struct expected_object {
    digest id{};
    std::vector<std::byte> bytes;
};

using object_map = std::map<std::string, expected_object>;
using lease_map = std::map<std::string, lease>;

std::optional<simulation_failure>
failure(std::uint64_t seed, std::uint64_t step, std::string message) {
    return simulation_failure{seed, step, std::move(message)};
}

std::optional<simulation_failure> open_again(store& subject,
                                             const std::filesystem::path& path,
                                             std::uint64_t seed,
                                             std::uint64_t step) {
    subject = {};
    auto reopened = open_store(path);
    if (!reopened)
        return failure(seed, step, "reopen failed: " + reopened.error().message);
    subject = std::move(*reopened);
    return std::nullopt;
}

std::optional<simulation_failure> run_schedule(store& subject,
                                               const std::filesystem::path& path,
                                               const std::vector<operation>& schedule,
                                               object_map& acknowledged,
                                               lease_map& roots,
                                               std::vector<expected_object>& attempted,
                                               std::uint64_t seed,
                                               std::uint64_t fault_step,
                                               crashing_injector* crash) {
    for (const auto& item : schedule) {
        if (item.kind == operation_kind::put || item.kind == operation_kind::put_orphan) {
            auto id = hash(algorithm::blake3, item.bytes);
            if (!id)
                return failure(
                    seed, fault_step, "could not hash scheduled input: " + id.error().message);
            attempted.push_back({*id, item.bytes});
            auto put = runtime::block_on(subject.put(item.bytes));
            if (crash && crash->fired())
                return std::nullopt;
            if (!put)
                return failure(seed, fault_step, "put failed: " + put.error().message);
            if (item.kind == operation_kind::put_orphan)
                continue;
            auto root = runtime::block_on(subject.lease_root(*put, std::chrono::hours{1}));
            if (crash && crash->fired())
                return std::nullopt;
            if (!root)
                return failure(seed, fault_step, "lease failed: " + root.error().message);
            acknowledged.emplace(to_string(*put), expected_object{*put, item.bytes});
            roots.insert_or_assign(to_string(*put), std::move(*root));
            continue;
        }
        if (item.kind == operation_kind::collect ||
            item.kind == operation_kind::collect_without_compaction ||
            item.kind == operation_kind::compact) {
            collection_options options;
            if (item.kind == operation_kind::collect_without_compaction) {
                options.compact_below = 0.0;
            }
            if (item.kind == operation_kind::compact) {
                options.compact_only = true;
            }
            auto collected = runtime::block_on(subject.collect(options));
            if (crash && crash->fired())
                return std::nullopt;
            if (!collected)
                return failure(seed, fault_step, "collection failed: " + collected.error().message);
            continue;
        }
        if (item.kind == operation_kind::renew) {
            auto selected = std::next(roots.begin(), item.choice % roots.size());
            auto renewed = runtime::block_on(selected->second.renew(std::chrono::hours{1}));
            if (crash && crash->fired())
                return std::nullopt;
            if (!renewed)
                return failure(
                    seed, fault_step, "lease renewal failed: " + renewed.error().message);
            continue;
        }
        if (item.kind == operation_kind::reopen) {
            auto reopened = open_again(subject, path, seed, fault_step);
            if (crash && crash->fired())
                return std::nullopt;
            if (reopened)
                return reopened;
            continue;
        }
        if (item.kind == operation_kind::get_unknown) {
            auto id = hash(algorithm::blake3, item.bytes);
            if (!id)
                return failure(
                    seed, fault_step, "could not hash unknown input: " + id.error().message);
            auto got = runtime::block_on(subject.get(*id));
            if (got || got.error().code != errc::not_found)
                return failure(seed, fault_step, "unknown digest did not return not_found");
            continue;
        }
        const auto selected =
            std::next(acknowledged.begin(), item.choice % acknowledged.size())->second;
        if (item.kind == operation_kind::get_known) {
            auto got = runtime::block_on(subject.get(selected.id));
            if (!got)
                return failure(
                    seed, fault_step, "known digest is unreadable: " + got.error().message);
            if (*got != selected.bytes)
                return failure(seed, fault_step, "known digest returned wrong bytes");
            continue;
        }
        const auto offset = item.choice % (selected.bytes.size() + 1);
        const auto available = selected.bytes.size() - offset;
        const auto wanted = std::min<std::size_t>(available, 1 + item.bytes.size() % 4096U);
        std::vector<std::byte> range(wanted);
        auto read = runtime::block_on(subject.read(selected.id, offset, range));
        if (!read)
            return failure(seed, fault_step, "range read failed: " + read.error().message);
        if (*read != wanted ||
            !std::equal(range.begin(), range.end(), selected.bytes.begin() + offset))
            return failure(seed, fault_step, "range read returned wrong bytes");
    }
    return std::nullopt;
}

std::vector<operation> make_schedule(std::mt19937_64& random,
                                     const std::vector<std::byte>& initial) {
    std::vector<operation> schedule;
    std::vector<std::vector<std::byte>> reusable{initial};
    schedule.reserve(12);
    schedule.push_back({operation_kind::put, initial, random()});
    for (unsigned index = 0; index != 3; ++index) {
        schedule.push_back({operation_kind::put_orphan, make_bytes(4096, random()), random()});
    }
    schedule.push_back({operation_kind::put, make_bytes(4096, random()), random()});
    schedule.push_back({operation_kind::collect_without_compaction, {}, random()});
    schedule.push_back({operation_kind::compact, {}, random()});
    for (unsigned position = 0; position != 5; ++position) {
        const auto choice = random();
        if (choice % 3 == 0) {
            std::vector<std::byte> bytes;
            if (reusable.size() > 1 && choice % 4 == 0) {
                bytes = reusable[choice % reusable.size()];
            } else {
                bytes = make_bytes(input_size(random), random());
                reusable.push_back(bytes);
            }
            schedule.push_back({operation_kind::put, std::move(bytes), choice});
            continue;
        }
        const auto kind = choice % 7;
        if (kind == 0) {
            schedule.push_back({operation_kind::get_known, {}, choice});
        } else if (kind == 1) {
            schedule.push_back({operation_kind::read, make_bytes(64, random()), choice});
        } else if (kind == 2) {
            schedule.push_back({operation_kind::reopen, {}, choice});
        } else if (kind == 3) {
            schedule.push_back({operation_kind::get_unknown, make_bytes(73, random()), choice});
        } else if (kind == 4) {
            schedule.push_back({operation_kind::collect, {}, choice});
        } else if (kind == 5) {
            schedule.push_back({operation_kind::renew, {}, choice});
        } else {
            schedule.push_back(
                {operation_kind::put_orphan, make_bytes(input_size(random), random()), choice});
        }
    }
    return schedule;
}

} // namespace

simulation_result run_store_simulation(std::uint64_t seed, const std::filesystem::path& workspace) {
    std::mt19937_64 random(seed);
    const auto initial = make_bytes(40U + random() % 4057U, random());
    const auto schedule = make_schedule(random, initial);

    temporary_directory probe(workspace / ("probe-" + std::to_string(seed)));
    auto probe_opened = open_store(probe.path());
    if (!probe_opened)
        return {0,
                0,
                failure(seed, 0, "could not open counter store: " + probe_opened.error().message)};
    auto initial_id = runtime::block_on(probe_opened->put(initial));
    if (!initial_id)
        return {0,
                0,
                failure(seed, 0, "could not prepare counter store: " + initial_id.error().message)};
    object_map probe_acknowledged{{to_string(*initial_id), {*initial_id, initial}}};
    auto probe_root =
        runtime::block_on(probe_opened->lease_root(*initial_id, std::chrono::hours{1}));
    if (!probe_root)
        return {
            0, 0, failure(seed, 0, "could not lease counter root: " + probe_root.error().message)};
    lease_map probe_roots;
    probe_roots.emplace(to_string(*initial_id), std::move(*probe_root));
    std::vector<expected_object> probe_attempted;
    counting_injector counter;
    {
        injector_guard guard(&counter);
        if (auto failed = run_schedule(*probe_opened,
                                       probe.path(),
                                       schedule,
                                       probe_acknowledged,
                                       probe_roots,
                                       probe_attempted,
                                       seed,
                                       0,
                                       nullptr)) {
            return {0, 0, std::move(failed)};
        }
    }
    *probe_opened = {};
    if (counter.count() == 0)
        return {0, 0, failure(seed, 0, "counter run observed no fault points")};

    const auto fault_step = 1U + random() % counter.count();
    temporary_directory actual(workspace / ("actual-" + std::to_string(seed)));
    auto opened = open_store(actual.path());
    if (!opened)
        return {fault_step,
                0,
                failure(seed,
                        fault_step,
                        "could not open simulation store: " + opened.error().message)};
    initial_id = runtime::block_on(opened->put(initial));
    if (!initial_id)
        return {fault_step,
                0,
                failure(seed,
                        fault_step,
                        "could not prepare simulation store: " + initial_id.error().message)};
    object_map acknowledged{{to_string(*initial_id), {*initial_id, initial}}};
    auto actual_root = runtime::block_on(opened->lease_root(*initial_id, std::chrono::hours{1}));
    if (!actual_root)
        return {fault_step,
                0,
                failure(seed,
                        fault_step,
                        "could not lease simulation root: " + actual_root.error().message)};
    lease_map roots;
    roots.emplace(to_string(*initial_id), std::move(*actual_root));
    std::vector<expected_object> attempted;
    crashing_injector crash(fault_step);
    {
        injector_guard guard(&crash);
        if (auto failed = run_schedule(*opened,
                                       actual.path(),
                                       schedule,
                                       acknowledged,
                                       roots,
                                       attempted,
                                       seed,
                                       fault_step,
                                       &crash)) {
            return {fault_step, 0, std::move(failed)};
        }
    }
    if (!crash.fired())
        return {fault_step, 0, failure(seed, fault_step, "selected fault point was not reached")};
    *opened = {};

    auto reopened = open_store(actual.path());
    if (!reopened) {
        return {fault_step,
                0,
                failure(seed,
                        fault_step,
                        "reopen failed after " + crash.description() + ": " +
                            reopened.error().message)};
    }
    for (const auto& entry : std::filesystem::directory_iterator(actual.path() / "gc")) {
        if (entry.path().filename().string().starts_with("compact-")) {
            return {fault_step,
                    0,
                    failure(seed, fault_step, "compaction recovery left a pending operation")};
        }
    }
    for (const auto& [unused, expected] : acknowledged) {
        auto got = runtime::block_on(reopened->get(expected.id));
        if (!got) {
            return {fault_step,
                    0,
                    failure(seed,
                            fault_step,
                            "acknowledged put is unreadable after " + crash.description() + ": " +
                                got.error().message)};
        }
        if (*got != expected.bytes) {
            return {fault_step,
                    0,
                    failure(seed,
                            fault_step,
                            "acknowledged put returned wrong bytes after " + crash.description())};
        }
    }

    std::uint64_t orphans = 0;
    for (const auto& expected : attempted) {
        if (acknowledged.contains(to_string(expected.id)))
            continue;
        auto got = runtime::block_on(reopened->get(expected.id));
        if (!got) {
            if (got.error().code != errc::not_found && got.error().code != errc::corrupt) {
                return {fault_step,
                        orphans,
                        failure(seed,
                                fault_step,
                                "interrupted put returned an unexpected error: " +
                                    got.error().message)};
            }
            continue;
        }
        if (*got != expected.bytes) {
            return {fault_step,
                    orphans,
                    failure(seed,
                            fault_step,
                            "orphan returned wrong bytes after " + crash.description())};
        }
        ++orphans;
    }

    const auto unknown = hash(algorithm::blake3, make_bytes(79, seed ^ 0x5a5a5a5a5a5a5a5aULL));
    if (!unknown)
        return {fault_step, orphans, failure(seed, fault_step, "could not hash unknown digest")};
    auto missing = runtime::block_on(reopened->get(*unknown));
    if (missing || missing.error().code != errc::not_found) {
        return {fault_step,
                orphans,
                failure(seed, fault_step, "unknown digest did not return not_found after reopen")};
    }

    *reopened = {};
    std::error_code removed_index;
    std::filesystem::remove_all(actual.path() / "index", removed_index);
    if (removed_index) {
        return {fault_step,
                orphans,
                failure(seed,
                        fault_step,
                        "could not remove index for rebuild: " + removed_index.message())};
    }
    reopened = open_store(actual.path());
    if (!reopened) {
        return {fault_step,
                orphans,
                failure(seed,
                        fault_step,
                        "index rebuild failed after " + crash.description() + ": " +
                            reopened.error().message)};
    }
    for (const auto& [unused, expected] : acknowledged) {
        auto got = runtime::block_on(reopened->get(expected.id));
        if (!got || *got != expected.bytes) {
            return {
                fault_step,
                orphans,
                failure(seed,
                        fault_step,
                        "index rebuild lost an acknowledged object after " + crash.description())};
        }
    }

    auto usable = runtime::block_on(reopened->put(make_bytes(83, seed ^ 0xa5a5a5a5a5a5a5a5ULL)));
    if (!usable)
        return {fault_step,
                orphans,
                failure(seed,
                        fault_step,
                        "store is not usable after reopen: " + usable.error().message)};
    return {fault_step, orphans, std::nullopt};
}

} // namespace liquid::sim
