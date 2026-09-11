// Copyright (c) 2021-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/system.h>
#include <compat/compat.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <util/sock.h>
#include <util/threadinterrupt.h>

#include <boost/test/unit_test.hpp>

#include <cassert>
#include <thread>

#ifdef USE_POLL
#include <sys/resource.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

using namespace std::chrono_literals;

BOOST_FIXTURE_TEST_SUITE(sock_tests, BasicTestingSetup)

static bool SocketIsClosed(const SOCKET& s)
{
    // Notice that if another thread is running and creates its own socket after `s` has been
    // closed, it may be assigned the same file descriptor number. In this case, our test will
    // wrongly pretend that the socket is not closed.
    int type;
    socklen_t len = sizeof(type);
    return getsockopt(s, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &len) == SOCKET_ERROR;
}

static SOCKET CreateSocket()
{
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOST_REQUIRE(s != static_cast<SOCKET>(SOCKET_ERROR));
    return s;
}

BOOST_AUTO_TEST_CASE(constructor_and_destructor)
{
    const SOCKET s = CreateSocket();
    Sock* sock = new Sock(s);
    BOOST_CHECK(*sock == s);
    BOOST_CHECK(!SocketIsClosed(s));
    delete sock;
    BOOST_CHECK(SocketIsClosed(s));
}

BOOST_AUTO_TEST_CASE(move_constructor)
{
    const SOCKET s = CreateSocket();
    Sock* sock1 = new Sock(s);
    Sock* sock2 = new Sock(std::move(*sock1));
    delete sock1;
    BOOST_CHECK(!SocketIsClosed(s));
    BOOST_CHECK(*sock2 == s);
    delete sock2;
    BOOST_CHECK(SocketIsClosed(s));
}

BOOST_AUTO_TEST_CASE(move_assignment)
{
    const SOCKET s1 = CreateSocket();
    const SOCKET s2 = CreateSocket();
    Sock* sock1 = new Sock(s1);
    Sock* sock2 = new Sock(s2);

    BOOST_CHECK(!SocketIsClosed(s1));
    BOOST_CHECK(!SocketIsClosed(s2));

    *sock2 = std::move(*sock1);
    BOOST_CHECK(!SocketIsClosed(s1));
    BOOST_CHECK(SocketIsClosed(s2));
    BOOST_CHECK(*sock2 == s1);

    delete sock1;
    BOOST_CHECK(!SocketIsClosed(s1));
    BOOST_CHECK(SocketIsClosed(s2));
    BOOST_CHECK(*sock2 == s1);

    delete sock2;
    BOOST_CHECK(SocketIsClosed(s1));
    BOOST_CHECK(SocketIsClosed(s2));
}

struct TcpSocketPair {
    Sock sender;
    Sock receiver;

    TcpSocketPair()
        : sender{Sock{CreateSocket()}},
        receiver{Sock{CreateSocket()}}
    {
        connect_pair();
    }

    TcpSocketPair(const TcpSocketPair&) = delete;
    TcpSocketPair& operator= (const TcpSocketPair&) = delete;
    TcpSocketPair(TcpSocketPair&&) = default;
    TcpSocketPair& operator= (TcpSocketPair&&) = default;

