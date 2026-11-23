/**
 * @file mock_tcp_client.hpp
 * @brief Mock TCP client for unit testing RyuLdnClient
 *
 * Provides full control over connect/disconnect/send/receive behavior
 * to exercise all code paths in RyuLdnClient without real sockets.
 */

#pragma once

#include "network/itcp_client.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <queue>
#include <vector>

namespace ryu_ldn::network::test {

/// @brief Packet queued in MockTcpClient::recv_queue and handed back by
/// receive_packet().
struct MockPacket {
    /// Packet type reported through the `type` output of receive_packet().
    protocol::PacketId id;
    /// Raw payload copied into the caller's buffer by receive_packet().
    std::vector<uint8_t> data;
};

/**
 * @brief Scriptable ITcpClient test double.
 * Every call is counted, results come from the next_*_result fields and
 * incoming packets come from recv_queue. No sockets are opened.
 */
class MockTcpClient : public ITcpClient {
public:
    /// True between a successful connect() and disconnect().
    bool connected = false;
    /// Set by initialize() when initialize_should_fail is false.
    bool initialized = false;
    /// When true, initialize() returns false and leaves initialized unset.
    bool initialize_should_fail = false;

    /// Result returned by connect(); connected only becomes true on Success.
    ClientResult next_connect_result = ClientResult::Success;
    /// Result returned by every send_*() call (except send_initialize() when
    /// use_initialize_result is set).
    ClientResult next_send_result = ClientResult::Success;
    /// Result returned by receive_packet() when recv_queue is empty; Success is
    /// mapped to Timeout.
    ClientResult next_recv_result = ClientResult::Timeout;

    /// When true, send_initialize() returns next_initialize_result instead of
    /// next_send_result.
    bool use_initialize_result = false;
    /// Result returned by send_initialize() when use_initialize_result is true.
    ClientResult next_initialize_result = ClientResult::Success;

    /// FIFO of packets receive_packet() delivers, one per call, before
    /// reporting next_recv_result.
    std::queue<MockPacket> recv_queue;

    /// Number of connect() calls since construction or the last
    /// reset_counters().
    uint32_t connect_call_count = 0;
    /// Number of disconnect() calls since construction or the last
    /// reset_counters().
    uint32_t disconnect_call_count = 0;
    /// Number of send_packet() calls since construction or the last
    /// reset_counters().
    uint32_t send_packet_call_count = 0;
    /// Number of send_raw() calls since construction or the last
    /// reset_counters().
    uint32_t send_raw_call_count = 0;
    /// Number of send_initialize() calls since construction or the last
    /// reset_counters().
    uint32_t send_initialize_call_count = 0;
    /// Number of send_passphrase(const PassphraseMessage&) calls since
    /// construction or the last reset_counters().
    uint32_t send_passphrase_msg_call_count = 0;
    /// Number of send_passphrase(const char*) calls since construction or the
    /// last reset_counters().
    uint32_t send_passphrase_str_call_count = 0;
    /// Number of send_ping() calls since construction or the last
    /// reset_counters().
    uint32_t send_ping_call_count = 0;
    /// Number of send_disconnect() calls since construction or the last
    /// reset_counters().
    uint32_t send_disconnect_call_count = 0;
    /// Number of send_create_access_point() calls since construction or the
    /// last reset_counters().
    uint32_t send_create_access_point_call_count = 0;
    /// Number of send_create_access_point_private() calls since construction or
    /// the last reset_counters().
    uint32_t send_create_access_point_private_call_count = 0;
    /// Number of send_connect() calls since construction or the last
    /// reset_counters().
    uint32_t send_connect_call_count = 0;
    /// Number of send_connect_private() calls since construction or the last
    /// reset_counters().
    uint32_t send_connect_private_call_count = 0;
    /// Number of send_scan() calls since construction or the last
    /// reset_counters().
    uint32_t send_scan_call_count = 0;
    /// Number of send_proxy_data() calls since construction or the last
    /// reset_counters().
    uint32_t send_proxy_data_call_count = 0;
    /// Number of send_set_accept_policy() calls since construction or the last
    /// reset_counters().
    uint32_t send_set_accept_policy_call_count = 0;
    /// Number of send_set_advertise_data() calls since construction or the last
    /// reset_counters().
    uint32_t send_set_advertise_data_call_count = 0;
    /// Number of send_reject() calls since construction or the last
    /// reset_counters().
    uint32_t send_reject_call_count = 0;
    /// Number of receive_packet() calls since construction or the last
    /// reset_counters().
    uint32_t recv_call_count = 0;

    /// Host passed to the last connect() call ("" for a null host).
    std::string last_connect_host;
    /// Port passed to the last connect() call.
    uint16_t last_connect_port = 0;
    /// Timeout (ms) passed to the last connect() call.
    uint32_t last_connect_timeout_ms = 0;

    /**
     * @brief Mock initialize: marks the client initialized unless scripted to
     * fail.
     *
     * @return false when initialize_should_fail is set, else true
     */
    bool initialize() override {
        if (initialize_should_fail)
            return false;
        initialized = true;
        return true;
    }

    /**
     * @brief Records the arguments and connects unless scripted to fail.
     *
     * @param host host to record; null is stored as an empty string
     * @param port port to record
     * @param timeout_ms timeout to record, in milliseconds
     * @return next_connect_result
     */
    ClientResult connect(
        const char* host, uint16_t port, uint32_t timeout_ms) override {
        connect_call_count++;
        last_connect_host = host ? host : "";
        last_connect_port = port;
        last_connect_timeout_ms = timeout_ms;
        if (next_connect_result == ClientResult::Success) {
            connected = true;
        }
        return next_connect_result;
    }

