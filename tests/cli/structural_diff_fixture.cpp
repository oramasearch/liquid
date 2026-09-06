#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto fail(const liquid::error& error) -> int {
    std::cerr << error.message << " (" << error.context << ")\n";
    return 1;
}

auto write(liquid::repository& repository, std::string path, std::string_view value)
    -> liquid::result<void> {
    return liquid::runtime::block_on(repository.write(std::move(path), bytes(value)));
}

auto measurement_repository(std::filesystem::path repository_path,
                            const std::filesystem::path& before_root,
                            const std::filesystem::path& after_root) -> int {
    auto created = liquid::runtime::block_on(liquid::repository::create(repository_path));
    if (!created) {
        return fail(created.error());
    }
    auto repository = std::move(*created);
    auto imported = liquid::runtime::block_on(repository.import_tree(before_root));
    if (!imported) {
        return fail(imported.error());
    }
    auto before = liquid::runtime::block_on(repository.cut("before"));
    if (!before) {
        return fail(before.error());
    }
    auto referenced = liquid::runtime::block_on(repository.set_ref("before", *before));
    if (!referenced) {
        return fail(referenced.error());
    }
    imported = liquid::runtime::block_on(repository.import_tree(after_root));
    if (!imported) {
        return fail(imported.error());
    }
    auto after = liquid::runtime::block_on(repository.cut("after"));
    if (!after) {
        return fail(after.error());
    }
    referenced = liquid::runtime::block_on(repository.set_ref("after", *after));
    return referenced ? 0 : fail(referenced.error());
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4) {
        return measurement_repository(argv[1], argv[2], argv[3]);
    }
    if (argc != 2) {
        std::cerr << "usage: structural-diff-fixture <repository> [<before> <after>]\n";
        return 2;
    }
    auto created = liquid::runtime::block_on(liquid::repository::create(argv[1]));
    if (!created) {
        return fail(created.error());
    }
    auto repository = std::move(*created);
    auto written = write(repository,
                         "functions.c",
                         "int alpha(void) { return 1; }\n"
                         "int beta(void) { return 2; }\n");
    if (!written) {
        return fail(written.error());
    }
    written = write(repository, "format.c", "int answer(void) { return 42; }\n");
    if (!written) {
        return fail(written.error());
    }
    written = write(repository, "notes.abc", "before\n");
    if (!written) {
        return fail(written.error());
    }
    auto before = liquid::runtime::block_on(repository.cut("before"));
    if (!before) {
        return fail(before.error());
    }
    auto referenced = liquid::runtime::block_on(repository.set_ref("before", *before));
    if (!referenced) {
        return fail(referenced.error());
    }
    written = write(repository,
                    "functions.c",
                    "int beta(void) { return 2; }\n"
                    "int alpha(void) { return 1; }\n");
    if (!written) {
        return fail(written.error());
    }
    written = write(repository,
                    "format.c",
                    "int answer(void)\n"
                    "{\n"
                    "    return 42;\n"
                    "}\n");
    if (!written) {
        return fail(written.error());
    }
    written = write(repository, "notes.abc", "after\n");
    if (!written) {
        return fail(written.error());
    }
    auto after = liquid::runtime::block_on(repository.cut("after"));
    if (!after) {
        return fail(after.error());
    }
    referenced = liquid::runtime::block_on(repository.set_ref("after", *after));
    if (!referenced) {
        return fail(referenced.error());
    }
    return 0;
}
