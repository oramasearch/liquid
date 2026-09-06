#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>

#include "install/faults.hpp"
#include "liquid/install.hpp"

namespace {

class killing_injector final : public liquid::install_detail::fault_injector {
public:
    explicit killing_injector(std::uint64_t selected) : selected_(selected) {}

    void visit(liquid::install_detail::fault_event) override {
        ++current_;
        if (current_ == selected_) {
            std::raise(SIGKILL);
        }
    }

private:
    std::uint64_t selected_{};
    std::uint64_t current_{};
};

} // namespace

auto main(int argc, char** argv) -> int {
    if (argc != 4) {
        return 2;
    }
    const auto selected = std::strtoull(argv[3], nullptr, 10);
    auto injector = killing_injector{selected};
    liquid::install_detail::set_fault_injector(&injector);
    auto installed = liquid::runtime::block_on(liquid::installation::install(
        std::filesystem::path{argv[1]}, std::filesystem::path{argv[2]}));
    liquid::install_detail::set_fault_injector(nullptr);
    return installed ? 0 : 1;
}
