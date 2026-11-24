/**
 * @file proxy_socket_tests.cpp
 * @brief Unit tests for the real ProxySocket (bsd/proxy_socket.cpp)
 *
 * The socket talks to the ProxySocketManager singleton through plain function
 * pointer callbacks; the tests install scripted ones. Blocking paths are driven
 * with a second thread. os::Event comes from the host Stratosphere stand-in.
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

#include <stratosphere.hpp>

#include "test_harness.hpp"
#include "bsd/proxy_socket.hpp"
#include "bsd/proxy_socket_manager.hpp"
#include "config/config.hpp"
#include "debug/log.hpp"

namespace bsdt = ryu_ldn::bsd;
namespace proto = ryu_ldn::protocol;
using ams::mitm::bsd::MAX_ACCEPT_QUEUE;
using ams::mitm::bsd::MAX_PROXY_SOCKETS;
using ams::mitm::bsd::PROXY_SOCKET_MAX_PAYLOAD;
using ams::mitm::bsd::PROXY_SOCKET_MAX_QUEUE_SIZE;
using ams::mitm::bsd::ProxySocket;
using ams::mitm::bsd::ProxySocketManager;
using ams::mitm::bsd::ProxySocketState;
/// Test fixture value: Errno.
using Errno = bsdt::BsdErrno;

namespace {

// ---------------------------------------------------------------------------
// Scripted manager callbacks
// ---------------------------------------------------------------------------
/// Test double: Call.
struct Call {
    /// Test fixture value: src ip.
    uint32_t src_ip = 0;
    /// Test fixture value: src port.
    uint16_t src_port = 0;
    /// Test fixture value: dst ip.
    uint32_t dst_ip = 0;
    /// Test fixture value: dst port.
    uint16_t dst_port = 0;
    /// Test fixture value: protocol.
    bsdt::ProtocolType protocol = bsdt::ProtocolType::Unspecified;
    /// Test fixture value: len.
    size_t len = 0;
};

/// Test double: Script.
struct Script {
    /// Test fixture value: data result.
    bool data_result = true;
    /// Test fixture value: connect result.
    bool connect_result = true;
    /// Test fixture value: reply result.
    bool reply_result = true;
    /// Test fixture value: disconnect result.
    bool disconnect_result = true;
    /// Calls received by the data callback.
    std::vector<Call> data;
    /// Calls received by the connect callback.
    std::vector<Call> connect;
    /// Calls received by the reply callback.
    std::vector<Call> reply;
    /// Calls received by the disconnect callback.
    std::vector<Call> disconnect;

    // When set, the connect callback answers the pending connect on the spot
    // (what the receive thread does when the server's ProxyConnectReply lands).
    /// Test fixture value: answer to.
    ProxySocket* answer_to = nullptr;
    /// Test fixture value: answer protocol.
    proto::ProtocolType answer_protocol = proto::ProtocolType::Unspecified;
};
/// Test fixture value: g.
Script g;

/**
 * @brief Data callback: records the call and returns the scripted result.
 *
 * @param sip source IPv4 address
 * @param sp source port
 * @param dip destination IPv4 address
 * @param dp destination port
 * @param p transport protocol
 * @param len payload size in bytes, recorded
 * @return g.data_result, the scripted outcome
 */
bool on_data(
    uint32_t sip,
    uint16_t sp,
    uint32_t dip,
    uint16_t dp,
    bsdt::ProtocolType p,
    const void* /*unused*/,
    size_t len) {
    g.data.push_back({sip, sp, dip, dp, p, len});
    return g.data_result;
}

/**
 * @brief Test helper: on connect.
 *
 * @param sip source IPv4 address
 * @param sp source port
 * @param dip destination IPv4 address
 * @param dp destination port
 * @param p transport protocol
 * @return g.connect_result, the scripted outcome
 */
bool on_connect(
    uint32_t sip,
    uint16_t sp,
    uint32_t dip,
    uint16_t dp,
    bsdt::ProtocolType p) {
    g.connect.push_back({sip, sp, dip, dp, p, 0});
    if (g.connect_result && g.answer_to != nullptr) {
        proto::ProxyConnectResponse response{};
        response.info.protocol = g.answer_protocol;
        g.answer_to->HandleConnectResponse(response);
    }
    return g.connect_result;
}

/**
 * @brief Test helper: on reply.
 *
 * @param sip source IPv4 address
 * @param sp source port
 * @param dip destination IPv4 address
 * @param dp destination port
 * @param p transport protocol
 * @return g.reply_result, the scripted outcome
 */
bool on_reply(
    uint32_t sip,
    uint16_t sp,
    uint32_t dip,
    uint16_t dp,
    bsdt::ProtocolType p) {
    g.reply.push_back({sip, sp, dip, dp, p, 0});
    return g.reply_result;
}

/**
 * @brief Test helper: on disconnect.
 *
 * @param sip source IPv4 address
 * @param sp source port
 * @param dip destination IPv4 address
 * @param dp destination port
 * @param p transport protocol
 * @return g.disconnect_result, the scripted outcome
 */
bool on_disconnect(
    uint32_t sip,
    uint16_t sp,
    uint32_t dip,
    uint16_t dp,
    bsdt::ProtocolType p) {
    g.disconnect.push_back({sip, sp, dip, dp, p, 0});
    return g.disconnect_result;
}

/// Test helper: reset world.
void reset_world() {
    ProxySocketManager::GetInstance().Reset();
    g = Script{};
    ams::os::g_host_wait_divisor = 1;
    auto& m = ProxySocketManager::GetInstance();
    m.SetSendCallback(on_data);
    m.SetProxyConnectCallback(on_connect);
    m.SetProxyConnectReplyCallback(on_reply);
    m.SetProxyDisconnectCallback(on_disconnect);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Address as the game hands it over / as Bind stores it.
 *
 * @param addr_field value stored verbatim in sin_addr
 * @param port host-order port, stored byte-swapped
 * @param family address family, 2 (AF_INET) by default
 * @return the filled SockAddrIn
 */
bsdt::SockAddrIn addr_with(
    uint32_t addr_field, uint16_t port, uint8_t family = 2) {
    bsdt::SockAddrIn a{};
    a.sin_len = sizeof(a);
    a.sin_family = family;
    a.sin_port = __builtin_bswap16(port);
    a.sin_addr = addr_field;
    return a;
}

/**
 * @brief Local (Ryujinx-format) address, as stored by Bind.
 *
 * @param ryu_ip IPv4 address in Ryujinx format
 * @param port host-order port
 * @return address as Bind() stores it
 */
bsdt::SockAddrIn local(uint32_t ryu_ip, uint16_t port) {
    return addr_with(ryu_ip, port);
}

/**
 * @brief Peer address in network byte order, as sent by the game.
 *
 * @param ryu_ip IPv4 address in Ryujinx format
 * @param port host-order port
 * @return address with the IP byte-swapped to network order, as a game sends it
 */
bsdt::SockAddrIn peer(uint32_t ryu_ip, uint16_t port) {
    return addr_with(__builtin_bswap32(ryu_ip), port);
}

/**
 * @brief Test helper: errval.
 *
 * @param r result to inspect
 * @return the raw result value
 */
uint32_t errval(ams::Result r) {
    return r.GetValue();
}
/**
 * @brief Test helper: errno of.
 *
 * @param e BSD errno
 * @return e as an unsigned value, the form stored in a Result
 */
uint32_t errno_of(Errno e) {
    return static_cast<uint32_t>(e);
}
/**
 * @brief Test helper: neg.
 *
 * @param e BSD errno
 * @return the negated errno, as the socket calls return it
 */
s32 neg(Errno e) {
    return -static_cast<s32>(e);
}

/// Test fixture value: IP A.
constexpr uint32_t IP_A = 0x0A720001;
/// Test fixture value: IP B.
constexpr uint32_t IP_B = 0x0A720002;

/**
 * @brief Test helper: make udp.
 *
 * @return a new, unbound UDP proxy socket
 */
ProxySocket make_udp() {
    return {bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp};
}
/**
 * @brief Test helper: make tcp.
 *
 * @return a new, unbound TCP proxy socket
 */
ProxySocket make_tcp() {
    return {bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp};
}

/**
 * @brief Test helper: sleep ms.
 *
 * @param ms duration in milliseconds
 */
void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

}  // namespace