    /**
     * @brief Counts the call and clears the connected flag.
     *
     */
    void disconnect() override {
        disconnect_call_count++;
        connected = false;
    }

    /**
     * @brief Reports the scripted connection flag.
     *
     * @return the value of connected
     */
    bool is_connected() const override {
        return connected;
    }

    /**
     * @brief Counts the call; the packet is not stored.
     *
     * @return next_send_result
     */
    ClientResult send_packet(protocol::PacketId, const void*, size_t) override {
        send_packet_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call; the bytes are not stored.
     *
     * @return next_send_result
     */
    ClientResult send_raw(const void*, size_t) override {
        send_raw_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call; may fail independently of the other send methods.
     *
     * @return next_initialize_result when use_initialize_result is set, else
     * next_send_result
     */
    ClientResult send_initialize(const protocol::InitializeMessage&) override {
        send_initialize_call_count++;
        // Lets a test fail the Initialize message alone while the preceding
        // Passphrase message (which uses next_send_result) still succeeds.
        if (use_initialize_result)
            return next_initialize_result;
        return next_send_result;
    }

    /**
     * @brief Counts the call of the message overload.
     *
     * @return next_send_result
     */
    ClientResult send_passphrase(const protocol::PassphraseMessage&) override {
        send_passphrase_msg_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call of the C-string overload.
     *
     * @return next_send_result
     */
    ClientResult send_passphrase(const char*) override {
        send_passphrase_str_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_ping(const protocol::PingMessage&) override {
        send_ping_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_disconnect(const protocol::DisconnectMessage&) override {
        send_disconnect_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_create_access_point(
        const protocol::CreateAccessPointRequest&,
        const uint8_t*,
        size_t) override {
        send_create_access_point_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_create_access_point_private(
        const protocol::CreateAccessPointPrivateRequest&,
        const uint8_t*,
        size_t) override {
        send_create_access_point_private_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_connect(const protocol::ConnectRequest&) override {
        send_connect_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_connect_private(
        const protocol::ConnectPrivateRequest&) override {
        send_connect_private_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_scan(const protocol::ScanFilterFull&) override {
        send_scan_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call; the payload is not stored.
     *
     * @return next_send_result
     */
    ClientResult send_proxy_data(
        const protocol::ProxyDataHeader&, const uint8_t*, size_t) override {
        send_proxy_data_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_set_accept_policy(
        const protocol::SetAcceptPolicyRequest&) override {
        send_set_accept_policy_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_set_advertise_data(const uint8_t*, size_t) override {
        send_set_advertise_data_call_count++;
        return next_send_result;
    }

    /**
     * @brief Counts the call.
     *
     * @return next_send_result
     */
    ClientResult send_reject(const protocol::RejectRequest&) override {
        send_reject_call_count++;
        return next_send_result;
    }

    /**
     * @brief Pops the next scripted packet from recv_queue.
     *
     * With an empty queue it returns next_recv_result, except that Success is
     * mapped to Timeout so process loops that run until a non-Success result
     * terminate.
     *
     * @param[out] type packet id of the popped packet (0xFF when empty)
     * @param[out] payload buffer receiving the packet data, may be null
     * @param payload_buffer_size capacity of payload in bytes
     * @param[out] payload_size size of the popped packet data
     * @return Success, BufferTooSmall when the packet exceeds the buffer, or
     *         the scripted result when the queue is empty
     */
    ClientResult receive_packet(
        protocol::PacketId& type,
        void* payload,
        size_t payload_buffer_size,
        size_t& payload_size,
        int32_t /*timeout_ms*/) override {
        recv_call_count++;
        if (recv_queue.empty()) {
            // When queue is empty, never return Success — that causes infinite
            // loops in process_packets() which loops until recv returns
            // non-Success. Return next_recv_result only if it's an error;
            // otherwise return Timeout.
            type = static_cast<protocol::PacketId>(0xFF);
            payload_size = 0;
            if (next_recv_result == ClientResult::Success) {
                return ClientResult::Timeout;
            }
            return next_recv_result;
        }
        MockPacket pkt = std::move(recv_queue.front());
        recv_queue.pop();
        type = pkt.id;
        payload_size = pkt.data.size();
        if (payload_size > payload_buffer_size) {
            return ClientResult::BufferTooSmall;
        }
        if (payload_size > 0 && payload != nullptr) {
            std::memcpy(payload, pkt.data.data(), payload_size);
        }
        return ClientResult::Success;
    }

    /** Reset all counters to zero. */
    void reset_counters() {
        connect_call_count = 0;
        disconnect_call_count = 0;
        send_packet_call_count = 0;
        send_raw_call_count = 0;
        send_initialize_call_count = 0;
        send_passphrase_msg_call_count = 0;
        send_passphrase_str_call_count = 0;
        send_ping_call_count = 0;
        send_disconnect_call_count = 0;
        send_create_access_point_call_count = 0;
        send_create_access_point_private_call_count = 0;
        send_connect_call_count = 0;
        send_connect_private_call_count = 0;
        send_scan_call_count = 0;
        send_proxy_data_call_count = 0;
        send_set_accept_policy_call_count = 0;
        send_set_advertise_data_call_count = 0;
        send_reject_call_count = 0;
        recv_call_count = 0;
    }

    /**
     * @brief Tells whether a scripted packet is waiting.
     *
     * @return true while recv_queue is not empty
     */
    bool has_packet_available() const override {
        return !recv_queue.empty();
    }

    /**
     * @brief Accepts and ignores the TCP_NODELAY request.
     *
     * @return always Success
     */
    ClientResult set_nodelay(bool /*enable*/) override {
        return ClientResult::Success;
    }
};

}  // namespace ryu_ldn::network::test