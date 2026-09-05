#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "encoding/cbor.hpp"
#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-symbols-test-" + std::to_string(next_++))) {}

    ~temporary_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view text) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto compiler_context(const std::filesystem::path& include_directory = {})
    -> std::vector<std::byte> {
    auto fields = liquid::encoding::cbor::value::map{};
    fields.emplace_back("compiler", LIQUID_CXX_COMPILER);
    if (!include_directory.empty()) {
        auto arguments = liquid::encoding::cbor::value::array{};
        arguments.emplace_back("-I" + include_directory.string());
        fields.emplace_back("arguments", std::move(arguments));
    }
    auto encoded = liquid::encoding::cbor::encode(liquid::encoding::cbor::value{std::move(fields)});
    EXPECT_TRUE(encoded) << encoded.error().message;
    return encoded.value_or(std::vector<std::byte>{});
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto record(liquid::frontend_module& module,
            std::string_view source,
            std::span<const std::byte> context = {}) -> liquid::symbol_record {
    const auto input = bytes(source);
    auto raw = module.symbols("c++", input, context);
    EXPECT_TRUE(raw) << raw.error().message;
    if (!raw) {
        return {};
    }
    const auto subject = liquid::hash(liquid::algorithm::blake3, input);
    EXPECT_TRUE(subject) << subject.error().message;
    if (!subject) {
        return {};
    }
    auto decoded = liquid::decode_frontend_symbols(*subject, *raw);
    EXPECT_TRUE(decoded) << decoded.error().message;
    if (!decoded) {
        return {};
    }
    return *decoded;
}

auto has_definition(const liquid::symbol_record& record, std::string_view name) -> bool {
    return std::ranges::any_of(record.occurrences, [&](const auto& occurrence) {
        return occurrence.name == name && occurrence.role == liquid::symbol_role::definition;
    });
}

auto first_reference(const liquid::symbol_record& record, std::string_view name)
    -> const liquid::symbol_occurrence* {
    const auto found = std::ranges::find_if(record.occurrences, [&](const auto& occurrence) {
        return occurrence.name == name && occurrence.role == liquid::symbol_role::reference;
    });
    return found == record.occurrences.end() ? nullptr : &*found;
}

TEST(Symbols, CompilerRecordResolvesAReferenceAcrossTranslationUnits) {
    auto module = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    ASSERT_TRUE(module);
    const auto context = compiler_context();
    const auto definition = record(*module, "int shared() { return 7; }\n", context);
    const auto reference =
        record(*module, "int shared();\nint use() { return shared(); }\n", context);
    ASSERT_TRUE(has_definition(definition, "shared"));
    const auto* use = first_reference(reference, "shared");
    ASSERT_NE(use, nullptr);
    EXPECT_EQ(use->symbol, "c++:shared");
    EXPECT_EQ(definition.origin, liquid::symbol_origin::compiler);

    temporary_repository location;
    auto repository = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(repository);
    auto stored = liquid::runtime::block_on(repository->store_symbols(definition));
    ASSERT_TRUE(stored);
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location.path()));
    ASSERT_TRUE(reopened);
    auto restored = liquid::runtime::block_on(reopened->symbols(definition.subject));
    ASSERT_TRUE(restored);
    EXPECT_EQ(liquid::to_string(restored->subject), liquid::to_string(definition.subject));
    EXPECT_EQ(restored->occurrences.size(), definition.occurrences.size());
}

TEST(Symbols, TreeSitterFallbackLabelsSyntacticResults) {
    auto module = liquid::frontend_module::load(LIQUID_TREE_SITTER_FRONTEND);
    ASSERT_TRUE(module);
    const auto value = record(*module, "int fallback() { return 1; }\n");
    EXPECT_EQ(value.origin, liquid::symbol_origin::syntactic);
    EXPECT_TRUE(has_definition(value, "fallback"));
}

TEST(Symbols, CompilerRecordMatchesTheCmakeFixture) {
    auto module = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    ASSERT_TRUE(module);
    const auto fixture = std::filesystem::path{LIQUID_FRONTEND_FIXTURES};
    const auto context = compiler_context(fixture);
    const auto functions = record(*module, read_text(fixture / "functions.cpp"), context);
    const auto header = record(*module, read_text(fixture / "value.hpp"), context);
    EXPECT_TRUE(has_definition(functions, "left"));
    EXPECT_TRUE(has_definition(functions, "right"));
    EXPECT_TRUE(has_definition(header, "value"));
    const auto* reference = first_reference(functions, "value");
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->symbol, "c++:value");
}

} // namespace
