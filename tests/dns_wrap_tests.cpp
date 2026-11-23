/**
 * @file dns_wrap_tests.cpp
 * @brief Unit tests for the real resolver wrappers (network/dns_wrap.cpp)
 *
 * The wrappers are driven through their public entry points
 * (__wrap_getaddrinfo, __wrap_getnameinfo, ...). The UDP exchange with the DNS
 * server is replaced by a scripted fake: this file defines socket(),
 * setsockopt(), sendto() and recvfrom(), which take precedence over libc for
 * the test binary, so no packet ever leaves the machine. malloc() is routed
 * through the linker's --wrap=malloc to inject allocation failures.
 * The EAI_* codes follow the target's convention (tests/stubs/netdb.h).
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_harness.hpp"

extern "C" {
/**
 * @brief Resolver entry point under test (linker --wrap of getaddrinfo)
 */
int __wrap_getaddrinfo(
    const char* node,
    const char* service,
    const struct addrinfo* hints,
    struct addrinfo** res);
/**
 * @brief Frees a list built by __wrap_getaddrinfo
 */
void __wrap_freeaddrinfo(struct addrinfo* res);
/**
 * @brief Numeric reverse lookup under test
 */
int __wrap_getnameinfo(
    const struct sockaddr* sa,
    socklen_t salen,
    char* host,
    socklen_t hostlen,
    char* serv,
    socklen_t servlen,
    int flags);
/**
 * @brief Error text of the wrapper
 */
const char* __wrap_gai_strerror(int ecode);

/**
 * @brief The real malloc, reached through --wrap=malloc
 *
 * @param size bytes to allocate
 * @return the allocation, or null on failure
 */
void* __real_malloc(size_t size);
}

// ---------------------------------------------------------------------------
// Allocation failure injection (only the resolver's AddrinfoStorage blocks)
// ---------------------------------------------------------------------------
namespace {
/**
 * @brief Size of one resolver result block (addrinfo plus sockaddr_in), used to
 *        recognize the allocations whose failure is injected
 */
constexpr size_t kStorageSize =
    sizeof(struct addrinfo) + sizeof(struct sockaddr_in);
/// Number of result block allocations seen since the test reset it
int g_storage_allocs = 0;
/// 1-based index of the storage allocation to fail; 0 = never
int g_storage_fail_at = 0;
}  // namespace

/**
 * @brief Allocator hook: fails the allocation of a result block when its index
 *        equals g_storage_fail_at, otherwise forwards to the real malloc
 *
 * @param size bytes requested
 * @return the allocation, or null for the injected failure
 */
extern "C" void* __wrap_malloc(size_t size) {
    if (size == kStorageSize && ++g_storage_allocs == g_storage_fail_at) {
        return nullptr;
    }
    return __real_malloc(size);
}

// ---------------------------------------------------------------------------
// Scripted DNS server
// ---------------------------------------------------------------------------
namespace {
/// State and script of the fake network under the resolver
struct FakeNet {
    /// When true, socket() fails with EMFILE
    bool socket_fails = false;
    /// When true, sendto() fails with ENETUNREACH
    bool sendto_fails = false;
    /// -2: return the scripted response; otherwise returned as is
    ssize_t recv_result = -2;
    /// Bytes returned by recvfrom() when recv_result is -2
    std::vector<uint8_t> response;
    /// Payload of the last query sent
    std::vector<uint8_t> last_query;
    /// Address (network order) the last query was sent to
    uint32_t last_server = 0;
    /// Number of sockets opened
    int sockets_opened = 0;
};
/// The fake network shared by the stubs and the tests
FakeNet g_net;

/// Restores g_net to its default (everything works, no response)
void reset_net() {
    g_net = FakeNet{};
}
}  // namespace

/**
 * @brief Fake socket(): hands out a real descriptor (/dev/null) so the code can
 *        close() it, or fails when g_net.socket_fails
 *
 * @return the descriptor, or -1 with errno EMFILE
 */