// ---------------------------------------------------------------------------
// State, bind, names
// ---------------------------------------------------------------------------

/**
 * @brief Test case: a new socket is unbound blocking and remembers its type and
 *   flags.
 */
TEST(a_new_socket_is_unbound_blocking_and_remembers_its_type_and_flags) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.GetState() == ProxySocketState::Created);
    ASSERT_TRUE(s.GetType() == bsdt::SocketType::Dgram);
    ASSERT_TRUE(s.GetProtocol() == bsdt::ProtocolType::Udp);
    ASSERT_FALSE(s.IsNonBlocking());
    s.SetNonBlocking(true);
    ASSERT_TRUE(s.IsNonBlocking());
    ASSERT_FALSE(s.IsBroadcastEnabled());
    s.SetBroadcastEnabled(true);
    ASSERT_TRUE(s.IsBroadcastEnabled());
    s.SetBroadcastAddress(0x0A72FFFF);
    ASSERT_FALSE(s.HasPendingData());
    ASSERT_FALSE(s.HasPendingConnections());
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(0));
}

/** @brief Test case: bind stores the address once and only for inet. */
/** @brief Test case: bind stores the address once and only for inet. */
TEST(bind_stores_the_address_once_and_only_for_inet) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_EQ(
        errval(s.Bind(addr_with(IP_A, 5000, 28))),
        errno_of(Errno::AfNoSupport));
    ASSERT_TRUE(s.GetState() == ProxySocketState::Created);
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.GetState() == ProxySocketState::Bound);
    ASSERT_EQ(s.GetLocalAddr().GetPort(), static_cast<uint16_t>(5000));
    ASSERT_EQ(s.GetLocalAddr().sin_addr, IP_A);
    ASSERT_EQ(errval(s.Bind(local(IP_A, 5001))), errno_of(Errno::Inval));
    ASSERT_EQ(s.GetLocalAddr().GetPort(), static_cast<uint16_t>(5000));
}

/**
 * @brief Test case: the local name is zeroed until bound and a null output is a
 *   fault.
 */
TEST(the_local_name_is_zeroed_until_bound_and_a_null_output_is_a_fault) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_EQ(errval(s.GetSockName(nullptr)), errno_of(Errno::Fault));

    bsdt::SockAddrIn out = addr_with(0xFFFFFFFF, 1234);
    ASSERT_TRUE(s.GetSockName(&out).IsSuccess());
    ASSERT_EQ(out.sin_addr, static_cast<uint32_t>(0));
    ASSERT_EQ(out.sin_port, static_cast<uint16_t>(0));
    ASSERT_EQ(out.sin_family, static_cast<uint8_t>(2));
    ASSERT_EQ(out.sin_len, static_cast<uint8_t>(sizeof(out)));

    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.GetSockName(&out).IsSuccess());
    ASSERT_EQ(out.sin_addr, IP_A);
    ASSERT_EQ(out.GetPort(), static_cast<uint16_t>(5000));
}

/** @brief Test case: the peer name needs a connected socket. */
/** @brief Test case: the peer name needs a connected socket. */
TEST(the_peer_name_needs_a_connected_socket) {
    reset_world();
    ProxySocket s = make_udp();
    bsdt::SockAddrIn out{};
    ASSERT_EQ(errval(s.GetPeerName(nullptr)), errno_of(Errno::Fault));
    ASSERT_EQ(errval(s.GetPeerName(&out)), errno_of(Errno::NotConn));

    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Connect(peer(IP_B, 6000)).IsSuccess());
    ASSERT_TRUE(s.GetPeerName(&out).IsSuccess());
    ASSERT_EQ(out.GetAddr(), IP_B);
    ASSERT_EQ(out.GetPort(), static_cast<uint16_t>(6000));
}

// ---------------------------------------------------------------------------
// Connect
// ---------------------------------------------------------------------------

/** @brief Test case: a udp connect only records the default destination. */
/** @brief Test case: a udp connect only records the default destination. */
TEST(a_udp_connect_only_records_the_default_destination) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_EQ(
        errval(s.Connect(addr_with(0, 1, 28))), errno_of(Errno::AfNoSupport));
    ASSERT_TRUE(s.Connect(peer(IP_B, 6000)).IsSuccess());
    ASSERT_TRUE(s.GetState() == ProxySocketState::Connected);
    ASSERT_TRUE(g.connect.empty());
}

/** @brief Test case: connecting a closed socket is a bad descriptor. */
/** @brief Test case: connecting a closed socket is a bad descriptor. */
TEST(connecting_a_closed_socket_is_a_bad_descriptor) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Close().IsSuccess());
    ASSERT_EQ(errval(s.Connect(peer(IP_B, 6000))), errno_of(Errno::BadF));
}

/**
 * @brief Test case: a tcp connect is unreachable when the proxy connect cannot
 *   be sent.
 */
TEST(a_tcp_connect_is_unreachable_when_the_proxy_connect_cannot_be_sent) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    g.connect_result = false;
    ASSERT_EQ(errval(s.Connect(peer(IP_B, 6000))), errno_of(Errno::NetUnreach));
    ASSERT_TRUE(s.GetState() == ProxySocketState::Bound);
    ASSERT_EQ(g.connect.size(), static_cast<size_t>(1));
}

/**
 * @brief Test case: a blocking tcp connect completes when the server answers.
 */
TEST(a_blocking_tcp_connect_completes_when_the_server_answers) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    g.answer_to = &s;
    ASSERT_TRUE(s.Connect(peer(IP_B, 6000)).IsSuccess());
    ASSERT_TRUE(s.GetState() == ProxySocketState::Connected);
    ASSERT_EQ(g.connect.size(), static_cast<size_t>(1));
    ASSERT_EQ(g.connect[0].dst_ip, IP_B);
    ASSERT_EQ(g.connect[0].dst_port, static_cast<uint16_t>(6000));
    ASSERT_EQ(g.connect[0].src_port, static_cast<uint16_t>(5000));
    ASSERT_TRUE(g.connect[0].protocol == bsdt::ProtocolType::Tcp);
}

/** @brief Test case: a blocking tcp connect times out without an answer. */
/** @brief Test case: a blocking tcp connect times out without an answer. */
TEST(a_blocking_tcp_connect_times_out_without_an_answer) {
    reset_world();
    ams::os::g_host_wait_divisor = 1000;  // the 4 s wait lasts 4 ms here
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_EQ(errval(s.Connect(peer(IP_B, 6000))), errno_of(Errno::TimedOut));
    ASSERT_TRUE(s.GetState() == ProxySocketState::Bound);
}

/**
 * @brief Test case: a blocking tcp connect is refused when the answer carries a
 *   protocol.
 */
TEST(a_blocking_tcp_connect_is_refused_when_the_answer_carries_a_protocol) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    g.answer_to = &s;
    g.answer_protocol = proto::ProtocolType::Tcp;
    ASSERT_EQ(
        errval(s.Connect(peer(IP_B, 6000))), errno_of(Errno::ConnRefused));
    ASSERT_TRUE(s.GetState() == ProxySocketState::Bound);
}

/**
 * @brief Test case: a non blocking tcp connect returns in progress and stays
 *   connecting.
 */
TEST(a_non_blocking_tcp_connect_returns_in_progress_and_stays_connecting) {
    reset_world();
    ProxySocket s = make_tcp();
    s.SetNonBlocking(true);
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_EQ(errval(s.Connect(peer(IP_B, 6000))), errno_of(Errno::InProgress));
    ASSERT_TRUE(s.GetState() == ProxySocketState::Connecting);
}

// ---------------------------------------------------------------------------
// Send
// ---------------------------------------------------------------------------

/**
 * @brief Test case: send needs a connected socket and uses the remembered
 *   destination.
 */
TEST(send_needs_a_connected_socket_and_uses_the_remembered_destination) {
    reset_world();
    ProxySocket s = make_udp();
    const char msg[] = "abc";
    ASSERT_EQ(s.Send(msg, 3, 0), neg(Errno::NotConn));

    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Connect(peer(IP_B, 6000)).IsSuccess());
    ASSERT_EQ(s.Send(msg, 3, 0), 3);
    ASSERT_EQ(g.data.size(), static_cast<size_t>(1));
    ASSERT_EQ(g.data[0].dst_ip, IP_B);
    ASSERT_EQ(g.data[0].dst_port, static_cast<uint16_t>(6000));
}

