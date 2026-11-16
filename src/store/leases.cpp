// SPDX-License-Identifier: MIT

#include "leases.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <random>
#include <span>
#include <system_error>

#include "backend.hpp"
#include "faults.hpp"
#include "pack.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> lease_magic{std::byte{'L'},
                                               std::byte{'Q'},
                                               std::byte{'L'},
                                               std::byte{'E'},
                                               std::byte{'A'},
                                               std::byte{'S'},
                                               std::byte{1},
                                               std::byte{0}};

error lease_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    for (unsigned index = 0; index != 2; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}

std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 2 > bytes.size())
        return 0;
    return std::uint16_t(std::to_integer<unsigned char>(bytes[at])) |
           (std::uint16_t(std::to_integer<unsigned char>(bytes[at + 1])) << 8U);
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 8 > bytes.size())
        return 0;
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + index])) << (8U * index);
    return value;
}

std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

bool valid_algorithm(unsigned char value) {
    return value == static_cast<unsigned char>(algorithm::blake3) ||
           value == static_cast<unsigned char>(algorithm::sha256) ||
           value == static_cast<unsigned char>(algorithm::blake3_keyed);
}

std::string random_id() {
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    std::string output(32, '0');
    for (auto& character : output)
        character = hex[random() & 15U];
    return output;
}

std::string record_key(const lease_record& record) {
    return "leases/lease-" + record.id + "-" + std::to_string(record.generation) + ".lease";
}

std::vector<std::byte> encode(const lease_record& record) {
    std::vector<std::byte> bytes(lease_magic.begin(), lease_magic.end());
    put_u16(bytes, static_cast<std::uint16_t>(record.id.size()));
    put_u16(bytes, static_cast<std::uint16_t>(record.owner.size()));
    bytes.push_back(std::byte{static_cast<unsigned char>(record.root.alg)});
    bytes.insert(bytes.end(), 7, std::byte{0});
    put_u64(bytes, record.generation);
    put_u64(bytes, static_cast<std::uint64_t>(record.expiry.time_since_epoch().count()));
    bytes.insert(bytes.end(), record.root.bytes.begin(), record.root.bytes.end());
    bytes.insert(bytes.end(),
                 reinterpret_cast<const std::byte*>(record.id.data()),
                 reinterpret_cast<const std::byte*>(record.id.data() + record.id.size()));
    bytes.insert(bytes.end(),
                 reinterpret_cast<const std::byte*>(record.owner.data()),
                 reinterpret_cast<const std::byte*>(record.owner.data() + record.owner.size()));
    put_u64(bytes, checksum(bytes));
    return bytes;
}

result<lease_record> decode(std::span<const std::byte> bytes, const std::filesystem::path& path) {
    if (bytes.size() < 76 || !std::equal(lease_magic.begin(), lease_magic.end(), bytes.begin()))
        return std::unexpected(lease_error(errc::corrupt, "invalid lease record", path));
    const auto id_size = get_u16(bytes, 8);
    const auto owner_size = get_u16(bytes, 10);
    const auto algorithm_byte = std::to_integer<unsigned char>(bytes[12]);
    if (!valid_algorithm(algorithm_byte) || bytes.size() != 76U + id_size + owner_size ||
        checksum(bytes.first(bytes.size() - 8)) != get_u64(bytes, bytes.size() - 8)) {
        return std::unexpected(lease_error(errc::corrupt, "corrupt lease record", path));
    }
    lease_record record;
    record.root.alg = static_cast<algorithm>(algorithm_byte);
    record.generation = get_u64(bytes, 20);
    record.expiry = std::chrono::sys_seconds{std::chrono::seconds{get_u64(bytes, 28)}};
    std::copy_n(bytes.begin() + 36, 32, record.root.bytes.begin());
    record.id.assign(reinterpret_cast<const char*>(bytes.data() + 68), id_size);
    record.owner.assign(reinterpret_cast<const char*>(bytes.data() + 68 + id_size), owner_size);
    record.key = path.generic_string();
    if (record.id.empty())
        return std::unexpected(lease_error(errc::corrupt, "lease id is empty", path));
    return record;
}

runtime::task<void> write_record(const std::shared_ptr<::liquid::store_backend>& backend,
                                 lease_record& record) {
    record.key = record_key(record);
    auto written = co_await backend->put(record.key, encode(record));
    if (!written)
        co_return std::unexpected(written.error());
    co_return co_await backend->barrier();
}

} // namespace