    void connect_pair()
    {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        BOOST_REQUIRE_EQUAL(receiver.Bind(reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
        BOOST_REQUIRE_EQUAL(receiver.Listen(1), 0);

        // Get the address of the listener.
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        BOOST_REQUIRE_EQUAL(receiver.GetSockName(reinterpret_cast<sockaddr*>(&bound), &blen), 0);
        BOOST_REQUIRE_EQUAL(blen, sizeof(bound));

        BOOST_REQUIRE_EQUAL(sender.Connect(reinterpret_cast<sockaddr*>(&bound), sizeof(bound)), 0);

        std::unique_ptr<Sock> accepted = receiver.Accept(nullptr, nullptr);
        BOOST_REQUIRE(accepted != nullptr);

        receiver = std::move(*accepted);
    }

    void send_and_receive()
    {
        const char* msg = "abcd";
        constexpr ssize_t msg_len = 4;
        char recv_buf[10];

        BOOST_CHECK_EQUAL(sender.Send(msg, msg_len, 0), msg_len);
        BOOST_CHECK_EQUAL(receiver.Recv(recv_buf, sizeof(recv_buf), 0), msg_len);
        BOOST_CHECK_EQUAL(strncmp(msg, recv_buf, msg_len), 0);
    }
};

BOOST_AUTO_TEST_CASE(send_and_receive)
{
    TcpSocketPair socks{};
    socks.send_and_receive();

    // Sockets are still connected after being moved.
    TcpSocketPair socks_moved = std::move(socks);
    socks_moved.send_and_receive();
}

BOOST_AUTO_TEST_CASE(wait)
{
    TcpSocketPair socks = TcpSocketPair{};

    bool wait_result{false};
    Sock::Event occurred{0};
    std::thread waiter([&]() { wait_result = socks.receiver.Wait(1s, Sock::RecvEvent, &occurred); });

    BOOST_CHECK_EQUAL(socks.sender.Send("a", 1, 0), 1);

    waiter.join();
    BOOST_CHECK(wait_result);
    BOOST_CHECK(occurred & Sock::RecvEvent);
}

static void CheckReadinessAndClose(Sock& sender, Sock& receiver)
{
    // Non-blocking sockets ensure a readiness regression cannot hang recv().
    BOOST_REQUIRE(sender.SetNonBlocking());
    BOOST_REQUIRE(receiver.SetNonBlocking());
    BOOST_CHECK(sender.IsSelectable());
    BOOST_CHECK(receiver.IsSelectable());

    Sock::Event occurred{Sock::ErrorEvent};
    BOOST_REQUIRE(receiver.Wait(2ms, Sock::RecvEvent, &occurred));
    BOOST_CHECK_EQUAL(occurred, 0);

    BOOST_REQUIRE(sender.Wait(1s, Sock::SendEvent, &occurred));
    BOOST_CHECK(occurred & Sock::SendEvent);
    BOOST_CHECK(!(occurred & Sock::ErrorEvent));
    BOOST_REQUIRE_EQUAL(sender.Send("a", 1, 0), 1);
    BOOST_REQUIRE(receiver.Wait(1s, Sock::RecvEvent, &occurred));
    BOOST_CHECK(occurred & Sock::RecvEvent);
    // Request both events on the same descriptor. In particular, Darwin poll
    // needs one combined pollfd entry, not duplicate entries for each event.
    constexpr auto both{Sock::RecvEvent | Sock::SendEvent};
    BOOST_REQUIRE(receiver.Wait(1s, both, &occurred));
    BOOST_CHECK_EQUAL(occurred & both, both);
    char received{};
    BOOST_REQUIRE_EQUAL(receiver.Recv(&received, 1, 0), 1);
    BOOST_CHECK_EQUAL(received, 'a');

    sender = Sock{INVALID_SOCKET};
    BOOST_REQUIRE(receiver.Wait(1s, Sock::RecvEvent, &occurred));
    // Depending on the socket family/OS, EOF is readable, a hangup, or both.
    BOOST_CHECK(occurred & (Sock::RecvEvent | Sock::ErrorEvent));
    BOOST_CHECK_EQUAL(receiver.Recv(&received, 1, 0), 0);
}

BOOST_AUTO_TEST_CASE(wait_readiness_timeout_and_peer_close)
{
    TcpSocketPair socks{};
    CheckReadinessAndClose(socks.sender, socks.receiver);
}

BOOST_AUTO_TEST_CASE(wait_many_readiness_and_timeout)
{
    TcpSocketPair first{};
    TcpSocketPair second{};
    BOOST_REQUIRE(first.receiver.SetNonBlocking());
    BOOST_REQUIRE(second.receiver.SetNonBlocking());
    // Alias the existing stack-owned sockets without taking ownership.
    const std::shared_ptr<const Sock> first_receiver{std::shared_ptr<const Sock>{}, &first.receiver};
    const std::shared_ptr<const Sock> second_receiver{std::shared_ptr<const Sock>{}, &second.receiver};
    Sock::EventsPerSock events{
        {first_receiver, Sock::Events{Sock::RecvEvent}},
        {second_receiver, Sock::Events{Sock::RecvEvent}},
    };
    BOOST_REQUIRE(first.receiver.WaitMany(2ms, events));
    BOOST_CHECK_EQUAL(events.at(first_receiver).occurred, 0);
    BOOST_CHECK_EQUAL(events.at(second_receiver).occurred, 0);

    BOOST_REQUIRE_EQUAL(first.sender.Send("a", 1, 0), 1);
    BOOST_REQUIRE_EQUAL(second.sender.Send("b", 1, 0), 1);
    Sock::Event occurred{0};
    // Establish that each byte has arrived before also requesting send events,
    // otherwise writable readiness could win the race against TCP delivery.
    BOOST_REQUIRE(first.receiver.Wait(1s, Sock::RecvEvent, &occurred));
    BOOST_REQUIRE(occurred & Sock::RecvEvent);
    BOOST_REQUIRE(second.receiver.Wait(1s, Sock::RecvEvent, &occurred));
    BOOST_REQUIRE(occurred & Sock::RecvEvent);
    constexpr Sock::Event both{Sock::RecvEvent | Sock::SendEvent};
    events.at(first_receiver).requested = both;
    events.at(second_receiver).requested = both;
    BOOST_REQUIRE(first.receiver.WaitMany(1s, events));
    BOOST_CHECK_EQUAL(events.at(first_receiver).occurred & both, both);
    BOOST_CHECK_EQUAL(events.at(second_receiver).occurred & both, both);
}

#ifdef USE_POLL
#ifndef __NetBSD__
// NetBSD reports socket EOF as requested read readiness, not POLLHUP. Its
// poll(2) therefore cannot signal peer close to a wait with no read interest.
// The RecvEvent EOF checks above and below cover that platform as well.
BOOST_AUTO_TEST_CASE(wait_error_only_peer_close)
{
    int sockets[2];
    BOOST_REQUIRE_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    Sock sender{static_cast<SOCKET>(sockets[0])};
    Sock receiver{static_cast<SOCKET>(sockets[1])};
    Sock::Event occurred{Sock::ErrorEvent};
    BOOST_REQUIRE(receiver.Wait(2ms, 0, &occurred));
    BOOST_CHECK_EQUAL(occurred, 0);
    sender = Sock{INVALID_SOCKET};
    BOOST_REQUIRE(receiver.Wait(1s, 0, &occurred));
    BOOST_CHECK(occurred & Sock::ErrorEvent);
}
#endif

BOOST_AUTO_TEST_CASE(wait_above_fd_setsize)
{
    rlimit limits{};
    BOOST_REQUIRE_EQUAL(getrlimit(RLIMIT_NOFILE, &limits), 0);
    if (limits.rlim_cur != RLIM_INFINITY && limits.rlim_cur <= FD_SETSIZE) {
        BOOST_TEST_MESSAGE("Skipping high-fd poll test: inherited soft limit is at most FD_SETSIZE");
        return;
    }
#ifdef __APPLE__
    int kernel_limit{0};
    size_t size{sizeof(kernel_limit)};
    if (sysctlbyname("kern.maxfilesperproc", &kernel_limit, &size, nullptr, 0) == 0 &&
        size == sizeof(kernel_limit) && kernel_limit > 0 && kernel_limit <= FD_SETSIZE) {
        BOOST_TEST_MESSAGE("Skipping high-fd poll test: kernel descriptor ceiling is at most FD_SETSIZE");
        return;
    }
#endif

    int sockets[2];
    BOOST_REQUIRE_EQUAL(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), 0);
    Sock sender{static_cast<SOCKET>(sockets[0])};
    Sock original_receiver{static_cast<SOCKET>(sockets[1])};
    // Reserve only one high-numbered descriptor, not thousands of sockets.
    // Do not raise process-wide limits from this unit test.
    const int high_fd{fcntl(sockets[1], F_DUPFD_CLOEXEC, FD_SETSIZE)};
    BOOST_REQUIRE_MESSAGE(high_fd >= FD_SETSIZE, "Cannot duplicate high socket: " << NetworkErrorString(errno));
    Sock receiver{static_cast<SOCKET>(high_fd)};
    original_receiver = Sock{INVALID_SOCKET};
    CheckReadinessAndClose(sender, receiver);
}
#endif

BOOST_AUTO_TEST_CASE(recv_until_terminator_limit)
{
    constexpr auto timeout = 1min; // High enough so that it is never hit.
    CThreadInterrupt interrupt;

    TcpSocketPair socks = TcpSocketPair{};

    std::thread receiver([&socks, &timeout, &interrupt]() {
        constexpr size_t max_data{10};
        bool threw_as_expected{false};
        // BOOST_CHECK_EXCEPTION() writes to some variables shared with the main thread which
        // creates a data race. So mimic it manually.
        try {
            (void)socks.receiver.RecvUntilTerminator('\n', timeout, interrupt, max_data);
        } catch (const std::runtime_error& e) {
            threw_as_expected = HasReason("too many bytes without a terminator")(e);
        }
        assert(threw_as_expected);
    });

    BOOST_REQUIRE_NO_THROW(socks.sender.SendComplete("1234567", timeout, interrupt));
    BOOST_REQUIRE_NO_THROW(socks.sender.SendComplete("89a\n", timeout, interrupt));

    receiver.join();
}

BOOST_AUTO_TEST_SUITE_END()
