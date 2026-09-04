#include <filesystem>
#include <print>
#include <string>

#include "liquid/repository.hpp"

namespace {

auto content(std::string value) -> std::vector<std::byte> {
  return {reinterpret_cast<const std::byte *>(value.data()),
          reinterpret_cast<const std::byte *>(value.data() + value.size())};
}

auto report_failure(const liquid::error &error) -> int {
  std::println("error: {} ({})", error.message, error.context);
  return 1;
}

} // namespace

int main() {
  const auto root = std::filesystem::current_path() / "repository-scenario";
  const auto child = std::filesystem::current_path() / "repository-scenario-child";
  std::error_code code;
  std::filesystem::remove_all(root, code);
  std::filesystem::remove_all(child, code);

  auto created = liquid::runtime::block_on(liquid::repository::create(root));
  if (!created)
    return report_failure(created.error());
  auto repository = std::move(*created);
  if (auto result =
          liquid::runtime::block_on(repository.write("notes/first.txt", content("first")));
      !result)
    return report_failure(result.error());
  if (auto result =
          liquid::runtime::block_on(repository.write("notes/second.txt", content("second")));
      !result)
    return report_failure(result.error());
  if (auto result =
          liquid::runtime::block_on(repository.move("notes/first.txt", "notes/renamed.txt"));
      !result)
    return report_failure(result.error());
  auto change = liquid::runtime::block_on(repository.cut("capture two files and rename one"));
  if (!change)
    return report_failure(change.error());
  auto split = liquid::runtime::block_on(repository.split("notes", child));
  if (!split)
    return report_failure(split.error());
  if (auto result = liquid::runtime::block_on(repository.nest(std::move(*split), "restored-notes"));
      !result)
    return report_failure(result.error());
  auto final_change = liquid::runtime::block_on(repository.cut("nest split repository"));
  if (!final_change)
    return report_failure(final_change.error());
  auto log = liquid::runtime::block_on(repository.log());
  if (!log)
    return report_failure(log.error());
  auto tree = liquid::runtime::block_on(repository.root());
  if (!tree)
    return report_failure(tree.error());
  auto refs = liquid::runtime::block_on(repository.refs());
  if (!refs)
    return report_failure(refs.error());

  std::println("created repository");
  std::println("cut: {}", liquid::to_string(change->value));
  std::println("split: notes -> restored-notes");
  std::println("head: {}", liquid::to_string(final_change->value));
  std::println("log:");
  for (const auto &entry : *log)
    std::println("  {}", entry.message);
  std::println("root:");
  for (const auto &entry : tree->entries)
    std::println("  {}", entry.name);
  std::println("refs:");
  for (const auto &entry : *refs)
    std::println("  {} {}", entry.name, liquid::to_string(entry.id.value));
  std::filesystem::remove_all(root, code);
  std::filesystem::remove_all(child, code);
  return 0;
}