/** @brief Test case: sendto refuses a shut down closed or unbound socket. */
/** @brief Test case: sendto refuses a shut down closed or unbound socket. */
TEST(sendto_refuses_a_shut_down_closed_or_unbound_socket) {
    reset_world();
    const char msg[] = "abc";

    ProxySocket unbound = make_udp();
    ASSERT_EQ(
        unbound.SendTo(msg, 3, 0, peer(IP_B, 6000)), neg(Errno::DestAddrReq));

    ProxySocket shut = make_udp();
    ASSERT_TRUE(shut.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(shut.Shutdown(bsdt::ShutdownHow::Write).IsSuccess());
    ASSERT_EQ(shut.SendTo(msg, 3, 0, peer(IP_B, 6000)), neg(Errno::Inval));

    ProxySocket closed = make_udp();
    ASSERT_TRUE(closed.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(closed.Close().IsSuccess());
    ASSERT_EQ(closed.SendTo(msg, 3, 0, peer(IP_B, 6000)), neg(Errno::Inval));
    ASSERT_TRUE(g.data.empty());
}

/** @brief Test case: sendto validates the payload before routing it. */
/** @brief Test case: sendto validates the payload before routing it. */
TEST(sendto_validates_the_payload_before_routing_it) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    std::vector<uint8_t> big(PROXY_SOCKET_MAX_PAYLOAD + 1, 0x11);

    ASSERT_EQ(s.SendTo(nullptr, 4, 0, peer(IP_B, 6000)), neg(Errno::Fault));
    ASSERT_EQ(
        s.SendTo(big.data(), big.size(), 0, peer(IP_B, 6000)),
        neg(Errno::MsgSize));
    ASSERT_TRUE(g.data.empty());

    // The largest payload and an empty datagram both go through.
    ASSERT_EQ(
        s.SendTo(big.data(), PROXY_SOCKET_MAX_PAYLOAD, 0, peer(IP_B, 6000)),
        static_cast<s32>(PROXY_SOCKET_MAX_PAYLOAD));
    ASSERT_EQ(s.SendTo(nullptr, 0, 0, peer(IP_B, 6000)), 0);
    ASSERT_EQ(g.data.size(), static_cast<size_t>(2));
}

/** @brief Test case: sendto hands the manager the ryujinx format addresses. */
/** @brief Test case: sendto hands the manager the ryujinx format addresses. */
TEST(sendto_hands_the_manager_the_ryujinx_format_addresses) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    const char msg[] = "hello";
    ASSERT_EQ(s.SendTo(msg, 5, 0, peer(0x0A72FFFF, 7000)), 5);
    ASSERT_EQ(g.data.size(), static_cast<size_t>(1));
    ASSERT_EQ(g.data[0].src_ip, IP_A);
    ASSERT_EQ(g.data[0].src_port, static_cast<uint16_t>(5000));
    ASSERT_EQ(g.data[0].dst_ip, static_cast<uint32_t>(0x0A72FFFF));
    ASSERT_EQ(g.data[0].dst_port, static_cast<uint16_t>(7000));
    ASSERT_EQ(g.data[0].len, static_cast<size_t>(5));
    ASSERT_TRUE(g.data[0].protocol == bsdt::ProtocolType::Udp);
}

/** @brief Test case: sendto is unreachable when the manager cannot send. */
/** @brief Test case: sendto is unreachable when the manager cannot send. */
TEST(sendto_is_unreachable_when_the_manager_cannot_send) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    const char msg[] = "x";
    g.data_result = false;
    ASSERT_EQ(s.SendTo(msg, 1, 0, peer(IP_B, 6000)), neg(Errno::NetUnreach));
    ProxySocketManager::GetInstance().SetSendCallback(nullptr);
    ASSERT_EQ(s.SendTo(msg, 1, 0, peer(IP_B, 6000)), neg(Errno::NetUnreach));
}

// ---------------------------------------------------------------------------
// Receive
// ---------------------------------------------------------------------------

/**
 * @brief Test case: recv not connected when unconnected reads when connected.
 */
TEST(recv_not_connected_when_unconnected_reads_when_connected) {
    reset_world();
    ProxySocket s = make_udp();
    uint8_t buf[8];
    ASSERT_EQ(s.Recv(buf, sizeof(buf), 0), neg(Errno::NotConn));

    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Connect(peer(IP_B, 6000)).IsSuccess());
    const uint8_t payload[] = {1, 2, 3};
    s.IncomingData(payload, 3, peer(IP_B, 6000));
    ASSERT_EQ(s.Recv(buf, sizeof(buf), 0), 3);
    ASSERT_EQ(buf[2], static_cast<uint8_t>(3));
}

/**
 * @brief Test case: recvfrom returns eof after a read shutdown and faults on a
 *   null buffer.
 */
TEST(recvfrom_returns_eof_after_a_read_shutdown_and_faults_on_a_null_buffer) {
    reset_world();
    ProxySocket s = make_udp();
    uint8_t buf[8];
    ASSERT_EQ(s.RecvFrom(nullptr, 4, 0, nullptr), neg(Errno::Fault));
    ASSERT_TRUE(s.Shutdown(bsdt::ShutdownHow::Read).IsSuccess());
    ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0, nullptr), 0);
}

/**
 * @brief Test case: a non blocking or dontwait read of an empty queue would
 *   block.
 */
TEST(a_non_blocking_or_dontwait_read_of_an_empty_queue_would_block) {
    reset_world();
    ProxySocket s = make_udp();
    uint8_t buf[8];
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0x80, nullptr), neg(Errno::Again));
    s.SetNonBlocking(true);
    ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0, nullptr), neg(Errno::Again));
}

/**
 * @brief Test case: recvfrom copies truncates reports the sender and pops
 *   unless peeking.
 */
TEST(recvfrom_copies_truncates_reports_the_sender_and_pops_unless_peeking) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    const uint8_t payload[] = {10, 20, 30, 40, 50};
    s.IncomingData(payload, 5, peer(IP_B, 6000));
    s.IncomingData(payload, 2, peer(IP_B, 6001));
    ASSERT_TRUE(s.HasPendingData());
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(7));

    uint8_t buf[8] = {};
    bsdt::SockAddrIn from{};
    ASSERT_EQ(s.RecvFrom(buf, 3, 0x2, &from), 3);  // MSG_PEEK, truncated
    ASSERT_EQ(buf[2], static_cast<uint8_t>(30));
    ASSERT_EQ(from.GetPort(), static_cast<uint16_t>(6000));
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(7));

    ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0, &from), 5);  // consumes the first
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(2));
    ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0, nullptr), 2);
    ASSERT_FALSE(s.HasPendingData());
    ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0x80, nullptr), neg(Errno::Again));
}

/**
 * @brief Test case: an empty destination buffer still consumes the datagram.
 */
TEST(an_empty_destination_buffer_still_consumes_the_datagram) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    s.IncomingData("x", 1, peer(IP_B, 6000));
    ASSERT_EQ(s.RecvFrom(nullptr, 0, 0, nullptr), 0);
    ASSERT_FALSE(s.HasPendingData());
}

/** @brief Test case: a blocking read waits for the next datagram. */
/** @brief Test case: a blocking read waits for the next datagram. */
TEST(a_blocking_read_waits_for_the_next_datagram) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    std::atomic<int> result{-9999};
    uint8_t buf[8] = {};
    std::thread reader(
        [&] { result = s.RecvFrom(buf, sizeof(buf), 0, nullptr); });
    sleep_ms(100);
    ASSERT_EQ(result.load(), -9999);  // still blocked
    s.IncomingData("zz", 2, peer(IP_B, 6000));
    reader.join();
    ASSERT_EQ(result.load(), 2);
}

/**
 * @brief Test case: a shutdown wakes a blocked reader which then reports would
 *   block.
 */
TEST(a_shutdown_wakes_a_blocked_reader_which_then_reports_would_block) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    std::atomic<int> result{-9999};
    uint8_t buf[8];
    std::thread reader(
        [&] { result = s.RecvFrom(buf, sizeof(buf), 0, nullptr); });
    sleep_ms(100);
    ASSERT_TRUE(s.Shutdown(bsdt::ShutdownHow::Both).IsSuccess());
    reader.join();
    ASSERT_EQ(result.load(), neg(Errno::Again));
}