extern "C" int socket(int /*unused*/, int /*unused*/, int /*unused*/) {
    if (g_net.socket_fails) {
        errno = EMFILE;
        return -1;
    }
    ++g_net.sockets_opened;
    return ::open(
        "/dev/null", O_RDONLY);  // a real descriptor the code can close()
}

/**
 * @brief Fake setsockopt() that accepts every option
 *
 * @return 0
 */
extern "C" int setsockopt(
    int /*unused*/,
    int /*unused*/,
    int /*unused*/,
    const void* /*unused*/,
    socklen_t /*unused*/) {
    return 0;
}

/**
 * @brief Fake sendto(): records the query and its target in g_net, or fails
 *        when g_net.sendto_fails
 *
 * @param buf query payload
 * @param len payload size
 * @param to destination, a sockaddr_in
 * @return len, or -1 with errno ENETUNREACH
 */
extern "C" ssize_t sendto(
    int /*unused*/,
    const void* buf,
    size_t len,
    int /*unused*/,
    const struct sockaddr* to,
    socklen_t /*unused*/) {
    if (g_net.sendto_fails) {
        errno = ENETUNREACH;
        return -1;
    }
    const auto* p = static_cast<const uint8_t*>(buf);
    g_net.last_query.assign(p, p + len);
    g_net.last_server =
        reinterpret_cast<const struct sockaddr_in*>(to)->sin_addr.s_addr;
    return static_cast<ssize_t>(len);
}

/**
 * @brief Fake recvfrom(): returns g_net.recv_result unless it is -2, in which
 *        case it copies the scripted response
 *
 * @param buf receives the response
 * @param len capacity of buf
 * @return bytes copied, or the scripted value
 */
extern "C" ssize_t recvfrom(
    int /*unused*/,
    void* buf,
    size_t len,
    int /*unused*/,
    struct sockaddr* /*unused*/,
    socklen_t* /*unused*/) {
    if (g_net.recv_result != -2) {
        return g_net.recv_result;
    }
    const size_t n = g_net.response.size() < len ? g_net.response.size() : len;
    std::memcpy(buf, g_net.response.data(), n);
    return static_cast<ssize_t>(n);
}

