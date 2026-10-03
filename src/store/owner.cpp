#include "owner.hpp"

#include <atomic>
#include <filesystem>
#include <utility>

#include "owner_lock.hpp"

namespace liquid {

struct owner_grant::state {
    explicit state(std::uint64_t fence) : fence(fence) {}

    std::uint64_t fence{};
    std::atomic<bool> lost{false};
};

owner_grant::owner_grant() noexcept = default;

owner_grant::owner_grant(std::uint64_t fence) : state_(std::make_shared<state>(fence)) {}

bool owner_grant::valid() const noexcept {
    return state_ != nullptr;
}

std::uint64_t owner_grant::fence() const noexcept {
    return state_ ? state_->fence : 0;
}

bool owner_grant::lost() const noexcept {
    return !state_ || state_->lost.load(std::memory_order_acquire);
}

void owner_grant::revoke() const noexcept {
    if (state_)
        state_->lost.store(true, std::memory_order_release);
}

namespace {

class disk_store_owner final : public store_owner {
public:
    explicit disk_store_owner(std::filesystem::path root) : path_(std::move(root) / "store.lock") {}

    runtime::task<owner_grant> acquire(std::string, std::chrono::milliseconds wait) override {
        if (lock_.held()) {
            co_return std::unexpected(error{
                errc::invalid_argument, "the disk store owner is already held", path_.string()});
        }
        auto acquired = co_await store_detail::owner_lock::acquire(path_, wait);
        if (!acquired)
            co_return std::unexpected(acquired.error());
        lock_ = std::move(*acquired);
        co_return owner_grant{0};
    }

    runtime::task<void> release() override {
        lock_.release();
        co_return result<void>{};
    }

private:
    std::filesystem::path path_;
    store_detail::owner_lock lock_;
};

runtime::task<void> release_detached(std::shared_ptr<store_owner> owner) {
    co_return co_await owner->release();
}

} // namespace

std::shared_ptr<store_owner> make_disk_store_owner(std::filesystem::path root) {
    return std::make_shared<disk_store_owner>(std::move(root));
}

namespace store_detail {

owned_store_backend::owned_store_backend(std::shared_ptr<::liquid::store_backend> inner,
                                         owner_grant grant,
                                         bool read_after_loss)
    : inner_(std::move(inner)), grant_(std::move(grant)), read_after_loss_(read_after_loss) {}

result<void> owned_store_backend::admit(bool mutation, const std::string& key) const {
    if (!grant_.lost() || (!mutation && read_after_loss_))
        return {};
    return std::unexpected(error{errc::locked,
                                 "this store's owner grant was lost; another holder may own the "
                                 "store, so close it",
                                 key});
}

template <class T> result<T> owned_store_backend::observe(result<T> outcome) const {
    if (!outcome && outcome.error().code == errc::locked)
        grant_.revoke();
    return outcome;
}

runtime::task<void> owned_store_backend::put(std::string key, std::vector<std::byte> bytes) {
    if (auto admitted = admit(true, key); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->put(std::move(key), std::move(bytes)));
}

runtime::task<void> owned_store_backend::put_if_absent(std::string key,
                                                       std::vector<std::byte> bytes) {
    if (auto admitted = admit(true, key); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->put_if_absent(std::move(key), std::move(bytes)));
}

runtime::task<void> owned_store_backend::put_range(std::string key,
                                                   std::uint64_t offset,
                                                   std::vector<std::byte> bytes) {
    if (auto admitted = admit(true, key); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->put_range(std::move(key), offset, std::move(bytes)));
}

runtime::task<std::vector<std::byte>>
owned_store_backend::get_range(std::string key, std::uint64_t offset, std::uint64_t size) {
    if (auto admitted = admit(false, key); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->get_range(std::move(key), offset, size));
}

runtime::task<void> owned_store_backend::remove(std::string key) {
    if (auto admitted = admit(true, key); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->remove(std::move(key)));
}

runtime::task<std::vector<store_backend_entry>> owned_store_backend::list(std::string prefix) {
    if (auto admitted = admit(false, prefix); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->list(std::move(prefix)));
}

runtime::task<void> owned_store_backend::barrier() {
    if (auto admitted = admit(true, "barrier"); !admitted)
        co_return std::unexpected(admitted.error());
    co_return observe(co_await inner_->barrier());
}

result<std::shared_ptr<::liquid::store_backend>>
owned_store_backend::fenced(std::uint64_t fence) const {
    return inner_->fenced(fence);
}

const std::shared_ptr<::liquid::store_backend>& owned_store_backend::inner() const noexcept {
    return inner_;
}

owner_hold::owner_hold(std::shared_ptr<::liquid::store_owner> owner,
                       owner_grant grant,
                       bool injected)
    : owner_(std::move(owner)), grant_(std::move(grant)), injected_(injected) {}

owner_hold::~owner_hold() {
    abandon();
}

owner_hold::owner_hold(owner_hold&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), grant_(std::exchange(other.grant_, {})),
      injected_(other.injected_) {}

owner_hold& owner_hold::operator=(owner_hold&& other) noexcept {
    if (this != &other) {
        abandon();
        owner_ = std::exchange(other.owner_, nullptr);
        grant_ = std::exchange(other.grant_, {});
        injected_ = other.injected_;
    }
    return *this;
}

void owner_hold::abandon() noexcept {
    grant_.revoke();
    auto owner = std::exchange(owner_, nullptr);
    if (!owner || !injected_)
        return;
    try {
        runtime::spawn(release_detached(std::move(owner)), [](result<void>) {});
    } catch (...) {
        // The owner's own expiry (a lease's time to live) then frees the store.
    }
}

runtime::task<void> owner_hold::release() {
    grant_.revoke();
    auto owner = std::exchange(owner_, nullptr);
    if (!owner)
        co_return result<void>{};
    co_return co_await owner->release();
}

} // namespace store_detail
} // namespace liquid