// ---------------------------------------------------------------------------
// IncomingData
// ---------------------------------------------------------------------------

/** @brief Test case: incoming data is clamped to the payload limit. */
/** @brief Test case: incoming data is clamped to the payload limit. */
TEST(incoming_data_is_clamped_to_the_payload_limit) {
    reset_world();
    ProxySocket s = make_udp();
    std::vector<uint8_t> big(PROXY_SOCKET_MAX_PAYLOAD + 500, 0x7E);
    s.IncomingData(big.data(), big.size(), peer(IP_B, 6000));
    ASSERT_EQ(s.GetPendingDataSize(), PROXY_SOCKET_MAX_PAYLOAD);
}

/** @brief Test case: a full receive queue drops the oldest datagram. */
/** @brief Test case: a full receive queue drops the oldest datagram. */
TEST(a_full_receive_queue_drops_the_oldest_datagram) {
    reset_world();
    ProxySocket s = make_udp();
    for (size_t i = 0; i < PROXY_SOCKET_MAX_QUEUE_SIZE + 1; i++) {
        const auto tag = static_cast<uint8_t>(i);
        s.IncomingData(&tag, 1, peer(IP_B, 6000));
    }
    ASSERT_EQ(s.GetPendingDataSize(), PROXY_SOCKET_MAX_QUEUE_SIZE);
    uint8_t buf[1];
    ASSERT_EQ(s.RecvFrom(buf, 1, 0, nullptr), 1);
    ASSERT_EQ(buf[0], static_cast<uint8_t>(1));  // datagram 0 was dropped
}

/** @brief Test case: a wrapped queue is still summed and read in order. */
/** @brief Test case: a wrapped queue is still summed and read in order. */
TEST(a_wrapped_queue_is_still_summed_and_read_in_order) {
    reset_world();
    ProxySocket s = make_udp();
    uint8_t buf[4];
    for (int round = 0; round < 3; round++) {
        for (uint8_t i = 0; i < 20; i++) {
            s.IncomingData(&i, 1, peer(IP_B, 6000));
        }
        for (uint8_t i = 0; i < 20; i++) {
            ASSERT_EQ(s.RecvFrom(buf, sizeof(buf), 0, nullptr), 1);
            ASSERT_EQ(buf[0], i);
        }
    }
    s.IncomingData("a", 1, peer(IP_B, 6000));
    s.IncomingData("b", 1, peer(IP_B, 6000));
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(2));
}

/**
 * @brief Test case: an empty or null payload is queued as an empty datagram.
 */
TEST(an_empty_or_null_payload_is_queued_as_an_empty_datagram) {
    reset_world();
    ProxySocket s = make_udp();
    s.IncomingData(nullptr, 0, peer(IP_B, 6000));
    s.IncomingData(nullptr, 5, peer(IP_B, 6000));
    ASSERT_TRUE(s.HasPendingData());
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(5));
}

/** @brief Test case: a verbose logger gets a hex dump of the first bytes. */
/** @brief Test case: a verbose logger gets a hex dump of the first bytes. */
TEST(a_verbose_logger_gets_a_hex_dump_of_the_first_bytes) {
    reset_world();
    const std::string path =
        (std::filesystem::temp_directory_path() / "proxy_socket_tests.log")
            .string();
    ryu_ldn::config::DebugConfig cfg{};
    cfg.enabled = true;
    cfg.level = 3;
    ryu_ldn::debug::g_logger.init(cfg, path.c_str());
    ASSERT_TRUE(
        ryu_ldn::debug::g_logger.should_log(ryu_ldn::debug::LogLevel::Verbose));

    ProxySocket s = make_udp();
    std::vector<uint8_t> payload(100, 0xAB);
    s.IncomingData(payload.data(), payload.size(), peer(IP_B, 6000));
    s.IncomingData(payload.data(), 3, peer(IP_B, 6000));
    ASSERT_EQ(s.GetPendingDataSize(), static_cast<size_t>(103));

    ryu_ldn::debug::g_logger.SetEnabled(false);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

/**
 * @brief Test case: setsockopt toggles broadcast and accepts the common
 *   options.
 */
TEST(setsockopt_toggles_broadcast_and_accepts_the_common_options) {
    reset_world();
    ProxySocket s = make_udp();
    const s32 on = 1;
    const s32 off = 0;
    const s32 sol = static_cast<s32>(bsdt::SocketOptionLevel::Socket);
    const s32 bc = static_cast<s32>(bsdt::SocketOption::Broadcast);

    ASSERT_TRUE(s.SetSockOpt(sol, bc, &on, sizeof(on)).IsSuccess());
    ASSERT_TRUE(s.IsBroadcastEnabled());
    ASSERT_TRUE(s.SetSockOpt(sol, bc, &off, sizeof(off)).IsSuccess());
    ASSERT_FALSE(s.IsBroadcastEnabled());
    ASSERT_EQ(
        errval(s.SetSockOpt(sol, bc, nullptr, sizeof(on))),
        errno_of(Errno::Inval));
    ASSERT_EQ(errval(s.SetSockOpt(sol, bc, &on, 1)), errno_of(Errno::Inval));

    using O = bsdt::SocketOption;
    const O ignored[] = {
        O::ReuseAddr,
        O::KeepAlive,
        O::DontRoute,
        O::Linger,
        O::OobInline,
        O::ReusePort,
        O::SndBuf,
        O::RcvBuf,
        O::SndLoWat,
        O::RcvLoWat,
        O::SndTimeo,
        O::RcvTimeo};
    for (O o : ignored) {
        ASSERT_TRUE(s.SetSockOpt(sol, static_cast<s32>(o), &on, sizeof(on))
                        .IsSuccess());
    }
    // Unknown option, and options of other levels, are accepted and ignored.
    ASSERT_TRUE(s.SetSockOpt(sol, 0x7777, &on, sizeof(on)).IsSuccess());
    ASSERT_TRUE(
        s.SetSockOpt(
             static_cast<s32>(bsdt::SocketOptionLevel::Tcp), 1, &on, sizeof(on))
            .IsSuccess());
}

/**
 * @brief Test case: getsockopt reports type error and broadcast and refuses the
 *   rest.
 */
TEST(getsockopt_reports_type_error_and_broadcast_and_refuses_the_rest) {
    reset_world();
    ProxySocket s = make_tcp();
    const s32 sol = static_cast<s32>(bsdt::SocketOptionLevel::Socket);
    s32 value = -1;
    size_t len = sizeof(value);

    ASSERT_EQ(
        errval(s.GetSockOpt(sol, 1, nullptr, &len)), errno_of(Errno::Fault));
    ASSERT_EQ(
        errval(s.GetSockOpt(sol, 1, &value, nullptr)), errno_of(Errno::Fault));

    ASSERT_TRUE(
        s.GetSockOpt(
             sol, static_cast<s32>(bsdt::SocketOption::Type), &value, &len)
            .IsSuccess());
    ASSERT_EQ(value, static_cast<s32>(bsdt::SocketType::Stream));
    ASSERT_EQ(len, sizeof(s32));

    value = -1;
    len = 16;
    ASSERT_TRUE(
        s.GetSockOpt(
             sol, static_cast<s32>(bsdt::SocketOption::Error), &value, &len)
            .IsSuccess());
    ASSERT_EQ(value, 0);
    ASSERT_EQ(len, sizeof(s32));

    s.SetBroadcastEnabled(true);
    value = -1;
    len = sizeof(value);
    ASSERT_TRUE(
        s.GetSockOpt(
             sol, static_cast<s32>(bsdt::SocketOption::Broadcast), &value, &len)
            .IsSuccess());
    ASSERT_EQ(value, 1);

    // Buffer too small for an int, unknown option, other level.
    len = 1;
    ASSERT_EQ(
        errval(s.GetSockOpt(
            sol, static_cast<s32>(bsdt::SocketOption::Type), &value, &len)),
        errno_of(Errno::NoProtoOpt));
    len = 1;
    ASSERT_EQ(
        errval(s.GetSockOpt(
            sol, static_cast<s32>(bsdt::SocketOption::Error), &value, &len)),
        errno_of(Errno::NoProtoOpt));
    len = 1;
    ASSERT_EQ(
        errval(s.GetSockOpt(
            sol,
            static_cast<s32>(bsdt::SocketOption::Broadcast),
            &value,
            &len)),
        errno_of(Errno::NoProtoOpt));
    len = sizeof(value);
    ASSERT_EQ(
        errval(s.GetSockOpt(sol, 0x7777, &value, &len)),
        errno_of(Errno::NoProtoOpt));
    ASSERT_EQ(
        errval(s.GetSockOpt(
            static_cast<s32>(bsdt::SocketOptionLevel::Tcp), 1, &value, &len)),
        errno_of(Errno::NoProtoOpt));
}

// ---------------------------------------------------------------------------
// Listen / Accept
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Test helper: request from.
 *
 * @param ryu_ip source IPv4 address in Ryujinx format
 * @param port source port
 * @return ProxyConnectRequest with only the source address set
 */
proto::ProxyConnectRequest request_from(uint32_t ryu_ip, uint16_t port) {
    proto::ProxyConnectRequest r{};
    r.info.source_ipv4 = ryu_ip;
    r.info.source_port = port;
    return r;
}
}  // namespace