// ---------------------------------------------------------------------------
// DNS message builders
// ---------------------------------------------------------------------------
namespace {
/// Raw bytes of a DNS message or part of one
using Bytes = std::vector<uint8_t>;

/**
 * @brief Appends a 16-bit value in network byte order
 *
 * @param b message being built
 * @param v value
 */
void put16(Bytes& b, uint16_t v) {
    b.push_back(static_cast<uint8_t>(v >> 8));
    b.push_back(static_cast<uint8_t>(v & 0xFF));
}

/**
 * @brief Builds a DNS header with id 0x1234 and no authority or additional
 *        records
 *
 * @param flags header flags word
 * @param qd question count
 * @param an answer count
 * @return the 12-byte header
 */
Bytes header(uint16_t flags, uint16_t qd, uint16_t an) {
    Bytes b;
    put16(b, 0x1234);
    put16(b, flags);
    put16(b, qd);
    put16(b, an);
    put16(b, 0);
    put16(b, 0);
    return b;
}

/**
 * @brief Appends bytes to a message
 *
 * @param b message being built
 * @param more bytes to add
 */
void append(Bytes& b, const Bytes& more) {
    b.insert(b.end(), more.begin(), more.end());
}

/**
 * @brief The question "www." + QTYPE A + QCLASS IN
 *
 * @return the encoded question section
 */
Bytes question() {
    return Bytes{3, 'w', 'w', 'w', 0, 0, 1, 0, 1};
}

/**
 * @brief A resource record whose name is a compression pointer to the question
 *
 * @param type record type
 * @param cls record class
 * @param rdata record data
 * @return the encoded record (TTL 60)
 */
Bytes rr(uint16_t type, uint16_t cls, const Bytes& rdata) {
    Bytes b{0xC0, 0x0C};
    put16(b, type);
    put16(b, cls);
    b.insert(b.end(), {0, 0, 0, 60});
    put16(b, static_cast<uint16_t>(rdata.size()));
    append(b, rdata);
    return b;
}

/**
 * @brief Builds an A record (type 1, class IN)
 *
 * @param a first address byte
 * @param b second address byte
 * @param c third address byte
 * @param d fourth address byte
 * @return the record
 */
Bytes a_record(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return rr(1, 1, Bytes{a, b, c, d});
}

/**
 * @brief Standard successful answer holding the given records
 *
 * @param records answer records, in order
 * @param flags header flags (default 0x8180: response, recursion available, no
 *        error)
 * @return the whole DNS message
 */
Bytes answer(const std::vector<Bytes>& records, uint16_t flags = 0x8180) {
    Bytes m = header(flags, 1, static_cast<uint16_t>(records.size()));
    append(m, question());
    for (const Bytes& r : records) {
        append(m, r);
    }
    return m;
}

/// Outcome of one resolve() call
struct Resolved {
    /// Return code of __wrap_getaddrinfo
    int rc;
    /// Dotted addresses of the returned list, in order
    std::vector<std::string> ips;
    /// ai_socktype of each returned entry
    std::vector<int> socktypes;
    /// Port (host order) of the last returned entry
    uint16_t port;
};

/**
 * @brief Resolves a host through the wrapper against the scripted response
 *
 * @param host name to resolve
 * @param response message the fake server returns
 * @param service service passed to getaddrinfo (default "80")
 * @return return code and the returned entries
 */
Resolved resolve(
    const char* host, const Bytes& response, const char* service = "80") {
    g_net.response = response;
    Resolved out{};
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    out.rc = __wrap_getaddrinfo(host, service, &hints, &res);
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET_ADDRSTRLEN];
        const auto* sin =
            reinterpret_cast<const struct sockaddr_in*>(p->ai_addr);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        out.ips.emplace_back(buf);
        out.socktypes.push_back(p->ai_socktype);
        out.port = ntohs(sin->sin_port);
    }
    __wrap_freeaddrinfo(res);
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------
// Entry-point validation and literals
// ---------------------------------------------------------------------------

/// Verifies a null result pointer gives EAI_SYSTEM with errno EINVAL.
TEST(a_null_result_pointer_is_a_system_error) {
    reset_net();
    errno = 0;
    ASSERT_EQ(
        __wrap_getaddrinfo("1.2.3.4", "80", nullptr, nullptr), EAI_SYSTEM);
    ASSERT_EQ(errno, EINVAL);
}

/**
 * @brief Verifies that neither node nor service gives EAI_NONAME and leaves the
 *        result pointer null.
 */
TEST(a_call_without_node_and_service_is_refused) {
    reset_net();
    auto* res = reinterpret_cast<struct addrinfo*>(0x1);
    ASSERT_EQ(__wrap_getaddrinfo(nullptr, nullptr, nullptr, &res), EAI_NONAME);
    ASSERT_TRUE(res == nullptr);
}

/// Verifies AF_INET6 hints give EAI_FAMILY while AF_UNSPEC is accepted.
TEST(only_ipv4_or_unspecified_families_are_accepted) {
    reset_net();
    struct addrinfo hints{};
    struct addrinfo* res = nullptr;
    hints.ai_family = AF_INET6;
    ASSERT_EQ(__wrap_getaddrinfo("1.2.3.4", "80", &hints, &res), EAI_FAMILY);
    hints.ai_family = AF_UNSPEC;
    ASSERT_EQ(__wrap_getaddrinfo("1.2.3.4", "80", &hints, &res), 0);
    __wrap_freeaddrinfo(res);
}

/**
 * @brief Verifies an IPv4 literal yields one entry with the hinted type,
 *        protocol and port and opens no socket.
 */
