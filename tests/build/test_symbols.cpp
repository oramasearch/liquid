#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "encoding/cbor.hpp"
#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(liquid::test::fixture_path("liquid-symbols-test-" + std::to_string(next_++))) {}

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

auto field(const liquid::encoding::cbor::value& value, std::string_view name)
    -> const liquid::encoding::cbor::value* {
    const auto* object = std::get_if<liquid::encoding::cbor::value::map>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find_if(*object, [&](const auto& entry) {
        const auto* key = std::get_if<std::string>(&entry.first.data);
        return key && *key == name;
    });
    return found == object->end() ? nullptr : &found->second;
}

auto compiler_context() -> std::vector<std::byte> {
    auto fields = liquid::encoding::cbor::value::map{};
    fields.emplace_back("compile_action", "blake3:recorded-compile");
    auto encoded = liquid::encoding::cbor::encode(liquid::encoding::cbor::value{std::move(fields)});
    EXPECT_TRUE(encoded) << encoded.error().message;
    return encoded.value_or(std::vector<std::byte>{});
}

auto compiler_record(liquid::frontend_module& module,
                     std::string_view source,
                     std::string_view action_output) -> liquid::symbol_record {
    const auto input = bytes(source);
    auto raw = module.symbols_from_output("c++", input, bytes(action_output));
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

auto symbol_action(liquid::frontend_module& module) -> liquid::encoding::cbor::value {
    auto action = module.symbols("c++", bytes("int value;"), compiler_context());
    EXPECT_TRUE(action) << action.error().message;
    if (!action) {
        return {};
    }
    auto decoded = liquid::encoding::cbor::decode(*action);
    EXPECT_TRUE(decoded) << decoded.error().message;
    return decoded.value_or(liquid::encoding::cbor::value{});
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
    const auto definition = compiler_record(
        *module,
        "int shared() { return 7; }\n",
        R"({"kind":"TranslationUnitDecl","inner":[{"kind":"FunctionDecl","name":"shared","loc":{"offset":4},"range":{"end":{"offset":9,"tokLen":1}},"inner":[{"kind":"CompoundStmt"}]}]})");
    const auto reference = compiler_record(
        *module,
        "int shared();\nint use() { return shared(); }\n",
        R"({"kind":"TranslationUnitDecl","inner":[{"kind":"FunctionDecl","name":"use","loc":{"offset":18},"range":{"end":{"offset":20,"tokLen":1}},"inner":[{"kind":"CompoundStmt"},{"kind":"DeclRefExpr","loc":{"offset":33},"range":{"end":{"offset":33,"tokLen":6}},"referencedDecl":{"name":"shared"}}]}]})");
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
    const auto input = bytes("int fallback() { return 1; }\n");
    auto raw = module->symbols("c++", input);
    ASSERT_TRUE(raw);
    auto subject = liquid::hash(liquid::algorithm::blake3, input);
    ASSERT_TRUE(subject);
    auto decoded = liquid::decode_frontend_symbols(*subject, *raw);
    ASSERT_TRUE(decoded);
    const auto value = *decoded;
    EXPECT_EQ(value.origin, liquid::symbol_origin::syntactic);
    EXPECT_TRUE(value.grammar.starts_with("sha256:"));
    EXPECT_TRUE(has_definition(value, "fallback"));
}

TEST(Symbols, CompilerActionUsesTheRecordedCompileIdentity) {
    auto module = liquid::frontend_module::load(LIQUID_CLANG_FRONTEND);
    ASSERT_TRUE(module);
    const auto action = symbol_action(*module);
    const auto* kind = field(action, "kind");
    const auto* compile_action = field(action, "compile_action");
    ASSERT_NE(kind, nullptr);
    ASSERT_NE(compile_action, nullptr);
    EXPECT_EQ(std::get<std::string>(kind->data), "clang-symbols");
    EXPECT_EQ(std::get<std::string>(compile_action->data), "blake3:recorded-compile");
}

} // namespace