/** @brief Test case: listen needs a bound tcp socket. */
/** @brief Test case: listen needs a bound tcp socket. */
TEST(listen_needs_a_bound_tcp_socket) {
    reset_world();
    ProxySocket udp = make_udp();
    ASSERT_TRUE(udp.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_EQ(errval(udp.Listen(4)), errno_of(Errno::OpNotSupp));

    ProxySocket tcp = make_tcp();
    ASSERT_EQ(errval(tcp.Listen(4)), errno_of(Errno::Inval));
    ASSERT_TRUE(tcp.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(tcp.Listen(4).IsSuccess());
    ASSERT_TRUE(tcp.GetState() == ProxySocketState::Listening);
}

/**
 * @brief Test case: accept needs listening socket and never blocks non
 *   blocking.
 */
TEST(accept_needs_listening_socket_and_never_blocks_non_blocking) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Accept(nullptr) == nullptr);
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Listen(4).IsSuccess());
    s.SetNonBlocking(true);
    ASSERT_TRUE(s.Accept(nullptr) == nullptr);
}

/**
 * @brief Test case: a connection request on a non listening socket is ignored.
 */
TEST(a_connection_request_on_a_non_listening_socket_is_ignored) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    s.IncomingConnection(request_from(IP_B, 6000));
    ASSERT_FALSE(s.HasPendingConnections());
    ASSERT_TRUE(g.reply.empty());
}

/**
 * @brief Test case: an incoming connection is queued confirmed and accepted.
 */
TEST(an_incoming_connection_is_queued_confirmed_and_accepted) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Listen(4).IsSuccess());
    s.IncomingConnection(request_from(IP_B, 6000));
    ASSERT_TRUE(s.HasPendingConnections());

    ASSERT_EQ(g.reply.size(), static_cast<size_t>(1));
    ASSERT_EQ(g.reply[0].dst_ip, IP_B);
    ASSERT_EQ(g.reply[0].dst_port, static_cast<uint16_t>(6000));
    ASSERT_EQ(g.reply[0].src_port, static_cast<uint16_t>(5000));
    ASSERT_TRUE(g.reply[0].protocol == bsdt::ProtocolType::Tcp);

    bsdt::SockAddrIn from{};
    auto accepted = s.Accept(&from);
    ASSERT_TRUE(accepted != nullptr);
    ASSERT_TRUE(accepted->GetState() == ProxySocketState::Connected);
    ASSERT_EQ(from.sin_addr, IP_B);  // Ryujinx format, not byte-swapped
    ASSERT_EQ(from.GetPort(), static_cast<uint16_t>(6000));
    ASSERT_EQ(accepted->GetLocalAddr().GetPort(), static_cast<uint16_t>(5000));
    ASSERT_FALSE(s.HasPendingConnections());

    s.IncomingConnection(request_from(IP_B, 6001));
    ASSERT_TRUE(s.Accept(nullptr) != nullptr);  // no address wanted
}

/**
 * @brief Test case: a full accept queue drops new connections but still answers
 *   the peer.
 */
TEST(a_full_accept_queue_drops_new_connections_but_still_answers_the_peer) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Listen(4).IsSuccess());
    for (size_t i = 0; i < MAX_ACCEPT_QUEUE + 1; i++) {
        s.IncomingConnection(
            request_from(IP_B, static_cast<uint16_t>(6000 + i)));
    }
    ASSERT_EQ(g.reply.size(), MAX_ACCEPT_QUEUE + 1);
    s.SetNonBlocking(true);
    size_t accepted = 0;
    bsdt::SockAddrIn from{};
    while (auto a = s.Accept(&from)) {
        ASSERT_EQ(
            from.GetPort(),
            static_cast<uint16_t>(6000 + accepted));  // arrival order
        accepted++;
    }
    ASSERT_EQ(accepted, MAX_ACCEPT_QUEUE);
}

/** @brief Test case: a blocking accept waits for a connection. */
/** @brief Test case: a blocking accept waits for a connection. */
TEST(a_blocking_accept_waits_for_a_connection) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Listen(4).IsSuccess());
    std::atomic<bool> got{false};
    bsdt::SockAddrIn from{};
    std::thread acceptor([&] { got = (s.Accept(&from) != nullptr); });
    sleep_ms(100);
    ASSERT_FALSE(got.load());
    s.IncomingConnection(request_from(IP_B, 6000));
    acceptor.join();
    ASSERT_TRUE(got.load());
    ASSERT_EQ(from.GetPort(), static_cast<uint16_t>(6000));
}

/** @brief Test case: two blocked acceptors one connection only one wins. */
TEST(two_blocked_acceptors_one_connection_only_one_wins) {
    reset_world();
    ProxySocket s = make_tcp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(s.Listen(4).IsSuccess());
    std::atomic<int> winners{0};
    std::atomic<int> losers{0};
    auto body = [&] { (s.Accept(nullptr) != nullptr ? winners : losers)++; };
    std::thread a(body);
    std::thread b(body);
    sleep_ms(200);  // both are blocked on the event
    s.IncomingConnection(request_from(IP_B, 6000));
    a.join();
    b.join();
    ASSERT_EQ(winners.load(), 1);
    ASSERT_EQ(losers.load(), 1);
}

// ---------------------------------------------------------------------------
// Shutdown / Close / WaitForData
// ---------------------------------------------------------------------------

/**
 * @brief Build a ShutdownHow holding a raw value.
 *
 * ShutdownHow has a fixed int32_t underlying type, so any int32_t is a valid
 * value of the enum; the one built here names no enumerator on purpose, to
 * exercise the rejection path in Shutdown().
 *
 * @param raw Underlying value to reinterpret.
 * @return The enum value holding raw.
 */
bsdt::ShutdownHow unknown_shutdown_how(int32_t raw) {
    bsdt::ShutdownHow how{};
    std::memcpy(&how, &raw, sizeof(how));
    return how;
}

/**
 * @brief Test case: shutdown sets the requested directions and refuses unknown
 *   ones.
 */
TEST(shutdown_sets_the_requested_directions_and_refuses_unknown_ones) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_TRUE(s.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_EQ(
        errval(s.Shutdown(unknown_shutdown_how(7))), errno_of(Errno::Inval));
    ASSERT_TRUE(s.Shutdown(bsdt::ShutdownHow::Write).IsSuccess());
    uint8_t buf[2];
    ASSERT_EQ(s.SendTo("x", 1, 0, peer(IP_B, 6000)), neg(Errno::Inval));
    s.IncomingData("x", 1, peer(IP_B, 6000));
    ASSERT_EQ(s.RecvFrom(buf, 2, 0, nullptr), 1);  // reading is still allowed
    ASSERT_TRUE(s.Shutdown(bsdt::ShutdownHow::Read).IsSuccess());
    ASSERT_EQ(s.RecvFrom(buf, 2, 0, nullptr), 0);
}

/**
 * @brief Test case: close drops queued data and only announces a connected tcp
 *   peer.
 */