TEST(an_ipv4_literal_is_returned_without_touching_the_network) {
    reset_net();
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    struct addrinfo* res = nullptr;
    ASSERT_EQ(__wrap_getaddrinfo("239.255.255.250", "1900", &hints, &res), 0);
    ASSERT_TRUE(res != nullptr);
    ASSERT_TRUE(res->ai_next == nullptr);
    ASSERT_EQ(res->ai_family, AF_INET);
    ASSERT_EQ(res->ai_socktype, SOCK_DGRAM);
    ASSERT_EQ(res->ai_protocol, IPPROTO_UDP);
    ASSERT_EQ(
        res->ai_addrlen, static_cast<socklen_t>(sizeof(struct sockaddr_in)));
    ASSERT_TRUE(res->ai_canonname == nullptr);
    const auto* sin = reinterpret_cast<const struct sockaddr_in*>(res->ai_addr);
    ASSERT_EQ(ntohs(sin->sin_port), static_cast<uint16_t>(1900));
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
    ASSERT_TRUE(std::strcmp(buf, "239.255.255.250") == 0);
    __wrap_freeaddrinfo(res);
    ASSERT_EQ(g_net.sockets_opened, 0);
}

/**
 * @brief Verifies that without hints or service the entry has socket type 0,
 *        protocol 0 and port 0.
 */
TEST(a_literal_without_hints_or_service_gets_zero_defaults) {
    reset_net();
    struct addrinfo* res = nullptr;
    ASSERT_EQ(__wrap_getaddrinfo("10.0.0.1", nullptr, nullptr, &res), 0);
    ASSERT_EQ(res->ai_socktype, 0);
    ASSERT_EQ(res->ai_protocol, 0);
    ASSERT_EQ(
        reinterpret_cast<const struct sockaddr_in*>(res->ai_addr)->sin_port,
        static_cast<uint16_t>(0));
    __wrap_freeaddrinfo(res);
}

/**
 * @brief Verifies a null node resolves to loopback, or to the wildcard address
 *        when AI_PASSIVE is set.
 */
TEST(without_a_node_the_loopback_or_the_wildcard_address_is_used) {
    reset_net();
    struct addrinfo* res = nullptr;
    ASSERT_EQ(__wrap_getaddrinfo(nullptr, "8080", nullptr, &res), 0);
    ASSERT_EQ(
        reinterpret_cast<const struct sockaddr_in*>(res->ai_addr)
            ->sin_addr.s_addr,
        htonl(INADDR_LOOPBACK));
    __wrap_freeaddrinfo(res);

    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_PASSIVE;
    ASSERT_EQ(__wrap_getaddrinfo(nullptr, "8080", &hints, &res), 0);
    ASSERT_EQ(
        reinterpret_cast<const struct sockaddr_in*>(res->ai_addr)
            ->sin_addr.s_addr,
        htonl(INADDR_ANY));
    __wrap_freeaddrinfo(res);

    hints.ai_flags = 0;
    ASSERT_EQ(__wrap_getaddrinfo(nullptr, "8080", &hints, &res), 0);
    ASSERT_EQ(
        reinterpret_cast<const struct sockaddr_in*>(res->ai_addr)
            ->sin_addr.s_addr,
        htonl(INADDR_LOOPBACK));
    __wrap_freeaddrinfo(res);
}

/**
 * @brief Verifies a failed allocation of the single literal result gives
 *        EAI_MEMORY and a null list.
 */
TEST(an_allocation_failure_on_the_single_result_is_reported) {
    reset_net();
    struct addrinfo* res = nullptr;
    g_storage_allocs = 0;
    g_storage_fail_at = 1;
    int rc = __wrap_getaddrinfo("1.2.3.4", "80", nullptr, &res);
    g_storage_fail_at = 0;
    ASSERT_EQ(rc, EAI_MEMORY);
    ASSERT_TRUE(res == nullptr);
}

// ---------------------------------------------------------------------------
// Hostname resolution through the scripted DNS server
// ---------------------------------------------------------------------------