runtime::task<std::shared_ptr<lease_state>>
create_lease_until(std::shared_ptr<::liquid::store_backend> backend,
                   std::filesystem::path context,
                   digest root,
                   std::optional<std::chrono::sys_seconds> expiry,
                   std::string owner) {
    if (owner.size() > std::numeric_limits<std::uint16_t>::max()) {
        co_return std::unexpected(
            lease_error(errc::invalid_argument, "lease owner is too long", context));
    }
    auto state = std::make_shared<lease_state>();
    state->backend = std::move(backend);
    state->record.id = random_id();
    state->record.root = root;
    state->record.owner = owner.empty() ? "local:" + state->record.id : std::move(owner);
    state->record.generation = 1;
    state->record.expiry = expiry.value_or(std::chrono::sys_seconds::max());
    auto written = co_await write_record(state->backend, state->record);
    if (!written)
        co_return std::unexpected(written.error());
    co_return state;
}

runtime::task<void> renew_lease(std::shared_ptr<lease_state> state, std::chrono::seconds ttl) {
    if (!state || state->released)
        co_return std::unexpected(
            error{errc::invalid_argument, "lease is not active", "lease::renew"});
    if (ttl < std::chrono::seconds::zero()) {
        co_return std::unexpected(
            error{errc::invalid_argument, "lease ttl cannot be negative", "lease::renew"});
    }
    auto replacement = state->record;
    ++replacement.generation;
    replacement.expiry =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) + ttl;
    auto written = co_await write_record(state->backend, replacement);
    if (!written)
        co_return std::unexpected(written.error());
    auto removed = co_await state->backend->remove(state->record.key);
    if (!removed && removed.error().code != errc::not_found) {
        co_return std::unexpected(removed.error());
    }
    state->record = std::move(replacement);
    co_return result<void>{};
}

runtime::task<void> release_lease(std::shared_ptr<lease_state> state) {
    if (!state || state->released)
        co_return result<void>{};
    auto removed = co_await state->backend->remove(state->record.key);
    if (!removed && removed.error().code != errc::not_found) {
        co_return std::unexpected(removed.error());
    }
    state->released = true;
    co_return result<void>{};
}

void release_lease_now(const std::shared_ptr<lease_state>& state) noexcept {
    if (!state || state->released || !state->backend)
        return;
    try {
        if (const auto path = backend_local_path(state->backend, state->record.key)) {
            auto ignored = std::error_code{};
            std::filesystem::remove(*path, ignored);
            state->released = true;
            return;
        }
        runtime::spawn(release_lease(state), [](result<void>) {});
    } catch (...) {
        // The holder's end is detected on the next open, which collects the lease.
    }
}

runtime::task<std::vector<lease_record>>
load_leases(std::shared_ptr<::liquid::store_backend> backend, std::filesystem::path context) {
    std::vector<lease_record> records;
    try {
        auto entries = co_await backend->list("leases/");
        if (!entries) {
            co_return std::unexpected(entries.error());
        }
        for (const auto& entry : *entries) {
            if (!entry.key.ends_with(".lease"))
                continue;
            auto bytes = co_await read_backend_object(backend, entry.key);
            // Another process renews and releases leases while this one lists them: a lease
            // renewed or released between the listing and the read is simply no longer there
            // (liquid #526).
            if (!bytes && bytes.error().code == errc::not_found)
                continue;
            if (!bytes)
                co_return std::unexpected(bytes.error());
            auto decoded = decode(*bytes, entry.key);
            if (!decoded)
                co_return std::unexpected(decoded.error());
            records.push_back(std::move(*decoded));
        }
        std::sort(records.begin(),
                  records.end(),
                  [](const lease_record& left, const lease_record& right) {
                      if (left.id != right.id)
                          return left.id < right.id;
                      return left.generation > right.generation;
                  });
        records.erase(std::unique(records.begin(),
                                  records.end(),
                                  [](const lease_record& left, const lease_record& right) {
                                      return left.id == right.id;
                                  }),
                      records.end());
        co_return records;
    } catch (const std::system_error& caught) {
        co_return std::unexpected(lease_error(errc::io, caught.what(), context));
    }
}

} // namespace liquid::store_detail