TEST(close_drops_queued_data_and_only_announces_a_connected_tcp_peer) {
    reset_world();
    ProxySocket udp = make_udp();
    ASSERT_TRUE(udp.Bind(local(IP_A, 5000)).IsSuccess());
    ASSERT_TRUE(udp.Connect(peer(IP_B, 6000)).IsSuccess());
    udp.IncomingData("x", 1, peer(IP_B, 6000));
    ASSERT_TRUE(udp.Close().IsSuccess());
    ASSERT_TRUE(udp.GetState() == ProxySocketState::Closed);
    ASSERT_FALSE(udp.HasPendingData());
    ASSERT_TRUE(g.disconnect.empty());

    ProxySocket idle_tcp = make_tcp();  // never connected: remote port is 0
    ASSERT_TRUE(idle_tcp.Bind(local(IP_A, 5001)).IsSuccess());
    ASSERT_TRUE(idle_tcp.Close().IsSuccess());
    ASSERT_TRUE(g.disconnect.empty());

    ProxySocket tcp = make_tcp();
    ASSERT_TRUE(tcp.Bind(local(IP_A, 5002)).IsSuccess());
    g.answer_to = &tcp;
    ASSERT_TRUE(tcp.Connect(peer(IP_B, 6000)).IsSuccess());
    ASSERT_TRUE(tcp.Close().IsSuccess());
    ASSERT_EQ(g.disconnect.size(), static_cast<size_t>(1));
    ASSERT_EQ(g.disconnect[0].dst_ip, IP_B);
    ASSERT_EQ(g.disconnect[0].dst_port, static_cast<uint16_t>(6000));
    ASSERT_EQ(g.disconnect[0].src_port, static_cast<uint16_t>(5002));
}

/** @brief Test case: wait for data returns at once if queued else times out. */
TEST(wait_for_data_returns_at_once_if_queued_else_times_out) {
    reset_world();
    ams::os::g_host_wait_divisor = 1000;
    ProxySocket s = make_udp();
    ASSERT_FALSE(s.WaitForData(50));
    s.IncomingData("x", 1, peer(IP_B, 6000));
    ASSERT_TRUE(s.WaitForData(50));
    ASSERT_TRUE(s.WaitForData(
        0));  // data already there: no wait even without a timeout
}

/** @brief Test case: waiting without a timeout blocks until data arrives. */
/** @brief Test case: waiting without a timeout blocks until data arrives. */
TEST(waiting_without_a_timeout_blocks_until_data_arrives) {
    reset_world();
    ProxySocket s = make_udp();
    std::atomic<bool> done{false};
    std::thread waiter([&] { done = s.WaitForData(0); });
    sleep_ms(100);
    ASSERT_FALSE(done.load());
    s.IncomingData("x", 1, peer(IP_B, 6000));
    waiter.join();
    ASSERT_TRUE(done.load());
}

/** @brief Test case: the receive event is the one the queue signals. */
/** @brief Test case: the receive event is the one the queue signals. */
TEST(the_receive_event_is_the_one_the_queue_signals) {
    reset_world();
    ProxySocket s = make_udp();
    ASSERT_FALSE(s.GetReceiveEvent().TimedWait(ams::TimeSpan(0)));
    s.IncomingData("x", 1, peer(IP_B, 6000));
    ASSERT_TRUE(s.GetReceiveEvent().TimedWait(ams::TimeSpan(0)));
}

// ---------------------------------------------------------------------------
// ProxySocketManager
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief Shorthand for the ProxySocketManager singleton.
 *
 * @return the ProxySocketManager singleton
 */
ProxySocketManager& mgr() {
    return ProxySocketManager::GetInstance();
}

/**
 * @brief A manager-owned UDP socket bound to `port` on `sin_addr` (0 = any).
 *
 * @param fd descriptor the manager registers the socket under
 * @param port host-order port to bind
 * @param sin_addr address stored by Bind(), 0 for any
 * @return the created socket, or null when the manager refused it
 */
ProxySocket* bound_udp(s32 fd, uint16_t port, uint32_t sin_addr = 0) {
    ProxySocket* s = mgr().CreateProxySocket(
        fd, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp);
    if (s != nullptr) {
        (void)s->Bind(addr_with(sin_addr, port));
    }
    return s;
}

/**
 * @brief Delivers a datagram to the manager as if it came from the server.
 *
 * @param src_ip source IPv4 address
 * @param src_port source port
 * @param dst_ip destination IPv4 address
 * @param dst_port destination port
 * @param data datagram payload
 * @param len payload size in bytes
 * @param p transport protocol
 * @return result of RouteIncomingData(): true when a socket received the data
 */
bool route(
    uint32_t src_ip,
    uint16_t src_port,
    uint32_t dst_ip,
    uint16_t dst_port,
    const void* data = "ab",
    size_t len = 2,
    bsdt::ProtocolType p = bsdt::ProtocolType::Udp) {
    return mgr().RouteIncomingData(
        src_ip, src_port, dst_ip, dst_port, p, data, len);
}

/**
 * @brief Test helper: read one.
 *
 * @param s socket to read from
 * @param from receives the sender address, may be null
 * @return RecvFrom() result: bytes read, or a negative errno (non-blocking
 * read)
 */
int read_one(ProxySocket* s, bsdt::SockAddrIn* from = nullptr) {
    uint8_t buf[2048];
    return s->RecvFrom(buf, sizeof(buf), 0x80, from);
}

}  // namespace

/**
 * @brief Test case: the manager creates finds and closes sockets by descriptor.
 */
TEST(the_manager_creates_finds_and_closes_sockets_by_descriptor) {
    reset_world();
    ASSERT_FALSE(mgr().IsProxySocket(10));
    ASSERT_TRUE(mgr().GetProxySocket(10) == nullptr);
    ProxySocket* s = mgr().CreateProxySocket(
        10, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp);
    ASSERT_TRUE(s != nullptr);
    ASSERT_TRUE(mgr().IsProxySocket(10));
    ASSERT_TRUE(mgr().GetProxySocket(10) == s);
    ASSERT_TRUE(
        mgr().CreateProxySocket(
            10, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp) == s);
    ASSERT_EQ(mgr().GetActiveSocketCount(), static_cast<size_t>(1));

    ASSERT_FALSE(mgr().CloseProxySocket(11));
    ASSERT_TRUE(mgr().CloseProxySocket(10));
    ASSERT_FALSE(mgr().IsProxySocket(10));
    ASSERT_EQ(mgr().GetActiveSocketCount(), static_cast<size_t>(0));
}

/**
 * @brief Test case: the manager holds at most max proxy sockets and reuses
 *   freed slots.
 */
TEST(the_manager_holds_at_most_max_proxy_sockets_and_reuses_freed_slots) {
    reset_world();
    for (s32 fd = 0; fd < static_cast<s32>(MAX_PROXY_SOCKETS); fd++) {
        ASSERT_TRUE(
            mgr().CreateProxySocket(
                fd, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp) !=
            nullptr);
    }
    ASSERT_TRUE(
        mgr().CreateProxySocket(
            1000, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp) == nullptr);
    ASSERT_TRUE(mgr().CloseProxySocket(5));
    ASSERT_TRUE(
        mgr().CreateProxySocket(
            1000, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp) != nullptr);
    ASSERT_EQ(mgr().GetActiveSocketCount(), MAX_PROXY_SOCKETS);
}

/** @brief Test case: closing a socket gives its port back to the pool. */
/** @brief Test case: closing a socket gives its port back to the pool. */
TEST(closing_a_socket_gives_its_port_back_to_the_pool) {
    reset_world();
    const size_t before = mgr().GetAvailablePortCount(bsdt::ProtocolType::Udp);
    const uint16_t port = mgr().AllocatePort(bsdt::ProtocolType::Udp);
    ASSERT_TRUE(port != 0);
    ASSERT_EQ(mgr().GetAvailablePortCount(bsdt::ProtocolType::Udp), before - 1);
    ProxySocket* s = bound_udp(3, port);
    ASSERT_TRUE(s != nullptr);
    ASSERT_TRUE(mgr().CloseProxySocket(3));
    ASSERT_EQ(mgr().GetAvailablePortCount(bsdt::ProtocolType::Udp), before);

    // A socket that never bound has no port to give back.
    ASSERT_TRUE(
        mgr().CreateProxySocket(
            4, bsdt::SocketType::Dgram, bsdt::ProtocolType::Udp) != nullptr);
    ASSERT_TRUE(mgr().CloseProxySocket(4));
    ASSERT_EQ(mgr().GetAvailablePortCount(bsdt::ProtocolType::Udp), before);
}