/**
 * @brief Verifies a host name yields every A record of the answer in order,
 *        with the hinted socket type and the port.
 */
TEST(a_hostname_is_resolved_to_every_a_record_in_order) {
    reset_net();
    Resolved r = resolve(
        "www.example.com",
        answer(
            {a_record(1, 2, 3, 4),
             a_record(5, 6, 7, 8),
             a_record(9, 10, 11, 12)}));
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.ips.size(), static_cast<size_t>(3));
    ASSERT_TRUE(r.ips[0] == "1.2.3.4");
    ASSERT_TRUE(r.ips[1] == "5.6.7.8");
    ASSERT_TRUE(r.ips[2] == "9.10.11.12");
    ASSERT_EQ(r.socktypes[2], SOCK_STREAM);
    ASSERT_EQ(r.port, static_cast<uint16_t>(80));
}

/**
 * @brief Verifies the query sent: recursion desired, one question, the
 *        length-prefixed name, type A, class IN, sent to 8.8.8.8, and a query
 *        id that advances by one per query.
 */
TEST(the_query_is_a_recursive_a_question_for_the_encoded_name) {
    reset_net();
    resolve("www.example.com", answer({a_record(1, 2, 3, 4)}));
    const Bytes& q = g_net.last_query;
    const Bytes name{
        3,
        'w',
        'w',
        'w',
        7,
        'e',
        'x',
        'a',
        'm',
        'p',
        'l',
        'e',
        3,
        'c',
        'o',
        'm',
        0};
    ASSERT_EQ(q.size(), 12 + name.size() + 4);
    ASSERT_EQ(q[2], 0x01);  // RD
    ASSERT_EQ(q[3], 0x00);
    ASSERT_EQ(q[4], 0x00);
    ASSERT_EQ(q[5], 0x01);  // QDCOUNT = 1
    ASSERT_TRUE(std::equal(name.begin(), name.end(), q.begin() + 12));
    ASSERT_EQ(q[q.size() - 4], 0x00);
    ASSERT_EQ(q[q.size() - 3], 0x01);  // QTYPE A
    ASSERT_EQ(q[q.size() - 2], 0x00);
    ASSERT_EQ(q[q.size() - 1], 0x01);  // QCLASS IN
    ASSERT_EQ(g_net.last_server, htonl(0x08080808));

    // The query id advances for every query.
    const auto first = static_cast<uint16_t>((q[0] << 8) | q[1]);
    resolve("www.example.com", answer({a_record(1, 2, 3, 4)}));
    const auto second =
        static_cast<uint16_t>((g_net.last_query[0] << 8) | g_net.last_query[1]);
    ASSERT_EQ(static_cast<uint16_t>(second - first), static_cast<uint16_t>(1));
}

/// Verifies a fully qualified name with a trailing dot resolves.
TEST(a_trailing_dot_is_accepted) {
    reset_net();
    Resolved r = resolve("example.com.", answer({a_record(1, 2, 3, 4)}));
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.ips.size(), static_cast<size_t>(1));
}

/// Verifies that 9 A records give only the first 8 entries.
TEST(at_most_eight_addresses_are_returned) {
    reset_net();
    std::vector<Bytes> records;
    for (uint8_t i = 1; i <= 9; ++i) {
        records.push_back(a_record(10, 0, 0, i));
    }
    Resolved r = resolve("www.example.com", answer(records));
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.ips.size(), static_cast<size_t>(8));
    ASSERT_TRUE(r.ips[7] == "10.0.0.8");
}

/**
 * @brief Verifies a CNAME, an A record of the wrong length and one of another
 *        class are skipped, leaving the real address.
 */
