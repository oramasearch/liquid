#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "liquid/store.hpp"
#include "store/chunker.hpp"

namespace {

class mapped_file {
public:
    explicit mapped_file(const std::filesystem::path& path) {
        descriptor_ = ::open(path.c_str(), O_RDONLY);
        if (descriptor_ < 0) throw std::runtime_error(std::strerror(errno));
        struct stat status {};
        if (::fstat(descriptor_, &status) != 0) {
            const std::string message = std::strerror(errno);
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error(message);
        }
        size_ = static_cast<std::size_t>(status.st_size);
        if (size_ != 0) {
            data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, descriptor_, 0);
            if (data_ == MAP_FAILED) {
                const std::string message = std::strerror(errno);
                ::close(descriptor_);
                descriptor_ = -1;
                throw std::runtime_error(message);
            }
#ifdef MADV_SEQUENTIAL
            ::madvise(data_, size_, MADV_SEQUENTIAL);
#endif
        }
    }

    ~mapped_file() {
        if (data_ && data_ != MAP_FAILED) ::munmap(data_, size_);
        if (descriptor_ >= 0) ::close(descriptor_);
    }

    mapped_file(const mapped_file&) = delete;
    mapped_file& operator=(const mapped_file&) = delete;

    std::span<const std::byte> bytes() const {
        return {static_cast<const std::byte*>(data_), size_};
    }

private:
    int descriptor_{-1};
    void* data_{};
    std::size_t size_{};
};

std::vector<std::filesystem::path> collect_files(int argc, char** argv) {
    std::vector<std::filesystem::path> files;
    for (int index = 2; index < argc; ++index) {
        const std::filesystem::path input(argv[index]);
        if (std::filesystem::is_regular_file(input)) {
            files.push_back(input);
        } else if (std::filesystem::is_directory(input)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(input)) {
                if (entry.is_regular_file()) files.push_back(entry.path());
            }
        } else {
            throw std::runtime_error("input is not a regular file or directory: " + input.string());
        }
    }
    return files;
}

std::uint64_t pack_bytes(const std::filesystem::path& store_root) {
    std::uint64_t total = 0;
    for (const auto& entry : std::filesystem::directory_iterator(store_root / "packs")) {
        if (entry.path().extension() == ".pack") total += entry.file_size();
    }
    return total;
}

std::pair<std::uint64_t, std::uint64_t> unique_chunks(
    const std::filesystem::path& store_root) {
    std::uint64_t count = 0;
    std::uint64_t bytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(store_root / "packs")) {
        if (entry.path().extension() != ".table") continue;
        std::ifstream input(entry.path(), std::ios::binary);
        std::vector<unsigned char> table(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (table.size() < 8 || (table.size() - 8) % 49 != 0) {
            throw std::runtime_error("malformed pack table: " + entry.path().string());
        }
        for (std::size_t offset = 8; offset < table.size(); offset += 49) {
            std::uint32_t raw_size = 0;
            for (std::size_t index = 0; index < sizeof(raw_size); ++index) {
                raw_size |= static_cast<std::uint32_t>(table[offset + 45 + index]) << (index * 8U);
            }
            ++count;
            bytes += raw_size;
        }
    }
    return {count, bytes};
}

std::uint64_t peak_rss_bytes() {
    struct rusage usage {};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) throw std::runtime_error(std::strerror(errno));
#ifdef __APPLE__
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

std::pair<std::uint64_t, std::uint64_t> content_unique_chunks(
    const std::vector<std::filesystem::path>& files) {
    std::set<std::string> unique;
    std::uint64_t bytes = 0;
    for (const auto& file : files) {
        mapped_file input(file);
        for (const auto& range : liquid::store_detail::chunk(input.bytes())) {
            auto id = liquid::hash(
                liquid::algorithm::blake3, input.bytes().subspan(range.offset, range.length));
            if (!id) throw std::runtime_error(id.error().message);
            if (unique.insert(liquid::to_string(*id)).second) bytes += range.length;
        }
    }
    return {unique.size(), bytes};
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::cerr << "usage: liquid-store-measure STORE INPUT...\n";
            return 2;
        }
        const std::filesystem::path store_root(argv[1]);
        const auto files = collect_files(argc, argv);
        liquid::store_options options;
        options.index_cache_size = 64 * 1024 * 1024;
        auto store = liquid::store::open(store_root, options);
        if (!store) throw std::runtime_error(store.error().message);

        std::uint64_t input_bytes = 0;
        const auto started = std::chrono::steady_clock::now();
        for (const auto& file : files) {
            mapped_file input(file);
            input_bytes += input.bytes().size();
            auto stored = store->put(input.bytes());
            if (!stored) throw std::runtime_error(stored.error().message + ": " + stored.error().context);
        }
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const auto stored_bytes = pack_bytes(store_root);
        const auto [unique_chunk_count, unique_chunk_bytes] = unique_chunks(store_root);
        const bool measure_content_dedup = input_bytes <= 2ULL * 1024ULL * 1024ULL * 1024ULL;
        const auto content_unique = measure_content_dedup
                                        ? content_unique_chunks(files)
                                        : std::pair<std::uint64_t, std::uint64_t>{};
        const auto mib_per_second = elapsed == 0.0
                                        ? 0.0
                                        : static_cast<double>(input_bytes) / (1024.0 * 1024.0) / elapsed;
        std::cout << "files=" << files.size() << '\n'
                  << "input_bytes=" << input_bytes << '\n'
                  << "pack_bytes=" << stored_bytes << '\n'
                  << "pack_ratio="
                  << (input_bytes == 0 ? 0.0 : static_cast<double>(stored_bytes) / input_bytes) << '\n'
                  << "unique_chunks=" << unique_chunk_count << '\n'
                  << "unique_chunk_bytes=" << unique_chunk_bytes << '\n'
                  << "raw_dedup_ratio="
                  << (input_bytes == 0 ? 0.0 : static_cast<double>(unique_chunk_bytes) / input_bytes) << '\n'
                  << "content_unique_chunks=";
        if (measure_content_dedup) {
            std::cout << content_unique.first << '\n'
                      << "content_unique_bytes=" << content_unique.second << '\n'
                      << "content_dedup_ratio="
                      << (input_bytes == 0 ? 0.0 : static_cast<double>(content_unique.second) / input_bytes)
                      << '\n';
        } else {
            std::cout << "not_measured\ncontent_unique_bytes=not_measured\n"
                      << "content_dedup_ratio=not_measured\n";
        }
        std::cout
                  << "elapsed_seconds=" << elapsed << '\n'
                  << "throughput_mib_s=" << mib_per_second << '\n'
                  << "peak_rss_bytes=" << peak_rss_bytes() << '\n';
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