/** @brief Test case: ports can be reserved once and released. */
/** @brief Test case: ports can be reserved once and released. */
TEST(ports_can_be_reserved_once_and_released) {
    reset_world();
    ASSERT_TRUE(mgr().ReservePort(50000, bsdt::ProtocolType::Udp));
    ASSERT_FALSE(mgr().ReservePort(50000, bsdt::ProtocolType::Udp));
    mgr().ReleasePort(50000, bsdt::ProtocolType::Udp);
    ASSERT_TRUE(mgr().ReservePort(50000, bsdt::ProtocolType::Udp));
}

/**
 * @brief Test case: close all and reset drop every socket and reset clears the
 *   rest.
 */
TEST(close_all_and_reset_drop_every_socket_and_reset_clears_the_rest) {
    reset_world();
    ASSERT_TRUE(bound_udp(1, 5000) != nullptr);
    ASSERT_TRUE(bound_udp(2, 5001) != nullptr);
    ASSERT_TRUE(mgr().ReservePort(50001, bsdt::ProtocolType::Udp));
    mgr().CloseAllProxySockets();
    ASSERT_EQ(mgr().GetActiveSocketCount(), static_cast<size_t>(0));
    ASSERT_TRUE(
        mgr().ReservePort(50001, bsdt::ProtocolType::Udp));  // ports released

    ASSERT_TRUE(bound_udp(1, 5000) != nullptr);
    mgr().SetLocalIp(IP_A);
    ASSERT_FALSE(route(IP_B, 1, IP_A, 9999));  // buffered
    mgr().Reset();
    ASSERT_EQ(mgr().GetActiveSocketCount(), static_cast<size_t>(0));
    ASSERT_EQ(mgr().GetLocalIp(), static_cast<uint32_t>(0));
    ASSERT_FALSE(
        mgr().SendProxyData(
            1, 2, 3, 4, bsdt::ProtocolType::Udp, "x", 1));  // callback gone
    ProxySocket* late = bound_udp(1, 9999);
    mgr().DeliverPendingPackets(late, 9999, bsdt::ProtocolType::Udp);
    ASSERT_FALSE(late->HasPendingData());  // ring emptied
}

/** @brief Test case: the local ip is stored. */
/** @brief Test case: the local ip is stored. */
TEST(the_local_ip_is_stored) {
    reset_world();
    mgr().SetLocalIp(IP_A);
    ASSERT_EQ(mgr().GetLocalIp(), IP_A);
}

/**
 * @brief Test case: send helpers false without callback else forward result.
 */
TEST(send_helpers_false_without_callback_else_forward_result) {
    reset_world();
    auto& m = mgr();
    m.SetSendCallback(nullptr);
    m.SetProxyConnectCallback(nullptr);
    m.SetProxyConnectReplyCallback(nullptr);
    m.SetProxyDisconnectCallback(nullptr);
    const auto u = bsdt::ProtocolType::Udp;
    ASSERT_FALSE(m.SendProxyData(1, 2, 3, 4, u, "x", 1));
    ASSERT_FALSE(m.SendProxyConnect(1, 2, 3, 4, u));
    ASSERT_FALSE(m.SendProxyConnectReply(1, 2, 3, 4, u));
    ASSERT_FALSE(m.SendProxyDisconnect(1, 2, 3, 4, u));

    m.SetSendCallback(on_data);
    m.SetProxyConnectCallback(on_connect);
    m.SetProxyConnectReplyCallback(on_reply);
    m.SetProxyDisconnectCallback(on_disconnect);
    ASSERT_TRUE(m.SendProxyData(1, 2, 3, 4, u, "x", 1));
    ASSERT_TRUE(m.SendProxyConnect(1, 2, 3, 4, u));
    ASSERT_TRUE(m.SendProxyConnectReply(1, 2, 3, 4, u));
    ASSERT_TRUE(m.SendProxyDisconnect(1, 2, 3, 4, u));
    ASSERT_EQ(g.data[0].dst_port, static_cast<uint16_t>(4));

    g.data_result = g.connect_result = g.reply_result = g.disconnect_result =
        false;
    ASSERT_FALSE(m.SendProxyData(1, 2, 3, 4, u, "x", 1));
    ASSERT_FALSE(m.SendProxyConnect(1, 2, 3, 4, u));
    ASSERT_FALSE(m.SendProxyConnectReply(1, 2, 3, 4, u));
    ASSERT_FALSE(m.SendProxyDisconnect(1, 2, 3, 4, u));
}

/**
 * @brief Test case: a connect response reaches the connecting socket it
 *   answers.
 */