TEST(records_that_are_not_ipv4_internet_addresses_are_skipped) {
    reset_net();
    // CNAME (5), an A record of the wrong length, an A record of another class,
    // then the real address.
    Resolved r = resolve(
        "www.example.com",
        answer(
            {rr(5, 1, Bytes{1, 'x', 0}),
             rr(1, 1, Bytes{1, 2, 3, 4, 5, 6}),
             rr(1, 3, Bytes{1, 2, 3, 4}),
             a_record(7, 7, 7, 7)}));
    ASSERT_EQ(r.rc, 0);
    ASSERT_EQ(r.ips.size(), static_cast<size_t>(1));
    ASSERT_TRUE(r.ips[0] == "7.7.7.7");
}

/// Verifies an answer holding only unusable records gives EAI_NONAME.
TEST(an_answer_without_any_usable_record_is_a_name_error) {
    reset_net();
    ASSERT_EQ(
        resolve("www.example.com", answer({rr(5, 1, Bytes{1, 'x', 0})})).rc,
        EAI_NONAME);
}

/**
 * @brief Verifies the parser skips several questions and accepts an answer
 *        named by the root label instead of a pointer.
 */
TEST(several_questions_and_a_root_named_answer_are_walked) {
    reset_net();
    Bytes m = header(0x8180, 2, 1);
    append(m, question());
    append(m, question());
    // Answer named by the root label instead of a pointer.
    append(m, Bytes{0, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 4, 3, 2, 1});
    Resolved r = resolve("www.example.com", m);
    ASSERT_EQ(r.rc, 0);
    ASSERT_TRUE(r.ips[0] == "4.3.2.1");
}

/// Verifies a 63-byte label, the longest allowed, is accepted.
TEST(a_hostname_with_a_long_but_valid_label_is_resolved) {
    reset_net();
    const std::string host = std::string(63, 'a') + ".com";
    ASSERT_EQ(resolve(host.c_str(), answer({a_record(1, 2, 3, 4)})).rc, 0);
}

// ---------------------------------------------------------------------------
// Error mapping
// ---------------------------------------------------------------------------

/**
 * @brief Verifies NXDOMAIN and an empty answer give EAI_NONAME, SERVFAIL,
 *        REFUSED and FORMERR give EAI_FAIL, and a truncated response gives
 *        EAI_AGAIN.
 */
TEST(response_codes_map_to_the_matching_getaddrinfo_errors) {
    reset_net();
    ASSERT_EQ(
        resolve("www.example.com", answer({a_record(1, 2, 3, 4)}, 0x8183)).rc,
        EAI_NONAME);  // NXDOMAIN
    ASSERT_EQ(
        resolve("www.example.com", answer({a_record(1, 2, 3, 4)}, 0x8182)).rc,
        EAI_FAIL);  // SERVFAIL
    ASSERT_EQ(
        resolve("www.example.com", answer({a_record(1, 2, 3, 4)}, 0x8185)).rc,
        EAI_FAIL);  // REFUSED
    ASSERT_EQ(
        resolve("www.example.com", answer({a_record(1, 2, 3, 4)}, 0x8181)).rc,
        EAI_FAIL);  // FORMERR
    ASSERT_EQ(
        resolve("www.example.com", answer({a_record(1, 2, 3, 4)}, 0x8380)).rc,
        EAI_AGAIN);  // truncated
    ASSERT_EQ(
        resolve("www.example.com", answer({}, 0x8180)).rc,
        EAI_NONAME);  // no answer
}

/// Verifies a message without the response bit gives EAI_AGAIN.
TEST(a_message_that_is_not_a_response_is_retryable) {
    reset_net();
    ASSERT_EQ(
        resolve("www.example.com", answer({a_record(1, 2, 3, 4)}, 0x0100)).rc,
        EAI_AGAIN);
}

/**
 * @brief Verifies a failed socket gives EAI_FAIL, while a failed send, a
 *        receive error, an empty receive and a message shorter than a header
 *        give EAI_AGAIN.
 */
