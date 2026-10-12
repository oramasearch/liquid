// Sockets opened and closed concurrently inside runtime::block_on (liquid #329).
//
// block_on runs its private io_context on several runner threads. A reactor pass on one runner
// holds raw descriptor-state pointers returned by kevent()/epoll_wait() while other runners run
// handlers. Before corosio#5 was fixed, a socket destroyed on one runner during that window was
// freed while the pass still held it, and the next lock of its mutex threw "mutex lock failed" on
// a runner thread (or the pass wrote to freed memory). This test drives that window: each lane
// opens a connected pair, parks a read on one end, writes twice to the other and closes it, and
// the reader destroys its socket on whichever runner resumed it. The same load also exposed
// corosio#7: a work-count underflow that left io_context::run(), and so block_on, waiting forever.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <utility>

#include <boost/capy/ex/run_async.hpp>
#include <boost/corosio/io_context.hpp>
#include <boost/corosio/local_connect_pair.hpp>
#include <boost/corosio/local_stream_socket.hpp>
#include <boost/corosio/socket_option.hpp>
#include <boost/corosio/tcp.hpp>
#include <boost/corosio/tcp_acceptor.hpp>
#include <boost/corosio/tcp_socket.hpp>

#include "liquid/runtime.hpp"

namespace {

// Measured on a 16-core Mac against the pin without the fix: 16 to 128 lanes crashed every
// one-second round (30 of 30), while 256 and 1024 lanes crashed in only a few rounds out of 20.
constexpr int lanes = 64;
constexpr int max_live_readers = lanes;
constexpr int rounds = 2;
constexpr auto round_length = std::chrono::seconds(1);

struct stress_state {
    std::chrono::steady_clock::time_point deadline;
    std::atomic<std::size_t> pairs{0};
    std::atomic<std::size_t> reads{0};
    std::atomic<std::size_t> pair_errors{0};
    std::atomic<int> live_readers{0};
    std::atomic<int> live_lanes{0};
    std::string first_error;
};

boost::capy::task<> reader(boost::corosio::local_stream_socket socket,
                           std::shared_ptr<stress_state> state) {
    char buffer[16];
    auto [error, count] = co_await socket.read_some(boost::capy::mutable_buffer(buffer, 16));
    (void)error;
    (void)count;
    state->reads.fetch_add(1);
    state->live_readers.fetch_sub(1);
    // The socket is closed and destroyed here, on the runner that resumed this coroutine.
}

// Lanes and readers take the executor by value. `this_coro::executor` is a reference into the
// awaiting coroutine's environment, and a detached reader's launcher still touches its executor
// after the reader has counted itself finished, by which time that environment may be gone.
boost::capy::task<> lane(boost::corosio::io_context::executor_type executor,
                         std::shared_ptr<stress_state> state) {
    const char byte = 'x';
    while (std::chrono::steady_clock::now() < state->deadline) {
        if (state->live_readers.load() >= max_live_readers) {
            // Not a timer: yield posts the continuation and touches nothing afterwards.
            co_await liquid::runtime::yield();
            continue;
        }
        auto first = boost::corosio::local_stream_socket{executor.context()};
        auto second = boost::corosio::local_stream_socket{executor.context()};
        if (const auto error = boost::corosio::connect_pair(first, second)) {
            if (state->pair_errors.fetch_add(1) == 0)
                state->first_error = error.message();
            break;
        }
        state->pairs.fetch_add(1);
#ifdef SO_NOSIGPIPE
        // connect_pair adopts the descriptors as they are, and the writer may reach a reader that
        // has already closed.
        int one = 1;
        ::setsockopt(second.native_handle(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        state->live_readers.fetch_add(1);
        boost::capy::run_async(executor)(reader(std::move(first), state));
        (void)co_await second.write_some(boost::capy::const_buffer(&byte, 1));
        (void)co_await second.write_some(boost::capy::const_buffer(&byte, 1));
        second.close();
    }
    state->live_lanes.fetch_sub(1);
}

liquid::runtime::task<void> open_and_close_sockets(std::shared_ptr<stress_state> state) {
    const auto current = co_await boost::capy::this_coro::executor;
    const auto executor =
        static_cast<boost::corosio::io_context&>(current.context()).get_executor();
    state->live_lanes.store(lanes);
    for (auto index = 0; index < lanes; ++index)
        boost::capy::run_async(executor)(lane(executor, state));
    while (state->live_lanes.load() != 0 || state->live_readers.load() != 0)
        co_await liquid::runtime::yield();
    co_return {};
}

// TCP connects started after their socket was opened (liquid #345, corosio#10). tcp_socket::connect
// opens and registers the socket when the awaitable is made. On Linux, epoll reports an open,
// not-yet-connecting socket as EPOLLOUT|EPOLLHUP; on a Corosio before 0def6d3 a reactor pass that
// recorded it before the connect parked failed the connect with EIO ("Input/output error"). Every
// HTTP client in liquid (sync push and pull among them) dials this way, from block_on's runners.
// 512 connects a run. Each leaves a loopback TIME_WAIT for up to a minute, so --gtest_repeat beyond
// about 25 on macOS (16384 ephemeral ports) runs out of ports ("Can't assign requested address").
constexpr int connect_lanes = 32;
constexpr int connects_per_lane = 16;

struct connect_state {
    std::atomic<int> connected{0};
    std::atomic<int> failed{0};
    std::atomic<int> accepted{0};
    std::atomic<int> live_lanes{0};
    std::atomic<bool> accepting{true};
    std::string first_error;
};

boost::capy::task<> accept_all(boost::corosio::io_context::executor_type executor,
                               boost::corosio::tcp_acceptor& acceptor,
                               std::shared_ptr<connect_state> state) {
    while (state->accepting.load()) {
        auto peer = boost::corosio::tcp_socket{executor.context()};
        auto [error] = co_await acceptor.accept(peer);
        if (error)
            break;
        state->accepted.fetch_add(1);
    }
}

boost::capy::task<> connect_lane(boost::corosio::io_context::executor_type executor,
                                 boost::corosio::endpoint endpoint, int lane,
                                 std::shared_ptr<connect_state> state) {
    for (int index = 0; index < connects_per_lane; ++index) {
        auto socket = boost::corosio::tcp_socket{executor.context()};
        auto connecting = socket.connect(endpoint);
        // Let the reactor see the open socket before the connect starts: 0 to 3 yields.
        for (int yields = 0; yields < (lane + index) % 4; ++yields)
            co_await liquid::runtime::yield();
        auto [error] = co_await std::move(connecting);
        if (error) {
            if (state->failed.fetch_add(1) == 0)
                state->first_error = error.message();
        } else {
            state->connected.fetch_add(1);
        }
    }
    state->live_lanes.fetch_sub(1);
}

liquid::runtime::task<void> connect_many(std::shared_ptr<connect_state> state) {
    const auto current = co_await boost::capy::this_coro::executor;
    const auto executor =
        static_cast<boost::corosio::io_context&>(current.context()).get_executor();
    auto acceptor = boost::corosio::tcp_acceptor{executor.context()};
    acceptor.open();
    acceptor.set_option(boost::corosio::socket_option::reuse_address(true));
    if (auto error = acceptor.bind(boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(), 0)))
        co_return std::unexpected(liquid::error{liquid::errc::io, error.message(), "bind"});
    if (auto error = acceptor.listen(1024))
        co_return std::unexpected(liquid::error{liquid::errc::io, error.message(), "listen"});
    const auto endpoint = boost::corosio::endpoint(boost::corosio::ipv4_address::loopback(),
                                                   acceptor.local_endpoint().port());
    boost::capy::run_async(executor)(accept_all(executor, acceptor, state));
    state->live_lanes.store(connect_lanes);
    for (int lane = 0; lane < connect_lanes; ++lane)
        boost::capy::run_async(executor)(connect_lane(executor, endpoint, lane, state));
    while (state->live_lanes.load() != 0)
        co_await liquid::runtime::yield();
    const auto expected = state->connected.load();
    for (int spins = 0; state->accepted.load() < expected && spins < 100000; ++spins)
        co_await liquid::runtime::yield();
    state->accepting.store(false);
    acceptor.close();
    co_return {};
}

} // namespace

TEST(RuntimeSockets, TcpConnectsStartedAfterTheOpenSucceedInsideBlockOn) {
    auto state = std::make_shared<connect_state>();
    const auto completed = liquid::runtime::block_on(connect_many(state));
    ASSERT_TRUE(completed.has_value()) << completed.error().message;
    EXPECT_EQ(state->failed.load(), 0) << "first failure: " << state->first_error;
    EXPECT_EQ(state->connected.load(), connect_lanes * connects_per_lane);
}

TEST(RuntimeSockets, ConcurrentOpenAndCloseInsideBlockOn) {
    for (auto round = 0; round < rounds; ++round) {
        auto state = std::make_shared<stress_state>();
        state->deadline = std::chrono::steady_clock::now() + round_length;
        const auto completed = liquid::runtime::block_on(open_and_close_sockets(state));
        ASSERT_TRUE(completed.has_value()) << completed.error().message;
        EXPECT_EQ(state->pair_errors.load(), 0u) << state->first_error;
        EXPECT_GT(state->pairs.load(), 0u);
        EXPECT_EQ(state->reads.load(), state->pairs.load());
    }
}