TEST(a_connect_response_reaches_the_connecting_socket_it_answers) {
    reset_world();
    ProxySocket* waiting = mgr().CreateProxySocket(
        1, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    waiting->SetNonBlocking(true);
    ASSERT_TRUE(waiting->Bind(addr_with(0, 5000)).IsSuccess());
    ASSERT_EQ(
        errval(waiting->Connect(peer(IP_B, 6000))),
        errno_of(Errno::InProgress));
    ProxySocket* idle = mgr().CreateProxySocket(
        2, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    ASSERT_TRUE(idle->Bind(addr_with(0, 5001)).IsSuccess());
    mgr().CloseProxySocket(
        2);  // leaves a free slot before `waiting`'s siblings
    ProxySocket* other = mgr().CreateProxySocket(
        3, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    other->SetNonBlocking(true);
    ASSERT_TRUE(other->Bind(addr_with(0, 5002)).IsSuccess());
    ASSERT_EQ(
        errval(other->Connect(peer(IP_B, 6000))), errno_of(Errno::InProgress));

    proto::ProxyConnectResponse response{};
    response.info.source_ipv4 = 0;
    response.info.source_port = 7777;  // nobody listens there
    ASSERT_FALSE(mgr().RouteConnectResponse(response));
    response.info.source_port = 5000;
    ASSERT_TRUE(mgr().RouteConnectResponse(response));
    response.info.source_port = 5002;
    ASSERT_TRUE(mgr().RouteConnectResponse(response));
    // Only Connecting sockets are candidates: a merely bound one is skipped.
    ProxySocket* bound = mgr().CreateProxySocket(
        4, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    ASSERT_TRUE(bound->Bind(addr_with(0, 5003)).IsSuccess());
    response.info.source_port = 5003;
    ASSERT_FALSE(mgr().RouteConnectResponse(response));
}

/** @brief Test case: a connect request reaches the matching tcp listener. */
/** @brief Test case: a connect request reaches the matching tcp listener. */
TEST(a_connect_request_reaches_the_matching_tcp_listener) {
    reset_world();
    auto listener = [](s32 fd, uint16_t port, uint32_t sin_addr) {
        ProxySocket* s = mgr().CreateProxySocket(
            fd, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
        (void)s->Bind(addr_with(sin_addr, port));
        (void)s->Listen(4);
        return s;
    };
    ProxySocket* unbound = mgr().CreateProxySocket(
        1, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    ProxySocket* wrong_port = listener(2, 5001, 0);
    ProxySocket* wrong_ip = listener(3, 5000, __builtin_bswap32(IP_B));
    ProxySocket* right = listener(4, 5000, __builtin_bswap32(IP_A));
    ProxySocket* any = listener(5, 5002, 0);

    proto::ProxyConnectRequest request{};
    request.info.source_ipv4 = IP_B;
    request.info.source_port = 6000;
    request.info.dest_ipv4 = IP_A;
    request.info.dest_port = 5000;
    ASSERT_TRUE(mgr().RouteConnectRequest(request));
    ASSERT_TRUE(right->HasPendingConnections());
    ASSERT_FALSE(wrong_ip->HasPendingConnections());
    ASSERT_FALSE(wrong_port->HasPendingConnections());
    ASSERT_FALSE(unbound->HasPendingConnections());

    request.info.dest_port =
        5002;  // listener bound to INADDR_ANY accepts any address
    ASSERT_TRUE(mgr().RouteConnectRequest(request));
    ASSERT_TRUE(any->HasPendingConnections());

    request.info.dest_port = 5999;
    ASSERT_FALSE(mgr().RouteConnectRequest(request));
}

/**
 * @brief Test case: incoming unicast data goes to the socket bound to that
 *   address and port.
 */
TEST(incoming_unicast_data_goes_to_the_socket_bound_to_that_address_and_port) {
    reset_world();
    ProxySocket* a = bound_udp(1, 5000, IP_A);
    ProxySocket* b = bound_udp(2, 5000, IP_B);
    ProxySocket* other_port = bound_udp(3, 5001, IP_A);
    ASSERT_TRUE(route(IP_B, 6000, IP_A, 5000));
    ASSERT_TRUE(a->HasPendingData());
    ASSERT_FALSE(b->HasPendingData());
    ASSERT_FALSE(other_port->HasPendingData());

    bsdt::SockAddrIn from{};
    ASSERT_EQ(read_one(a, &from), 2);
    ASSERT_EQ(
        from.sin_addr,
        __builtin_bswap32(IP_B));  // network byte order for the game
    ASSERT_EQ(from.GetPort(), static_cast<uint16_t>(6000));
    ASSERT_EQ(from.sin_len, static_cast<uint8_t>(sizeof(from)));
}

/**
 * @brief Test case: wildcard socket gets any address for its protocol and port.
 */
TEST(wildcard_socket_gets_any_address_for_its_protocol_and_port) {
    reset_world();
    ProxySocket* any = bound_udp(1, 5000, 0);
    ProxySocket* tcp = mgr().CreateProxySocket(
        2, bsdt::SocketType::Stream, bsdt::ProtocolType::Tcp);
    ASSERT_TRUE(tcp->Bind(addr_with(0, 5000)).IsSuccess());
    ASSERT_TRUE(route(IP_B, 6000, IP_A, 5000));
    ASSERT_TRUE(any->HasPendingData());
    ASSERT_FALSE(tcp->HasPendingData());
}

/**
 * @brief Test case: broadcast reaches every socket of the subnet on that port.
 */
TEST(broadcast_reaches_every_socket_of_the_subnet_on_that_port) {
    reset_world();
    ProxySocket* same_subnet_1 = bound_udp(1, 5000, 0x0A720005);
    ProxySocket* same_subnet_2 = bound_udp(2, 5000, 0x0A720006);
    ProxySocket* other_subnet = bound_udp(3, 5000, 0x0A730005);
    ProxySocket* other_port = bound_udp(4, 5001, 0x0A720005);
    ASSERT_TRUE(route(IP_B, 6000, 0x0A72FFFF, 5000));
    ASSERT_TRUE(same_subnet_1->HasPendingData());
    ASSERT_TRUE(same_subnet_2->HasPendingData());
    ASSERT_FALSE(other_subnet->HasPendingData());
    ASSERT_FALSE(other_port->HasPendingData());

    // A directed broadcast ending in 0xFF is recognised as well.
    ASSERT_TRUE(route(IP_B, 6000, 0x0A7200FF, 5000));
    ASSERT_EQ(read_one(same_subnet_1), 2);
    ASSERT_EQ(read_one(same_subnet_1), 2);
}

/** @brief Test case: a delivery is capped at eight sockets. */
/** @brief Test case: a delivery is capped at eight sockets. */
TEST(a_delivery_is_capped_at_eight_sockets) {
    reset_world();
    std::vector<ProxySocket*> sockets;
    sockets.reserve(10);
    for (s32 fd = 0; fd < 10; fd++) {
        sockets.push_back(bound_udp(fd, 5000, 0));
    }
    ASSERT_TRUE(route(IP_B, 6000, IP_A, 5000));
    size_t delivered = 0;
    for (ProxySocket* s : sockets) {
        delivered += s->HasPendingData() ? 1U : 0U;
    }
    ASSERT_EQ(delivered, static_cast<size_t>(8));
}

/**
 * @brief Test case: data without listener buffered then delivered on bind in
 *   order.
 */
TEST(data_without_listener_buffered_then_delivered_on_bind_in_order) {
    reset_world();
    ASSERT_FALSE(route(IP_B, 6000, IP_A, 7000, "A", 1));
    ASSERT_FALSE(route(IP_B, 6001, IP_A, 7001, "B", 1));
    ASSERT_FALSE(route(IP_B, 6002, IP_A, 7000, "C", 1));
    ASSERT_FALSE(
        route(IP_B, 6003, IP_A, 7000, "T", 1, bsdt::ProtocolType::Tcp));

    mgr().DeliverPendingPackets(
        nullptr, 7000, bsdt::ProtocolType::Udp);  // nothing to deliver to

    ProxySocket* first = bound_udp(1, 7000);
    mgr().DeliverPendingPackets(first, 7000, bsdt::ProtocolType::Udp);
    bsdt::SockAddrIn from{};
    uint8_t buf[4];
    ASSERT_EQ(first->RecvFrom(buf, sizeof(buf), 0x80, &from), 1);
    ASSERT_EQ(buf[0], static_cast<uint8_t>('A'));
    ASSERT_EQ(from.sin_addr, __builtin_bswap32(IP_B));
    ASSERT_EQ(from.GetPort(), static_cast<uint16_t>(6000));
    ASSERT_EQ(first->RecvFrom(buf, sizeof(buf), 0x80, nullptr), 1);
    ASSERT_EQ(buf[0], static_cast<uint8_t>('C'));
    ASSERT_FALSE(first->HasPendingData());

    // What did not match is still waiting, in order, for its own socket.
    ProxySocket* second = bound_udp(2, 7001);
    mgr().DeliverPendingPackets(second, 7001, bsdt::ProtocolType::Udp);
    ASSERT_EQ(second->RecvFrom(buf, sizeof(buf), 0x80, nullptr), 1);
    ASSERT_EQ(buf[0], static_cast<uint8_t>('B'));

    // Delivering again finds nothing for the first socket and keeps the TCP
    // packet.
    mgr().DeliverPendingPackets(first, 7000, bsdt::ProtocolType::Udp);
    ASSERT_FALSE(first->HasPendingData());
}

/** @brief Test case: delivering with an empty ring does nothing. */
/** @brief Test case: delivering with an empty ring does nothing. */
TEST(delivering_with_an_empty_ring_does_nothing) {
    reset_world();
    ProxySocket* s = bound_udp(1, 7000);
    mgr().DeliverPendingPackets(s, 7000, bsdt::ProtocolType::Udp);
    ASSERT_FALSE(s->HasPendingData());
}

/**
 * @brief Test case: the pending ring keeps the newest sixteen and clamps the
 *   payload.
 */
TEST(the_pending_ring_keeps_the_newest_sixteen_and_clamps_the_payload) {
    reset_world();
    for (uint8_t i = 0; i < 20; i++) {
        ASSERT_FALSE(route(IP_B, 6000, IP_A, 7000, &i, 1));
    }
    std::vector<uint8_t> big(2000, 0x5A);
    ASSERT_FALSE(route(IP_B, 6000, IP_A, 7000, big.data(), big.size()));
    ASSERT_FALSE(route(IP_B, 6000, IP_A, 7000, nullptr, 3));

    ProxySocket* s = bound_udp(1, 7000);
    mgr().DeliverPendingPackets(s, 7000, bsdt::ProtocolType::Udp);
    uint8_t buf[2048];
    // 22 sent, 16 kept: packets 6..19, the big one, the empty-data one.
    for (uint8_t expected = 6; expected < 20; expected++) {
        ASSERT_EQ(s->RecvFrom(buf, sizeof(buf), 0x80, nullptr), 1);
        ASSERT_EQ(buf[0], expected);
    }
    ASSERT_EQ(s->RecvFrom(buf, sizeof(buf), 0x80, nullptr), 1400);
    // Null data with a length keeps that length (the slot's bytes are not
    // written).
    ASSERT_EQ(s->RecvFrom(buf, sizeof(buf), 0x80, nullptr), 3);
    ASSERT_FALSE(s->HasPendingData());
}

/**
 * @brief Runs the whole suite; the exit code is non-zero if any test failed.
 *
 * @return 0 when every test passed, non-zero otherwise
 */
int main() {
    return test_report("ProxySocket Tests");
}