TEST(transport_failures_are_retryable) {
    reset_net();
    g_net.socket_fails = true;
    ASSERT_EQ(
        resolve("www.example.com", {}).rc,
        EAI_FAIL);  // no socket: not worth retrying

    reset_net();
    g_net.sendto_fails = true;
    ASSERT_EQ(resolve("www.example.com", {}).rc, EAI_AGAIN);

    reset_net();
    g_net.recv_result = -1;  // receive timeout
    ASSERT_EQ(resolve("www.example.com", {}).rc, EAI_AGAIN);

    reset_net();
    g_net.recv_result = 0;
    ASSERT_EQ(resolve("www.example.com", {}).rc, EAI_AGAIN);

    reset_net();
    ASSERT_EQ(
        resolve("www.example.com", Bytes{1, 2, 3, 4, 5}).rc,
        EAI_AGAIN);  // shorter than a header
}

/**
 * @brief Verifies empty names, empty labels, a label over 63 bytes and an
 *        over-long name give EAI_FAIL without opening a socket.
 */
TEST(names_that_cannot_be_encoded_never_reach_the_network) {
    reset_net();
    const Bytes ok = answer({a_record(1, 2, 3, 4)});
    const std::string long_label(64, 'a');
    std::string too_long;
    for (int i = 0; i < 5; ++i) {
        too_long += std::string(62, 'b') + ".";
    }
    ASSERT_EQ(resolve("", ok).rc, EAI_FAIL);
    ASSERT_EQ(resolve("a..b", ok).rc, EAI_FAIL);
    ASSERT_EQ(resolve(".a", ok).rc, EAI_FAIL);
    ASSERT_EQ(resolve(long_label.c_str(), ok).rc, EAI_FAIL);
    ASSERT_EQ(resolve(too_long.c_str(), ok).rc, EAI_FAIL);
    ASSERT_EQ(g_net.sockets_opened, 0);
}

/**
 * @brief Verifies truncated or unterminated names, trailers, record headers,
 *        record data and a cut second answer give EAI_AGAIN instead of reading
 *        out of bounds.
 */
TEST(malformed_responses_are_rejected_as_retryable) {
    reset_net();
    // question name whose label runs past the end
    Bytes m = header(0x8180, 1, 1);
    append(m, Bytes{10, 'a'});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);

    // question name ending inside a compression pointer
    m = header(0x8180, 1, 1);
    append(m, Bytes{0xC0});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);

    // question name never terminated
    m = header(0x8180, 1, 1);
    append(m, Bytes{1, 'a'});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);

    // question trailer (QTYPE/QCLASS) cut short
    m = header(0x8180, 1, 1);
    append(m, Bytes{0, 0, 1});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);

    // answer record header cut short
    m = header(0x8180, 0, 1);
    append(m, Bytes{0, 0, 1, 0, 1});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);

    // answer whose data runs past the end
    m = header(0x8180, 0, 1);
    append(m, Bytes{0, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 1, 2});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);

    // second answer cut short after a valid first one
    m = header(0x8180, 1, 2);
    append(m, question());
    append(m, a_record(1, 2, 3, 4));
    append(m, Bytes{0xC0});
    ASSERT_EQ(resolve("www.example.com", m).rc, EAI_AGAIN);
}

/**
 * @brief Verifies that failing the first or second result block allocation
 *        gives EAI_MEMORY and a null list (what was built is freed).
 */
TEST(an_allocation_failure_while_building_the_list_frees_what_was_built) {
    reset_net();
    const Bytes three = answer(
        {a_record(1, 2, 3, 4), a_record(5, 6, 7, 8), a_record(9, 9, 9, 9)});
    for (int fail_at = 1; fail_at <= 2; ++fail_at) {
        g_net.response = three;
        struct addrinfo* res = nullptr;
        g_storage_allocs = 0;
        g_storage_fail_at = fail_at;
        int rc = __wrap_getaddrinfo("www.example.com", "80", nullptr, &res);
        g_storage_fail_at = 0;
        ASSERT_EQ(rc, EAI_MEMORY);
        ASSERT_TRUE(res == nullptr);
    }
}

// ---------------------------------------------------------------------------
// getnameinfo / freeaddrinfo / gai_strerror
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Builds an IPv4 socket address 192.168.1.20:port
 *
 * @param port port, host order
 * @return the address
 */
struct sockaddr_in make_sin(uint16_t port) {
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(AF_INET, "192.168.1.20", &sa.sin_addr);
    return sa;
}
}  // namespace

/// Verifies __wrap_getnameinfo writes the dotted address and the port as text.
TEST(getnameinfo_formats_the_numeric_host_and_service) {
    struct sockaddr_in sa = make_sin(1900);
    char host[32];
    char serv[8];
    ASSERT_EQ(
        __wrap_getnameinfo(
            reinterpret_cast<struct sockaddr*>(&sa),
            sizeof(sa),
            host,
            sizeof(host),
            serv,
            sizeof(serv),
            0),
        0);
    ASSERT_TRUE(std::strcmp(host, "192.168.1.20") == 0);
    ASSERT_TRUE(std::strcmp(serv, "1900") == 0);
}

/**
 * @brief Verifies a null buffer or a zero length leaves that output untouched
 *        while the call succeeds.
 */
TEST(getnameinfo_fills_only_the_outputs_it_is_given) {
    struct sockaddr_in sa = make_sin(80);
    char buf[32] = "untouched";
    ASSERT_EQ(
        __wrap_getnameinfo(
            reinterpret_cast<struct sockaddr*>(&sa),
            sizeof(sa),
            nullptr,
            0,
            buf,
            0,
            0),
        0);
    ASSERT_TRUE(std::strcmp(buf, "untouched") == 0);
    ASSERT_EQ(
        __wrap_getnameinfo(
            reinterpret_cast<struct sockaddr*>(&sa),
            sizeof(sa),
            buf,
            0,
            nullptr,
            8,
            0),
        0);
    ASSERT_TRUE(std::strcmp(buf, "untouched") == 0);
}

/**
 * @brief Verifies a null address, a too short length and a non-IPv4 family give
 *        EAI_FAMILY, and a 4-byte host buffer gives EAI_OVERFLOW.
 */
TEST(getnameinfo_refuses_bad_addresses_and_small_buffers) {
    struct sockaddr_in sa = make_sin(80);
    char host[32];
    ASSERT_EQ(
        __wrap_getnameinfo(
            nullptr, sizeof(sa), host, sizeof(host), nullptr, 0, 0),
        EAI_FAMILY);
    ASSERT_EQ(
        __wrap_getnameinfo(
            reinterpret_cast<struct sockaddr*>(&sa),
            4,
            host,
            sizeof(host),
            nullptr,
            0,
            0),
        EAI_FAMILY);
    sa.sin_family = AF_INET6;
    ASSERT_EQ(
        __wrap_getnameinfo(
            reinterpret_cast<struct sockaddr*>(&sa),
            sizeof(sa),
            host,
            sizeof(host),
            nullptr,
            0,
            0),
        EAI_FAMILY);
    sa.sin_family = AF_INET;
    ASSERT_EQ(
        __wrap_getnameinfo(
            reinterpret_cast<struct sockaddr*>(&sa),
            sizeof(sa),
            host,
            4,
            nullptr,
            0,
            0),
        EAI_OVERFLOW);
}

/**
 * @brief Verifies freeaddrinfo(nullptr) is harmless and gai_strerror always
 *        returns "dns error".
 */
TEST(freeing_a_null_list_is_harmless_and_the_error_string_is_constant) {
    __wrap_freeaddrinfo(nullptr);
    ASSERT_TRUE(std::strcmp(__wrap_gai_strerror(EAI_NONAME), "dns error") == 0);
    ASSERT_TRUE(std::strcmp(__wrap_gai_strerror(0), "dns error") == 0);
}

/**
 * @brief Runs every registered test of this suite
 *
 * @return 1 if at least one test failed, 0 otherwise
 */
int main() {
    return test_report("DnsWrap Tests");
}
