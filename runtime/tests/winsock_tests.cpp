// winsock_tests.cpp - Winsock (WSOCK32, WS2_32), the IP helper API and
// Netbios, through the guest ABI: every call is pushed on a guest stack and
// reached through imports_dispatch, so a wrong stdcall count shows up as an
// unbalanced stack. Sockets are real host sockets on the loopback interface;
// nothing leaves the machine. Where a test needs an interface list (adapter
// order, broadcast fan-out) it installs a fake one whose broadcast addresses
// are the loopback address, so a "broadcast" lands back here.
//
// RECOMP_NET_TEST_BROADCAST=1 also sends one real limited broadcast on the
// host's own interfaces (a junk payload to a random high port) and checks
// that it comes back through the wildcard socket.
#include "../imports.h"
#include "../loader.h"
#include "../memory.h"
#include "../net_adapters.h"
#include "../win32.h"
#include "../winsock.h"
#include "../../platform/os.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[768];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    if (!ok || getenv("WINSOCK_TESTS_VERBOSE"))
        printf("  [%s] %s\n", ok ? "ok" : "FAIL", msg);
    return ok;
}

// ---------------------------------------------------------------------------
// Guest calls.
// ---------------------------------------------------------------------------
static X86 g_main;
static uint32_t call_target(X86 *c, uint32_t target, const std::vector<uint32_t> &args) {
    const uint32_t before = c->r[R_ESP];
    uint32_t esp = before;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0x00401000u);
    c->r[R_ESP] = esp;
    if (!imports_dispatch(c, target)) {
        check(false, "%08x is not a shim trampoline", target);
        c->r[R_ESP] = before;
        return 0;
    }
    const uint32_t expected =
        imports_argc(target) == ARGC_CDECL ? before - uint32_t(args.size()) * 4 : before;
    check(c->r[R_ESP] == expected, "%s leaves ESP balanced",
          imports_describe(target) ? imports_describe(target) : "shim");
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}
static uint32_t call(X86 *c, const char *dll, const char *name, const std::vector<uint32_t> &args) {
    const uint32_t target = imports_resolve(dll, name);
    if (!target) {
        check(false, "no trampoline for %s!%s", dll, name);
        return 0;
    }
    return call_target(c, target, args);
}
static uint32_t ws(const char *name, const std::vector<uint32_t> &args, X86 *c = &g_main) {
    return call(c, "WS2_32.dll", name, args);
}
static uint32_t w1(const char *name, const std::vector<uint32_t> &args, X86 *c = &g_main) {
    return call(c, "WSOCK32.dll", name, args);
}
static uint32_t k32(const char *name, const std::vector<uint32_t> &args, X86 *c = &g_main) {
    return call(c, "KERNEL32.dll", name, args);
}
static uint32_t ip_hlp(const char *name, const std::vector<uint32_t> &args) {
    return call(&g_main, "IPHLPAPI.DLL", name, args);
}
static uint32_t last_error(X86 *c = &g_main) {
    return ws("WSAGetLastError", {}, c);
}

// Scratch memory: one bump region per test, reset by each.
static uint32_t g_scratch = 0, g_top = 0;
static uint32_t gbuf(uint32_t n) {
    uint32_t a = g_top;
    memset(g_mem + a, 0, n);
    g_top += (n + 15) & ~15u;
    return a;
}
static uint32_t gstr(const std::string &s) {
    uint32_t a = gbuf((uint32_t)s.size() + 1);
    memcpy(g_mem + a, s.c_str(), s.size() + 1);
    return a;
}
static void reset_scratch() {
    g_top = g_scratch;
}

const uint32_t SOCKET_ERROR = 0xffffffffu, INVALID_SOCKET = 0xffffffffu;
const uint32_t AF_INET = 2, SOCK_STREAM = 1, SOCK_DGRAM = 2, SOL_SOCKET = 0xffff;
const uint32_t SO_REUSEADDR = 4, SO_BROADCAST = 0x20, SO_RCVTIMEO = 0x1006, SO_ERROR = 0x1007;
const uint32_t SO_TYPE = 0x1008;
const uint32_t FIONBIO = 0x8004667e, FIONREAD = 0x4004667f;
const uint32_t FD_READ = 1, FD_WRITE = 2, FD_ACCEPT = 8, FD_CONNECT = 0x10, FD_CLOSE = 0x20;
const uint32_t WAIT_TIMEOUT = 0x102;
const uint32_t LOOPBACK = 0x7f000001u;

static uint32_t swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
// A SOCKADDR_IN in guest memory.
static uint32_t sin(uint32_t ip, uint16_t port) {
    uint32_t a = gbuf(16);
    wr16(a, AF_INET);
    wr8(a + 2, (uint8_t)(port >> 8));
    wr8(a + 3, (uint8_t)port);
    wr32(a + 4, swap32(ip));
    return a;
}
static uint16_t sin_port(uint32_t a) {
    return (uint16_t)(rd8(a + 2) << 8 | rd8(a + 3));
}
static uint32_t sin_ip(uint32_t a) {
    return swap32(rd32(a + 4));
}
static uint32_t udp(X86 *c = &g_main) {
    return ws("socket", {AF_INET, SOCK_DGRAM, 17}, c);
}
static uint32_t tcp(X86 *c = &g_main) {
    return ws("socket", {AF_INET, SOCK_STREAM, 0}, c);
}
static bool bind_to(uint32_t s, uint32_t ip, uint16_t port) {
    return ws("bind", {s, sin(ip, port), 16}) == 0;
}
static uint16_t port_of(uint32_t s) {
    uint32_t a = gbuf(16), len = gbuf(4);
    wr32(len, 16);
    if (ws("getsockname", {s, a, len}) != 0)
        return 0;
    return sin_port(a);
}
static uint32_t sendto_(uint32_t s, const std::string &data, uint32_t ip, uint16_t port) {
    uint32_t b = gstr(data);
    return ws("sendto", {s, b, (uint32_t)data.size(), 0, sin(ip, port), 16});
}
static uint32_t set_nonblocking(uint32_t s, bool on) {
    uint32_t v = gbuf(4);
    wr32(v, on ? 1 : 0);
    return ws("ioctlsocket", {s, FIONBIO, v});
}
static uint32_t set_opt(uint32_t s, uint32_t level, uint32_t name, uint32_t value,
                        bool winsock1 = false) {
    uint32_t v = gbuf(4);
    wr32(v, value);
    return winsock1 ? w1("setsockopt", {s, level, name, v, 4})
                    : ws("setsockopt", {s, level, name, v, 4});
}
static uint32_t get_opt(uint32_t s, uint32_t level, uint32_t name, bool winsock1 = false) {
    uint32_t v = gbuf(4), len = gbuf(4);
    wr32(len, 4);
    uint32_t r = winsock1 ? w1("getsockopt", {s, level, name, v, len})
                          : ws("getsockopt", {s, level, name, v, len});
    return r == 0 ? rd32(v) : 0xdeadbeefu;
}
// A datagram received now or never: non-blocking, so a test cannot hang.
static std::string recv_now(uint32_t s, uint32_t *from_ip = nullptr,
                            uint16_t *from_port = nullptr) {
    uint32_t b = gbuf(2048), from = gbuf(16), len = gbuf(4);
    wr32(len, 16);
    uint32_t n = ws("recvfrom", {s, b, 2048, 0, from, len});
    if (n == SOCKET_ERROR)
        return "<error " + std::to_string(last_error()) + ">";
    if (from_ip)
        *from_ip = sin_ip(from);
    if (from_port)
        *from_port = sin_port(from);
    return std::string((const char *)g_mem + b, n);
}
static double now_ms() {
    return (double)os_monotonic_ns() / 1e6;
}
// Datagrams a socket has queued, read until it would block.
static int drain(uint32_t s) {
    set_nonblocking(s, true);
    int n = 0;
    while (recv_now(s).compare(0, 6, "<error") != 0)
        ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Start-up, errors and addresses.
// ---------------------------------------------------------------------------
static void test_startup() {
    reset_scratch();
    uint32_t data = gbuf(400);
    check(ws("socket", {AF_INET, SOCK_DGRAM, 0}) == INVALID_SOCKET && last_error() == 10093,
          "socket before WSAStartup: WSANOTINITIALISED");
    check(ws("WSAStartup", {0x0002, data}) == 0 && rd8(data) == 2 && rd8(data + 1) == 0,
          "WSAStartup(2.0) grants 2.0 (a plugin checks wVersion == 2.0)");
    check(rd16(data + 2) == 0x0202, "wHighVersion 2.2");
    check(w1("WSAStartup", {0x0101, data}) == 0 && rd16(data) == 0x0101,
          "WSOCK32 WSAStartup(1.1) grants 1.1");
    check(ws("WSAStartup", {0x0303, data}) == 0 && rd16(data) == 0x0202, "3.3 is granted 2.2");
    check(ws("WSAStartup", {0x0000, data}) == 10092, "0.0: WSAVERNOTSUPPORTED");
    check(ws("WSACleanup", {}) == 0, "WSACleanup with start-ups left");
    // Three start-ups less one: a clean-up and a start-up mid-session keep
    // every socket.
    uint32_t s = udp();
    check(s != INVALID_SOCKET && bind_to(s, LOOPBACK, 0), "a socket after start-up");
    check(ws("WSACleanup", {}) == 0 && ws("WSACleanup", {}) == 0, "two more clean-ups");
    check(ws("WSACleanup", {}) == SOCKET_ERROR && last_error() == 10093,
          "a clean-up too many: WSANOTINITIALISED");
    check(ws("WSAStartup", {0x0002, data}) == 0 && port_of(s) != 0,
          "the socket survives clean-up and start-up again");
    ws("closesocket", {s});
    check(ws("WSAStartup", {0x0101, data}) == 0, "the count stays above zero for the rest");
}

static void test_errors_and_addresses() {
    reset_scratch();
    check(winsock_error_from_errno(EWOULDBLOCK) == 10035, "EWOULDBLOCK -> WSAEWOULDBLOCK");
    check(winsock_error_from_errno(ECONNREFUSED) == 10061, "ECONNREFUSED -> WSAECONNREFUSED");
    check(winsock_error_from_errno(EADDRINUSE) == 10048, "EADDRINUSE -> WSAEADDRINUSE");
    check(winsock_error_from_errno(ECONNRESET) == 10054, "ECONNRESET -> WSAECONNRESET");
    check(winsock_error_from_errno(EPIPE) == 10054, "EPIPE -> WSAECONNRESET");
    check(winsock_error_from_errno(ETIMEDOUT) == 10060, "ETIMEDOUT -> WSAETIMEDOUT");
    check(winsock_error_from_errno(EHOSTUNREACH) == 10065, "EHOSTUNREACH -> WSAEHOSTUNREACH");
    check(winsock_error_from_errno(EMSGSIZE) == 10040, "EMSGSIZE -> WSAEMSGSIZE");
    check(winsock_error_from_errno(EBADF) == 10038, "EBADF -> WSAENOTSOCK");

    check(ws("htons", {0x8236}) == 0x3682 && ws("ntohs", {0x3682}) == 0x8236, "htons/ntohs");
    check(ws("htonl", {0x7f000001}) == 0x0100007f && w1("ntohl", {0x0100007f}) == 0x7f000001,
          "htonl/ntohl");
    struct {
        const char *text;
        uint32_t want; // network order, as inet_addr returns it
    } cases[] = {{"192.168.1.227", 0xe301a8c0u},
                 {"127.0.0.1", 0x0100007fu},
                 {"255.255.255.255", 0xffffffffu},
                 {"10.1", 0x0100000au},
                 {"0x7f.1", 0x0100007fu},
                 {"010.0.0.1", 0x01000008u},
                 {"1.2.3.4 ", 0x04030201u},
                 {"Mac.mynet", 0xffffffffu},
                 {"", 0xffffffffu},
                 {"1.2.3.256", 0xffffffffu},
                 {"1.2.3.4.5", 0xffffffffu},
                 {"128.127.126.125", 0x7d7e7f80u}};
    for (auto &k : cases) {
        uint32_t got = ws("inet_addr", {gstr(k.text)});
        check(got == k.want, "inet_addr(\"%s\") = %08x (want %08x)", k.text, got, k.want);
    }
    check(ws("inet_addr", {0}) == 0xffffffffu, "inet_addr(NULL) = INADDR_NONE");
    uint32_t a = ws("inet_ntoa", {0xe301a8c0u});
    std::string first = a ? gm_str(a) : "";
    uint32_t b = ws("inet_ntoa", {0x0100007fu});
    check(first == "192.168.1.227" && a == b && gm_str(b) == "127.0.0.1",
          "inet_ntoa: one buffer per thread, overwritten by the next call");
    // GetProcAddress by ordinal: "#N", each DLL's own numbering.
    check(w1("#11", {0x0100007fu}) == b, "WSOCK32 ordinal 11 is inet_ntoa");
    check(ws("#11", {gstr("1.2.3.4")}) == 0x04030201u, "WS2_32 ordinal 11 is inet_addr");
    check(w1("#10", {gstr("1.2.3.4")}) == 0x04030201u, "WSOCK32 ordinal 10 is inet_addr");
    uint32_t v = gbuf(4);
    wr32(v, 1);
    check(ws("#10", {0x1234, FIONBIO, v}) == SOCKET_ERROR && last_error() == 10038,
          "WS2_32 ordinal 10 is ioctlsocket");
}

static void test_invalid_sockets() {
    reset_scratch();
    uint32_t v = gbuf(4);
    wr32(v, 1);
    // A lobby that sets SO_BROADCAST on a socket it failed to create.
    check(w1("setsockopt", {INVALID_SOCKET, SOL_SOCKET, SO_BROADCAST, v, 4}) == SOCKET_ERROR &&
              last_error() == 10038,
          "setsockopt(INVALID_SOCKET) fails with WSAENOTSOCK");
    check(ws("closesocket", {0x1234}) == SOCKET_ERROR && last_error() == 10038,
          "closesocket of no socket: WSAENOTSOCK");
    check(ws("recv", {0x1234, gbuf(4), 4, 0}) == SOCKET_ERROR && last_error() == 10038,
          "recv on no socket: WSAENOTSOCK");
    check(ws("socket", {23, SOCK_DGRAM, 0}) == INVALID_SOCKET && last_error() == 10047,
          "AF_INET6: WSAEAFNOSUPPORT");
    check(ws("socket", {AF_INET, SOCK_DGRAM, 6}) == INVALID_SOCKET && last_error() == 10043,
          "UDP with IPPROTO_TCP: WSAEPROTONOSUPPORT");
    check(ws("socket", {AF_INET, 3, 0}) == INVALID_SOCKET && last_error() == 10044,
          "SOCK_RAW: WSAESOCKTNOSUPPORT");
    uint32_t s = udp();
    uint32_t a = gbuf(16), len = gbuf(4);
    wr32(len, 16);
    check(ws("getsockname", {s, a, len}) == SOCKET_ERROR && last_error() == 10022,
          "getsockname before bind: WSAEINVAL");
    check(get_opt(s, SOL_SOCKET, SO_TYPE) == SOCK_DGRAM, "SO_TYPE");
    check(get_opt(s, SOL_SOCKET, 0x7777) == 0xdeadbeefu && last_error() == 10042,
          "an unknown option: WSAENOPROTOOPT");
    check(ws("closesocket", {s}) == 0, "closesocket");
    check(winsock_open_sockets() == 0, "no socket left open");
}

// ---------------------------------------------------------------------------
// Datagrams and streams on the loopback interface.
// ---------------------------------------------------------------------------
static void test_udp_echo() {
    reset_scratch();
    uint32_t a = udp(), b = udp();
    check(bind_to(a, LOOPBACK, 0) && bind_to(b, 0, 0), "bind a to 127.0.0.1:0, b to *:0");
    uint16_t pa = port_of(a), pb = port_of(b);
    check(pa && pb && pa != pb, "ephemeral ports %u and %u", pa, pb);
    check(sendto_(b, "ping", LOOPBACK, pa) == 4, "sendto returns the length");
    uint32_t from = 0;
    uint16_t from_port = 0;
    std::string got = recv_now(a, &from, &from_port); // blocking: the datagram is queued
    check(got == "ping" && from == LOOPBACK && from_port == pb,
          "recvfrom: the datagram and its sender (%s from %08x:%u)", got.c_str(), from, from_port);
    check(sendto_(a, "pong", from, from_port) == 4 && recv_now(b) == "pong", "and back");
    // Zero-length and truncated datagrams.
    check(sendto_(b, "", LOOPBACK, pa) == 0 && recv_now(a) == "", "a zero-length datagram");
    std::string big(100, 'x');
    big[0] = 'A';
    sendto_(b, big, LOOPBACK, pa);
    uint32_t buf = gbuf(16);
    check(ws("recv", {a, buf, 10, 0}) == SOCKET_ERROR && last_error() == 10040 && rd8(buf) == 'A' &&
              rd8(buf + 9) == 'x',
          "a datagram longer than the buffer: WSAEMSGSIZE, the part that fit kept");
    set_nonblocking(a, true);
    check(ws("recv", {a, buf, 10, 0}) == SOCKET_ERROR && last_error() == 10035,
          "and the rest of it is gone (WSAEWOULDBLOCK)");
    // Second binds.
    uint32_t c = udp();
    check(ws("bind", {c, sin(LOOPBACK, pa), 16}) == SOCKET_ERROR && last_error() == 10048,
          "a second bind of the port: WSAEADDRINUSE");
    check(ws("bind", {a, sin(LOOPBACK, 0), 16}) == SOCKET_ERROR && last_error() == 10022,
          "binding a bound socket: WSAEINVAL");
    uint32_t d = udp(), e = udp();
    check(set_opt(d, SOL_SOCKET, SO_REUSEADDR, 1) == 0 &&
              set_opt(e, SOL_SOCKET, SO_REUSEADDR, 1) == 0,
          "SO_REUSEADDR on two sockets");
    bind_to(d, LOOPBACK, 0);
    check(ws("bind", {e, sin(LOOPBACK, port_of(d)), 16}) == 0, "which may then share a port");
    check(get_opt(d, SOL_SOCKET, SO_REUSEADDR) == 1, "getsockopt reads SO_REUSEADDR back");
    // Broadcast needs SO_BROADCAST.
    check(sendto_(b, "x", 0xffffffffu, pa) == SOCKET_ERROR && last_error() == 10013,
          "a broadcast without SO_BROADCAST: WSAEACCES");
    for (uint32_t s : {a, b, c, d, e})
        ws("closesocket", {s});
}

static void test_tcp_echo() {
    reset_scratch();
    uint32_t l = tcp(), cl = tcp();
    check(bind_to(l, LOOPBACK, 0) && ws("listen", {l, 0x7fffffff}) == 0,
          "listen(SOMAXCONN) on a bound socket");
    uint16_t lp = port_of(l);
    uint32_t other = tcp();
    check(ws("bind", {other, sin(LOOPBACK, lp), 16}) == SOCKET_ERROR && last_error() == 10048,
          "a second bind of a listening port: WSAEADDRINUSE");
    check(ws("connect", {cl, sin(LOOPBACK, lp), 16}) == 0, "blocking connect");
    uint32_t peer = gbuf(16), len = gbuf(4);
    wr32(len, 16);
    uint32_t s = ws("accept", {l, peer, len});
    check(s != INVALID_SOCKET && rd32(len) == 16 && sin_ip(peer) == LOOPBACK &&
              sin_port(peer) == port_of(cl),
          "accept: the peer's address");
    uint32_t msg = gstr("hello");
    check(ws("send", {cl, msg, 5, 0}) == 5, "send");
    uint32_t buf = gbuf(64);
    check(ws("recv", {s, buf, 64, 0}) == 5 && gm_str(buf) == "hello", "recv");
    check(ws("send", {s, gstr("world!"), 6, 0}) == 6, "send back");
    check(ws("recv", {cl, buf, 3, 2 /* MSG_PEEK */}) == 3, "MSG_PEEK");
    check(ws("recv", {cl, buf, 64, 0}) == 6, "the peeked bytes are still there");
    uint32_t n = gbuf(4);
    check(ws("getpeername", {s, peer, len}) == 0 && sin_port(peer) == port_of(cl), "getpeername");
    check(ws("connect", {cl, sin(LOOPBACK, lp), 16}) == SOCKET_ERROR && last_error() == 10056,
          "connect again: WSAEISCONN");
    check(ws("shutdown", {cl, 1}) == 0, "shutdown(SD_SEND)");
    check(ws("recv", {s, buf, 64, 0}) == 0, "the other end reads the end of the stream");
    check(ws("send", {cl, msg, 5, 0}) == SOCKET_ERROR && last_error() == 10058,
          "send after shutdown: WSAESHUTDOWN");
    check(ws("ioctlsocket", {s, FIONREAD, n}) == 0 && rd32(n) == 0, "FIONREAD");
    for (uint32_t k : {l, cl, s, other})
        ws("closesocket", {k});
}

static void test_nonblocking() {
    reset_scratch();
    uint32_t u = udp();
    bind_to(u, LOOPBACK, 0);
    set_nonblocking(u, true);
    double t0 = now_ms();
    check(ws("recvfrom", {u, gbuf(16), 16, 0, 0, 0}) == SOCKET_ERROR && last_error() == 10035,
          "recvfrom with nothing queued: WSAEWOULDBLOCK");
    check(now_ms() - t0 < 50, "at once");
    uint32_t l = tcp();
    bind_to(l, LOOPBACK, 0);
    ws("listen", {l, 5});
    set_nonblocking(l, true);
    check(ws("accept", {l, 0, 0}) == INVALID_SOCKET && last_error() == 10035,
          "accept with nobody waiting: WSAEWOULDBLOCK");
    // A non-blocking connect: in progress, then writable.
    uint32_t c = tcp();
    set_nonblocking(c, true);
    uint32_t r = ws("connect", {c, sin(LOOPBACK, port_of(l)), 16});
    uint32_t err = last_error();
    check(r == 0 || err == 10035, "non-blocking connect: 0 or WSAEWOULDBLOCK (%u)", err);
    uint32_t wset = gbuf(8), tv = gbuf(8);
    wr32(wset, 1);
    wr32(wset + 4, c);
    wr32(tv, 2);
    wr32(tv + 4, 0);
    check(ws("select", {0, 0, wset, 0, tv}) == 1 && ws("__WSAFDIsSet", {c, wset}) == 1,
          "select: writable once connected");
    check(ws("connect", {c, sin(LOOPBACK, port_of(l)), 16}) == SOCKET_ERROR &&
              last_error() == 10056,
          "connect again: WSAEISCONN");
    uint32_t s = ws("accept", {l, 0, 0});
    check(s != INVALID_SOCKET, "accept now has a connection");
    // A connection refused: a port nobody listens on.
    uint32_t gone = tcp();
    bind_to(gone, LOOPBACK, 0);
    uint16_t dead = port_of(gone);
    ws("closesocket", {gone});
    uint32_t f = tcp();
    set_nonblocking(f, true);
    ws("connect", {f, sin(LOOPBACK, dead), 16});
    uint32_t eset = gbuf(8);
    wr32(eset, 1);
    wr32(eset + 4, f);
    wr32(wset, 1);
    wr32(wset + 4, f);
    uint32_t ready = ws("select", {0, 0, wset, eset, tv});
    check(ready == 1 && rd32(eset) == 1 && rd32(wset) == 0,
          "a refused connect is in the except set, not the write set (%u)", ready);
    check(get_opt(f, SOL_SOCKET, SO_ERROR) == 10061, "SO_ERROR: WSAECONNREFUSED");
    uint32_t g = tcp();
    check(ws("connect", {g, sin(LOOPBACK, dead), 16}) == SOCKET_ERROR && last_error() == 10061,
          "a blocking connect to it: WSAECONNREFUSED");
    // select with nothing ready waits out its timeout.
    uint32_t rset = gbuf(8);
    wr32(rset, 1);
    wr32(rset + 4, u);
    wr32(tv, 0);
    wr32(tv + 4, 100000);
    t0 = now_ms();
    check(ws("select", {0, rset, 0, 0, tv}) == 0 && rd32(rset) == 0, "select times out");
    double waited = now_ms() - t0;
    check(waited >= 90 && waited < 1000, "after its timeout (%.0f ms)", waited);
    check(ws("select", {0, 0, 0, 0, tv}) == SOCKET_ERROR && last_error() == 10022,
          "select with no sockets: WSAEINVAL");
    // And returns as soon as something arrives.
    uint32_t sender = udp();
    sendto_(sender, "x", LOOPBACK, port_of(u));
    wr32(rset, 1);
    wr32(rset + 4, u);
    wr32(tv, 5);
    check(ws("select", {0, rset, 0, 0, tv}) == 1 && w1("__WSAFDIsSet", {u, rset}) == 1,
          "select: readable");
    // SO_RCVTIMEO bounds a blocking receive.
    uint32_t t = udp();
    bind_to(t, LOOPBACK, 0);
    set_opt(t, SOL_SOCKET, SO_RCVTIMEO, 100);
    t0 = now_ms();
    check(ws("recv", {t, gbuf(4), 4, 0}) == SOCKET_ERROR && last_error() == 10060,
          "SO_RCVTIMEO: WSAETIMEDOUT");
    waited = now_ms() - t0;
    check(waited >= 90 && waited < 1000, "after 100 ms (%.0f ms)", waited);
    for (uint32_t k : {u, l, c, s, f, g, sender, t})
        ws("closesocket", {k});
}

// ---------------------------------------------------------------------------
// WSAEventSelect.
// ---------------------------------------------------------------------------
static uint32_t make_event(bool manual) {
    return k32("CreateEventA", {0, manual ? 1u : 0u, 0, 0});
}
static uint32_t wait(uint32_t ev, uint32_t ms) {
    return k32("WaitForSingleObject", {ev, ms});
}

static void test_event_select() {
    reset_scratch();
    uint32_t ev = make_event(false); // auto-reset, as a server waiting on it uses
    uint32_t r = udp(), s = udp();
    bind_to(r, 0, 0);
    uint16_t pr = port_of(r);
    check(ws("WSAEventSelect", {r, ev, FD_READ}) == 0, "WSAEventSelect(FD_READ)");
    check(wait(ev, 0) == WAIT_TIMEOUT, "nothing to read: not signalled");
    for (int i = 0; i < 3; ++i)
        sendto_(s, "dgram" + std::to_string(i), LOOPBACK, pr);
    check(wait(ev, 2000) == 0, "signalled when datagrams arrive");
    // One datagram per wake-up: the event comes back while more are queued.
    for (int i = 0; i < 3; ++i) {
        std::string got = recv_now(r);
        check(got == "dgram" + std::to_string(i), "datagram %d (%s)", i, got.c_str());
        uint32_t w = wait(ev, 0);
        if (i < 2)
            check(w == 0, "re-signalled after reading %d of 3", i + 1);
        else
            check(w == WAIT_TIMEOUT, "quiet once the queue is empty");
    }
    check(ws("recv", {r, gbuf(8), 8, 0}) == SOCKET_ERROR && last_error() == 10035,
          "WSAEventSelect made the socket non-blocking");
    uint32_t v = gbuf(4);
    wr32(v, 0);
    check(ws("ioctlsocket", {r, FIONBIO, v}) == SOCKET_ERROR && last_error() == 10022,
          "and FIONBIO cannot make it blocking while selected");
    // A datagram that arrives while the guest waits wakes it at once.
    std::thread late([pr] {
        os_sleep_us(50000);
        int fd = os_net_socket(OS_NET_DGRAM);
        os_net_sendto(fd, "late", 4, 0, LOOPBACK, pr, 0);
        os_net_close(fd);
    });
    double t0 = now_ms();
    uint32_t w = wait(ev, 3000);
    double took = now_ms() - t0;
    late.join();
    check(w == 0 && took < 1000, "a datagram during the wait signals it (%.0f ms)", took);
    check(recv_now(r) == "late", "and it is there");
    // A later selection replaces the earlier: the plugin's listening socket
    // is selected for FD_READ on one event, then FD_ACCEPT on another.
    uint32_t ev_a = make_event(false), ev_b = make_event(false);
    uint32_t l = tcp();
    bind_to(l, LOOPBACK, 0);
    ws("listen", {l, 0x7fffffff});
    ws("WSAEventSelect", {l, ev_a, FD_READ});
    ws("WSAEventSelect", {l, ev_b, FD_ACCEPT});
    uint32_t c1 = tcp(), c2 = tcp();
    set_nonblocking(c1, true);
    set_nonblocking(c2, true);
    ws("connect", {c1, sin(LOOPBACK, port_of(l)), 16});
    ws("connect", {c2, sin(LOOPBACK, port_of(l)), 16});
    check(wait(ev_b, 2000) == 0, "FD_ACCEPT signals the second event");
    check(wait(ev_a, 0) == WAIT_TIMEOUT, "not the first");
    uint32_t a1 = ws("accept", {l, 0, 0});
    check(a1 != INVALID_SOCKET, "accept one");
    check(wait(ev_b, 1000) == 0, "FD_ACCEPT again while another connection waits");
    uint32_t a2 = ws("accept", {l, 0, 0});
    check(a2 != INVALID_SOCKET && wait(ev_b, 0) == WAIT_TIMEOUT, "accept the other; quiet");
    // An accepted socket inherits the listening socket's selection.
    uint32_t netev = gbuf(44);
    ws("WSAEventSelect", {l, ev_b, FD_ACCEPT | FD_READ});
    uint32_t c3 = tcp();
    check(ws("connect", {c3, sin(LOOPBACK, port_of(l)), 16}) == 0, "a third client connects");
    uint32_t a3 = INVALID_SOCKET;
    for (int i = 0; i < 100 && a3 == INVALID_SOCKET; ++i) {
        wait(ev_b, 100);
        a3 = ws("accept", {l, 0, 0});
    }
    check(a3 != INVALID_SOCKET, "accepted");
    ws("WSAEnumNetworkEvents", {a3, ev_b, netev});
    ws("send", {c3, gstr("abc"), 3, 0});
    check(wait(ev_b, 2000) == 0, "the accepted socket signals the inherited event");
    check(ws("WSAEnumNetworkEvents", {a3, ev_b, netev}) == 0 && rd32(netev) == FD_READ,
          "with FD_READ (%#x)", rd32(netev));
    // FD_CONNECT, FD_WRITE, FD_READ and FD_CLOSE through WSAEnumNetworkEvents.
    uint32_t ec = make_event(true);
    uint32_t cl = tcp();
    check(ws("WSAEventSelect", {cl, ec, FD_CONNECT | FD_WRITE | FD_READ | FD_CLOSE}) == 0,
          "select a client for connect/write/read/close");
    uint32_t cr = ws("connect", {cl, sin(LOOPBACK, port_of(l)), 16});
    check(cr == 0 || last_error() == 10035, "connect on a selected socket does not block");
    check(wait(ec, 2000) == 0, "the connect is signalled");
    uint32_t sv = INVALID_SOCKET;
    for (int i = 0; i < 100 && sv == INVALID_SOCKET; ++i) {
        sv = ws("accept", {l, 0, 0});
        if (sv == INVALID_SOCKET)
            k32("Sleep", {10});
    }
    check(ws("WSAEnumNetworkEvents", {cl, ec, netev}) == 0, "WSAEnumNetworkEvents");
    uint32_t got = rd32(netev);
    check((got & FD_CONNECT) && rd32(netev + 4 + 4 * 4) == 0, "FD_CONNECT with no error (%#x)",
          got);
    check((got & FD_WRITE) != 0, "FD_WRITE once connected");
    check(wait(ec, 0) == WAIT_TIMEOUT, "the event was reset");
    ws("send", {sv, gstr("data"), 4, 0});
    check(wait(ec, 2000) == 0, "data signals");
    ws("WSAEnumNetworkEvents", {cl, ec, netev});
    check(rd32(netev) == FD_READ, "FD_READ (%#x)", rd32(netev));
    uint32_t buf = gbuf(16);
    check(ws("recv", {cl, buf, 16, 0}) == 4, "read it");
    ws("closesocket", {sv});
    check(wait(ec, 2000) == 0, "the peer's close signals");
    ws("WSAEnumNetworkEvents", {cl, ec, netev});
    check((rd32(netev) & FD_CLOSE) && rd32(netev + 4 + 4 * 5) == 0, "FD_CLOSE, gracefully (%#x)",
          rd32(netev));
    check(ws("WSAEventSelect", {cl, 0, 0}) == 0, "cancel the selection");
    for (uint32_t k : {r, s, l, c1, c2, c3, a1, a2, a3, cl})
        ws("closesocket", {k});
    for (uint32_t e : {ev, ev_a, ev_b, ec})
        k32("CloseHandle", {e});
    check(winsock_open_sockets() == 0, "no socket left open");
}

// ---------------------------------------------------------------------------
// Blocking calls wait as guest waits: other guest threads run meanwhile.
// ---------------------------------------------------------------------------
static uint32_t g_block_sock = 0;
static std::atomic<int> g_block_state{0};
static std::string g_block_got;
static void blocking_reader(X86 *c) {
    g_block_state = 1;
    uint32_t buf = g_scratch + 0x8000;
    uint32_t n = ws("recv", {g_block_sock, buf, 64, 0}, c);
    g_block_got = n == SOCKET_ERROR ? "<error " + std::to_string(last_error(c)) + ">"
                                    : std::string((const char *)g_mem + buf, n);
    g_block_state = 2;
    set_eax(c, 0);
}
static void test_blocking_threads() {
    reset_scratch();
    g_block_sock = udp();
    bind_to(g_block_sock, LOOPBACK, 0);
    uint32_t body = imports_alloc_trampoline("test", "blocking_reader", blocking_reader, 1);
    uint32_t th = k32("CreateThread", {0, 0, body, 0, 0, 0});
    for (int i = 0; i < 100 && g_block_state < 1; ++i)
        k32("Sleep", {1});
    k32("Sleep", {50});
    check(g_block_state == 1, "the reader blocks in recv while this thread runs");
    uint32_t s = udp();
    sendto_(s, "wake", LOOPBACK, port_of(g_block_sock));
    check(wait(th, 2000) == 0 && g_block_state == 2 && g_block_got == "wake",
          "and returns with the datagram (%s)", g_block_got.c_str());
    // A socket closed under a blocked call ends the call.
    g_block_state = 0;
    th = k32("CreateThread", {0, 0, body, 0, 0, 0});
    for (int i = 0; i < 100 && g_block_state < 1; ++i)
        k32("Sleep", {1});
    k32("Sleep", {20});
    ws("closesocket", {g_block_sock});
    check(wait(th, 2000) == 0 && g_block_state == 2 && g_block_got.compare(0, 6, "<error") == 0,
          "closesocket ends a blocked recv (%s)", g_block_got.c_str());
    ws("closesocket", {s});
}

// ---------------------------------------------------------------------------
// The adapter list, the IP helper API and broadcast fan-out, on fake
// interfaces.
// ---------------------------------------------------------------------------
static OsNetInterface iface(const char *name, unsigned index, uint32_t ip, uint32_t mask,
                            uint32_t broadcast, bool up = true, bool p2p = false,
                            bool loopback = false) {
    OsNetInterface f;
    memset(&f, 0, sizeof f);
    snprintf(f.name, sizeof f.name, "%s", name);
    f.index = index;
    f.ip = ip;
    f.netmask = mask;
    f.broadcast = broadcast;
    f.up = up;
    f.running = up;
    f.loopback = loopback;
    f.point_to_point = p2p;
    f.can_broadcast = !p2p && !loopback;
    if (!p2p && !loopback) {
        f.mac_len = 6;
        uint8_t mac[6] = {0x02, 0x00, 0x5e, 0x10, 0x00, (uint8_t)index};
        memcpy(f.mac, mac, 6);
    }
    f.mtu = 1500;
    return f;
}

static std::string names(const std::vector<NetAdapter> &v) {
    std::string s;
    for (const NetAdapter &a : v)
        s += (s.empty() ? "" : ",") + a.name;
    return s;
}

static void test_adapter_rules() {
    std::vector<OsNetInterface> f = {
        iface("lo0", 1, LOOPBACK, 0xff000000u, 0, true, false, true),
        iface("en0", 4, 0x0a000002u, 0xffffff00u, 0x0a0000ffu),
        iface("utun4", 20, 0x64400001u, 0xffffffffu, 0, true, true),
        iface("bridge100", 30, 0xc0a84001u, 0xffffff00u, 0xc0a840ffu),
        iface("feth1234", 25, 0x0a930001u, 0xffffff00u, 0x0a9300ffu),
        iface("en1", 5, 0xc0a801e3u, 0xffffff00u, 0xc0a801ffu),
        iface("en2", 6, 0xc0a80202u, 0xffffff00u, 0xc0a802ffu, false), // down
    };
    // en1 carries the default route: it comes first although en0 has the
    // lower index.
    std::vector<NetAdapter> v = net_adapters_from(f.data(), (int)f.size(), 5, 0xc0a80101u, nullptr);
    check(names(v) == "en1,en0,feth1234,utun4,bridge100",
          "order: LAN (default route first), ZeroTier, tunnel, other; no lo0, no down (%s)",
          names(v).c_str());
    check(v[0].default_route && v[0].gateway == 0xc0a80101u && !v[1].gateway,
          "the default route's gateway on its adapter");
    // Twelve LAN interfaces: ten reported.
    std::vector<OsNetInterface> many;
    for (unsigned i = 0; i < 12; ++i) {
        char n[8];
        snprintf(n, sizeof n, "en%u", i);
        many.push_back(iface(n, 10 + i, 0x0a000001u + (i << 8), 0xffffff00u, 0));
    }
    v = net_adapters_from(many.data(), (int)many.size(), 0, 0, nullptr);
    check(v.size() == 10 && v[0].name == "en0" && v[9].name == "en9", "at most ten adapters");
    // A second address on the first adapter takes 40 bytes of the 6,400.
    many.insert(many.begin() + 1, iface("en0", 10, 0x0a0000feu, 0xffffff00u, 0));
    v = net_adapters_from(many.data(), (int)many.size(), 0, 0, nullptr);
    check(v.size() == 9 && v[0].addresses.size() == 2,
          "an adapter with two addresses: nine fit the buffer (%zu)", v.size());
    // The override.
    v = net_adapters_from(f.data(), (int)f.size(), 5, 0, "feth1234,en0,lo0");
    check(names(v) == "feth1234,en0", "RECOMP_NET_ADAPTERS picks and orders (%s)",
          names(v).c_str());
    // The host's own list obeys the rules too.
    net_set_interfaces_for_test(nullptr, -1, 0, 0);
    std::vector<NetAdapter> host = net_adapters();
    bool sane = host.size() <= 10;
    for (const NetAdapter &a : host)
        sane =
            sane && !a.addresses.empty() && a.addresses[0].ip && (a.addresses[0].ip >> 24) != 127;
    check(sane, "the host's adapters: none loopback, all with IPv4 (%s)", names(host).c_str());
    for (const NetAdapter &a : host)
        printf("  host adapter %s #%u %s%s\n", a.name.c_str(), a.index,
               net_dotted(a.addresses[0].ip).c_str(), a.default_route ? " (default route)" : "");
}

// The fake interfaces the remaining tests run on: two LAN adapters and a
// ZeroTier one, each broadcasting to the loopback address, and a tunnel.
static std::vector<OsNetInterface> fake_interfaces() {
    std::vector<OsNetInterface> f = {
        iface("en0", 4, 0x0a630001u, 0xffffff00u, LOOPBACK),
        iface("en0", 4, 0x0a630002u, 0xffffff00u, LOOPBACK), // a second address
        iface("en1", 5, 0x0a640001u, 0xffff0000u, LOOPBACK),
        iface("feth7", 9, 0x0a930001u, 0xffffff00u, LOOPBACK),
        iface("lo0", 1, LOOPBACK, 0xff000000u, 0, true, false, true),
    };
    return f;
}

static void test_iphlpapi() {
    reset_scratch();
    std::vector<OsNetInterface> f = fake_interfaces();
    net_set_interfaces_for_test(f.data(), (int)f.size(), 4, 0x0a6300feu);
    uint32_t lenp = gbuf(4);
    wr32(lenp, 0);
    check(ip_hlp("GetAdaptersInfo", {0, lenp}) == 111, "a NULL buffer: ERROR_BUFFER_OVERFLOW");
    check(rd32(lenp) == 3 * 640 + 40, "and the size: three records and one more address (%u)",
          rd32(lenp));
    // The fixed 6,400-byte buffer a plugin of the period passes.
    uint32_t buf = gbuf(6400);
    wr32(lenp, 6400);
    check(ip_hlp("GetAdaptersInfo", {buf, lenp}) == 0, "a 6,400-byte buffer is enough");
    uint32_t a0 = buf, a1 = rd32(a0), a2 = a1 ? rd32(a1) : 0;
    check(a1 == buf + 640 && a2 == buf + 1280 && rd32(a2) == 0, "three records chained by Next");
    check(gm_str(a0 + 268).find("en0") != std::string::npos &&
              gm_str(a2 + 268).find("ZeroTier") != std::string::npos,
          "readable descriptions (%s / %s)", gm_str(a0 + 268).c_str(), gm_str(a2 + 268).c_str());
    check(gm_str(a0 + 432) == "10.99.0.1" && gm_str(a0 + 448) == "255.255.255.0",
          "IpAddressList.IpAddress.String and IpMask");
    uint32_t second = rd32(a0 + 428);
    check(second && gm_str(second + 4) == "10.99.0.2", "the second address chained after them");
    check(rd32(a0 + 412) == 4 && rd32(a0 + 400) == 6 && rd8(a0 + 404 + 5) == 4,
          "Index and the hardware address");
    check(gm_str(a0 + 472) == "10.99.0.254" && gm_str(a1 + 472) == "0.0.0.0",
          "the gateway on the default route's adapter");
    check(gm_str(a0 + 8).size() == 38 && gm_str(a0 + 8)[0] == '{', "AdapterName is a GUID");
    // gethostbyname(gethostname()) answers the same addresses in the same
    // order, so a lookup of an adapter's address by position agrees.
    uint32_t name = gbuf(256);
    check(ws("gethostname", {name, 256}) == 0 && gm_str(name).size() > 0, "gethostname (%s)",
          gm_str(name).c_str());
    uint32_t he = ws("gethostbyname", {name});
    std::vector<uint32_t> listed;
    for (uint32_t i = 0, p; he && (p = rd32(rd32(he + 12) + 4 * i)) != 0; ++i)
        listed.push_back(swap32(rd32(p)));
    std::vector<uint32_t> want = {0x0a630001u, 0x0a630002u, 0x0a640001u, 0x0a930001u};
    check(listed == want, "the own host's addresses: every adapter's, in adapter order");
    check(rd16(he + 8) == 2 && rd16(he + 10) == 4, "h_addrtype AF_INET, h_length 4");
    // The plugin's walk: adapter k (1-based), its first address, its position
    // in the host entry.
    for (uint32_t k = 1; k <= 3; ++k) {
        uint32_t rec = buf;
        for (uint32_t i = 1; i < k; ++i)
            rec = rd32(rec);
        uint32_t ip = ws("inet_addr", {rec + 432});
        uint32_t pos = 0;
        while (pos < listed.size() && swap32(listed[pos]) != ip)
            ++pos;
        check(pos < listed.size(), "adapter %u's address is in the host entry", k);
    }
    // A buffer too small by one byte.
    wr32(lenp, 3 * 640 + 39);
    check(ip_hlp("GetAdaptersInfo", {buf, lenp}) == 111 && rd32(lenp) == 3 * 640 + 40,
          "one byte short: ERROR_BUFFER_OVERFLOW and the size");
    // GetAdaptersAddresses.
    wr32(lenp, 0);
    check(ip_hlp("GetAdaptersAddresses", {2, 0, 0, 0, lenp}) == 111 && rd32(lenp) > 0,
          "GetAdaptersAddresses: the size first");
    uint32_t gaa = gbuf(rd32(lenp));
    check(ip_hlp("GetAdaptersAddresses", {0, 0, 0, gaa, lenp}) == 0, "then the list");
    uint32_t uni = rd32(gaa + 16);
    check(rd32(gaa) == 144 && rd32(gaa + 4) == 4 && uni && rd32(uni) == 48 &&
              swap32(rd32(rd32(uni + 12) + 4)) == 0x0a630001u && rd8(uni + 44) == 24,
          "IP_ADAPTER_ADDRESSES: index, first unicast address, prefix length");
    check(gm_wstr(rd32(gaa + 40)) == "en0", "FriendlyName");
    check(ip_hlp("GetAdaptersAddresses", {23, 0, 0, gaa, lenp}) == 232, "IPv6: ERROR_NO_DATA");
    // GetIpForwardTable.
    wr32(lenp, 0);
    check(ip_hlp("GetIpForwardTable", {0, lenp, 1}) == 122, "GetIpForwardTable: the size first");
    uint32_t ft = gbuf(rd32(lenp));
    check(ip_hlp("GetIpForwardTable", {ft, lenp, 1}) == 0 && rd32(ft) > 0, "then the table");
    check(rd32(ft + 4) == 0 && rd32(ft + 4 + 4) == 0 && swap32(rd32(ft + 4 + 12)) == 0x0a6300feu &&
              rd32(ft + 4 + 16) == 4,
          "sorted, the default route first, through the gateway on en0");
    uint32_t idx = gbuf(4);
    check(ip_hlp("GetBestInterface", {swap32(LOOPBACK), idx}) == 0 && rd32(idx) == 1,
          "GetBestInterface(127.0.0.1): the loopback interface");
    check(ip_hlp("GetBestInterface", {swap32(0x0a930042u), idx}) == 0 && rd32(idx) == 9,
          "GetBestInterface on the ZeroTier subnet: feth7");
    uint32_t route = gbuf(56);
    check(ip_hlp("GetBestRoute", {swap32(0x0a640505u), 0, route}) == 0 && rd32(route + 16) == 5 &&
              swap32(rd32(route + 4)) == 0xffff0000u,
          "GetBestRoute on en1's /16");
    // GetIpAddrTable and the interface list.
    wr32(lenp, 0);
    ip_hlp("GetIpAddrTable", {0, lenp, 0});
    uint32_t at = gbuf(rd32(lenp));
    check(ip_hlp("GetIpAddrTable", {at, lenp, 0}) == 0 && rd32(at) == 5 &&
              swap32(rd32(at + 4 + 24 * 4)) == LOOPBACK,
          "GetIpAddrTable: four addresses and the loopback one");
    uint32_t s = udp();
    uint32_t list = gbuf(76 * 8), ret = gbuf(4);
    check(ws("WSAIoctl", {s, 0x4004747f, 0, 0, list, 76 * 8, ret, 0, 0}) == 0 &&
              rd32(ret) == 5 * 76 && (rd32(list) & 2) && swap32(rd32(list + 4 + 4)) == 0x0a630001u,
          "SIO_GET_INTERFACE_LIST");
    ws("closesocket", {s});
    // Netbios: one LANA per adapter with a hardware address.
    uint32_t ncb = gbuf(64), lana = gbuf(256);
    wr8(ncb, 0x37); // NCBENUM
    wr32(ncb + 4, lana);
    wr16(ncb + 8, 256);
    check(call(&g_main, "NETAPI32.dll", "Netbios", {ncb}) == 0 && rd8(lana) == 3, "NCBENUM: 3");
    memset(g_mem + ncb, 0, 64);
    wr8(ncb, 0x32); // NCBRESET
    wr8(ncb + 48, 2);
    check(call(&g_main, "NETAPI32.dll", "Netbios", {ncb}) == 0, "NCBRESET");
    uint32_t st = gbuf(600);
    memset(g_mem + ncb, 0, 64);
    wr8(ncb, 0x33); // NCBASTAT
    wr8(ncb + 48, 1);
    wr32(ncb + 4, st);
    wr16(ncb + 8, 600);
    memcpy(g_mem + ncb + 10, "*               ", 16);
    check(call(&g_main, "NETAPI32.dll", "Netbios", {ncb}) == 0 && rd8(st) == 0x02 &&
              rd8(st + 5) == 5 && rd8(ncb + 1) == 0,
          "NCBASTAT: LANA 1 is en1's hardware address");
    wr8(ncb + 48, 7);
    check(call(&g_main, "NETAPI32.dll", "Netbios", {ncb}) == 0x23, "a LANA too many: NRC_BRIDGE");
    // No interfaces at all.
    net_set_interfaces_for_test(nullptr, 0, 0, 0);
    check(ip_hlp("GetAdaptersInfo", {buf, lenp}) == 232, "no adapters: ERROR_NO_DATA");
    he = ws("gethostbyname", {name});
    check(he && swap32(rd32(rd32(rd32(he + 12)))) == LOOPBACK && rd32(rd32(he + 12) + 4) == 0,
          "and the own host is the loopback address");
    net_set_interfaces_for_test(nullptr, -1, 0, 0);
}

static void test_broadcast() {
    reset_scratch();
    std::vector<OsNetInterface> f = fake_interfaces();
    net_set_interfaces_for_test(f.data(), (int)f.size(), 4, 0);
    uint32_t r = udp(), s = udp();
    bind_to(r, LOOPBACK, 0);
    uint16_t pr = port_of(r);
    check(set_opt(s, SOL_SOCKET, SO_BROADCAST, 1) == 0, "SO_BROADCAST");
    check(sendto_(s, "query", 0xffffffffu, pr) == 5, "a broadcast to 255.255.255.255");
    k32("Sleep", {50});
    check(drain(r) == 3, "is sent once per adapter: en0, en1 and the ZeroTier one");
    // A socket bound to one adapter's address broadcasts on that adapter only,
    // and reports that address, although the host socket is on the wildcard.
    uint32_t b = udp();
    set_opt(b, SOL_SOCKET, SO_BROADCAST, 1);
    check(bind_to(b, 0x0a640001u, 0), "bind to en1's address");
    uint32_t name = gbuf(16), len = gbuf(4);
    wr32(len, 16);
    ws("getsockname", {b, name, len});
    check(sin_ip(name) == 0x0a640001u, "getsockname reports it");
    check(sendto_(b, "advert", 0xffffffffu, pr) == 6, "a broadcast from it");
    k32("Sleep", {50});
    check(drain(r) == 1, "leaves through en1 alone");
    // A directed broadcast reaches its own adapter.
    check(sendto_(s, "direct", LOOPBACK, pr) == 6, "a unicast is a unicast");
    k32("Sleep", {20});
    check(drain(r) == 1, "one copy");
    // What an interface-bound socket accepts: datagrams to its address only,
    // here none, so a datagram to 127.0.0.1 on its port is not delivered.
    sendto_(s, "stray", LOOPBACK, port_of(b));
    k32("Sleep", {20});
    set_nonblocking(b, true);
    check(ws("recv", {b, gbuf(16), 16, 0}) == SOCKET_ERROR && last_error() == 10035,
          "an interface-bound socket ignores a datagram to another local address");
    for (uint32_t k : {r, s, b})
        ws("closesocket", {k});
    net_set_interfaces_for_test(nullptr, -1, 0, 0);

    if (!recomp_env("NET_TEST_BROADCAST") || atoi(recomp_env("NET_TEST_BROADCAST")) == 0)
        return;
    // One real limited broadcast on the host's interfaces, which loop a copy
    // back to the wildcard socket that listens for it.
    std::vector<NetAdapter> host = net_adapters();
    int capable = 0;
    for (const NetAdapter &a : host)
        capable += a.can_broadcast ? 1 : 0;
    uint32_t lr = udp(), ls = udp();
    bind_to(lr, 0, 0);
    set_opt(ls, SOL_SOCKET, SO_BROADCAST, 1);
    uint16_t port = port_of(lr);
    check(sendto_(ls, "recomp-kit winsock_tests broadcast", 0xffffffffu, port) == 34,
          "a real broadcast on %d adapter(s)", capable);
    k32("Sleep", {200});
    int got = drain(lr);
    check(got == capable, "came back once per broadcast-capable adapter (%d of %d)", got, capable);
    ws("closesocket", {lr});
    ws("closesocket", {ls});
}

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------
static uint32_t g_other_hostent = 0;
static std::atomic<int> g_ticks{0};
static std::atomic<bool> g_ticking{false};
static void hostent_thread(X86 *c) {
    uint32_t name = g_scratch + 0x9000;
    gm_put_str(name, "10.7.7.7", 16);
    g_other_hostent = ws("gethostbyname", {name}, c);
    set_eax(c, 0);
}
static void ticker(X86 *c) {
    while (g_ticking) {
        ++g_ticks;
        k32("Sleep", {1}, c);
    }
    set_eax(c, 0);
}
static int slow_resolver(const char *name, uint32_t *ips, int cap, char *canonical, size_t cc) {
    os_sleep_us(150000);
    if (strcmp(name, "lan-host.test") != 0)
        return OS_NET_RESOLVE_NOT_FOUND;
    ips[0] = 0x0a141e28u;
    snprintf(canonical, cc, "lan-host.test");
    return 1;
}
static void test_names() {
    reset_scratch();
    std::vector<OsNetInterface> f = fake_interfaces();
    net_set_interfaces_for_test(f.data(), (int)f.size(), 4, 0);
    uint32_t a = ws("gethostbyname", {gstr("10.1.2.3")});
    check(a && swap32(rd32(rd32(rd32(a + 12)))) == 0x0a010203u && gm_str(rd32(a)) == "10.1.2.3",
          "a dotted address resolves to itself");
    uint32_t b = ws("gethostbyname", {gstr("10.4.5.6")});
    check(a == b && swap32(rd32(rd32(rd32(a + 12)))) == 0x0a040506u,
          "the HOSTENT is the thread's: the next call overwrites it");
    uint32_t body = imports_alloc_trampoline("test", "hostent_thread", hostent_thread, 1);
    uint32_t th = k32("CreateThread", {0, 0, body, 0, 0, 0});
    wait(th, 2000);
    check(g_other_hostent && g_other_hostent != a &&
              swap32(rd32(rd32(rd32(g_other_hostent + 12)))) == 0x0a070707u &&
              swap32(rd32(rd32(rd32(a + 12)))) == 0x0a040506u,
          "another thread has its own, and leaves this one alone");
    uint32_t lh = ws("gethostbyname", {gstr("LocalHost")});
    check(lh && swap32(rd32(rd32(rd32(lh + 12)))) == LOOPBACK, "localhost");
    // A name that must fail at once.
    os_setenv("RECOMP_NET_OFFLINE_HOSTS", "dead.server.example, other.example");
    double t0 = now_ms();
    check(ws("gethostbyname", {gstr("DEAD.server.example")}) == 0 && last_error() == 11001,
          "an offline host: WSAHOST_NOT_FOUND");
    check(now_ms() - t0 < 50, "at once (%.1f ms)", now_ms() - t0);
    os_unsetenv("RECOMP_NET_OFFLINE_HOSTS");
    // The resolver runs off the baton: another guest thread keeps running
    // while this one waits for it.
    winsock_set_resolver_for_test(slow_resolver);
    g_ticking = true;
    g_ticks = 0;
    uint32_t tick_body = imports_alloc_trampoline("test", "ticker", ticker, 1);
    uint32_t tt = k32("CreateThread", {0, 0, tick_body, 0, 0, 0});
    k32("Sleep", {5});
    int before = g_ticks;
    uint32_t r = ws("gethostbyname", {gstr("lan-host.test")});
    int during = g_ticks - before;
    check(r && swap32(rd32(rd32(rd32(r + 12)))) == 0x0a141e28u, "a name the resolver answers");
    check(during >= 20, "another guest thread ran while it resolved (%d ticks)", during);
    check(ws("gethostbyname", {gstr("nowhere.test")}) == 0 && last_error() == 11001,
          "a name it does not: WSAHOST_NOT_FOUND");
    winsock_set_resolve_timeout_for_test(50);
    t0 = now_ms();
    check(ws("gethostbyname", {gstr("lan-host.test")}) == 0 && last_error() == 11002,
          "a resolver slower than the timeout: WSATRY_AGAIN");
    check(now_ms() - t0 < 140, "after the timeout (%.0f ms)", now_ms() - t0);
    winsock_set_resolve_timeout_for_test(0);
    os_setenv("RECOMP_NET_DNS", "0");
    t0 = now_ms();
    check(ws("gethostbyname", {gstr("lan-host.test")}) == 0 && last_error() == 11001 &&
              now_ms() - t0 < 50,
          "RECOMP_NET_DNS=0: every other name fails at once");
    os_unsetenv("RECOMP_NET_DNS");
    winsock_set_resolver_for_test(nullptr);
    g_ticking = false;
    wait(tt, 2000);
    // gethostbyaddr answers without a reverse lookup.
    uint32_t addr = gbuf(4);
    wr32(addr, swap32(0x0a640001u));
    uint32_t ha = ws("gethostbyaddr", {addr, 4, AF_INET});
    uint32_t hn = gbuf(256);
    ws("gethostname", {hn, 256});
    check(ha && gm_str(rd32(ha)) == gm_str(hn), "gethostbyaddr of an own address: the host name");
    wr32(addr, swap32(0xc0a80105u));
    ha = ws("gethostbyaddr", {addr, 4, AF_INET});
    check(ha && gm_str(rd32(ha)) == "192.168.1.5" &&
              swap32(rd32(rd32(rd32(ha + 12)))) == 0xc0a80105u,
          "of another address: the address itself, at once");
    check(ws("gethostname", {hn, 1}) == SOCKET_ERROR && last_error() == 10014,
          "gethostname into a buffer too small: WSAEFAULT");
    net_set_interfaces_for_test(nullptr, -1, 0, 0);
}

// ---------------------------------------------------------------------------
// The two DLLs: every export registered, and the IP option numbers that
// differ between them.
// ---------------------------------------------------------------------------
static void test_registration() {
    reset_scratch();
    const char *common[] = {"WSAStartup",
                            "WSACleanup",
                            "WSAGetLastError",
                            "WSASetLastError",
                            "__WSAFDIsSet",
                            "accept",
                            "bind",
                            "closesocket",
                            "connect",
                            "getpeername",
                            "getsockname",
                            "getsockopt",
                            "htonl",
                            "htons",
                            "inet_addr",
                            "inet_ntoa",
                            "ioctlsocket",
                            "listen",
                            "ntohl",
                            "ntohs",
                            "recv",
                            "recvfrom",
                            "select",
                            "send",
                            "sendto",
                            "setsockopt",
                            "shutdown",
                            "socket",
                            "gethostbyaddr",
                            "gethostbyname",
                            "gethostname"};
    int missing = 0;
    for (const char *dll : {"WSOCK32.dll", "WS2_32.dll"})
        for (const char *n : common)
            if (!imports_has_shim(dll, n)) {
                ++missing;
                printf("  missing %s!%s\n", dll, n);
            }
    for (const char *n : {"WSAEventSelect", "WSAEnumNetworkEvents", "WSACreateEvent",
                          "WSAWaitForMultipleEvents", "WSAIoctl", "WSARecvFrom", "WSASendTo"})
        if (!imports_has_shim("WS2_32.dll", n)) {
            ++missing;
            printf("  missing WS2_32.dll!%s\n", n);
        }
    for (const char *n : {"GetAdaptersInfo", "GetAdaptersAddresses", "GetIpForwardTable",
                          "GetBestInterface", "GetBestRoute"})
        missing += imports_has_shim("iphlpapi.dll", n) ? 0 : 1;
    check(missing == 0, "every Winsock and IP helper export is registered");
    check(imports_resolve("Iphlpapi.dll", "GetAdaptersInfo") != 0,
          "GetProcAddress finds GetAdaptersInfo however the DLL is spelled");
    // IP_TTL is 7 in Winsock 1.1 and 4 in Winsock 2.
    uint32_t s = udp();
    check(set_opt(s, 0, 7, 33, true) == 0 && get_opt(s, 0, 4) == 33,
          "WSOCK32 IP_TTL (7) is WS2_32 IP_TTL (4)");
    check(set_opt(s, 0, 4, 44) == 0 && get_opt(s, 0, 7, true) == 44, "and back");
    ws("closesocket", {s});
}

// ---------------------------------------------------------------------------
// The capture (RECOMP_NET_PCAP).
// ---------------------------------------------------------------------------
struct Captured {
    uint8_t proto = 0, flags = 0;
    uint32_t src = 0, dst = 0, seq = 0, sec = 0;
    uint16_t sport = 0, dport = 0;
    bool header_sum_ok = false;
    std::string data;
};
static uint32_t be(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i)
        v = v << 8 | p[i];
    return v;
}
static std::vector<Captured> read_capture(const std::string &path, uint32_t *linktype) {
    std::vector<Captured> out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return out;
    uint32_t header[6] = {};
    if (fread(header, 1, sizeof header, f) == sizeof header && header[0] == 0xa1b2c3d4u)
        *linktype = header[5];
    uint32_t rec[4];
    while (fread(rec, 1, sizeof rec, f) == sizeof rec) {
        std::vector<uint8_t> pkt(rec[2]);
        if (fread(pkt.data(), 1, pkt.size(), f) != pkt.size() || pkt.size() < 28)
            break;
        Captured c;
        c.sec = rec[0];
        uint32_t sum = 0;
        for (int i = 0; i < 20; i += 2)
            sum += be(&pkt[i], 2);
        while (sum >> 16)
            sum = (sum & 0xffff) + (sum >> 16);
        c.header_sum_ok = sum == 0xffff && be(&pkt[2], 2) == pkt.size();
        c.proto = pkt[9];
        c.src = be(&pkt[12], 4);
        c.dst = be(&pkt[16], 4);
        c.sport = (uint16_t)be(&pkt[20], 2);
        c.dport = (uint16_t)be(&pkt[22], 2);
        size_t data = 28;
        if (c.proto == 6) {
            c.seq = be(&pkt[24], 4);
            c.flags = pkt[33];
            data = 40;
        }
        c.data.assign((const char *)pkt.data() + data, pkt.size() - data);
        out.push_back(c);
    }
    fclose(f);
    return out;
}

static void test_capture() {
    reset_scratch();
    char path[512];
    const char *tmp = getenv("TMPDIR");
    snprintf(path, sizeof path, "%s/winsock_tests_%llu.pcap", tmp && *tmp ? tmp : "/tmp",
             (unsigned long long)os_monotonic_ns());
    winsock_set_pcap_for_test(path);
    // A datagram each way, and a peek that is recorded once, when it is read.
    uint32_t a = udp(), b = udp();
    bind_to(a, LOOPBACK, 0);
    bind_to(b, 0, 0);
    const uint16_t pa = port_of(a), pb = port_of(b);
    sendto_(b, "ping", LOOPBACK, pa);
    uint32_t buf = gbuf(64);
    ws("recv", {a, buf, 64, 2 /* MSG_PEEK */});
    recv_now(a);
    // A stream: data, the client's close, the end of the stream read on the
    // server's side, the server's close.
    uint32_t l = tcp(), cl = tcp();
    bind_to(l, LOOPBACK, 0);
    ws("listen", {l, 4});
    const uint16_t lp = port_of(l);
    ws("connect", {cl, sin(LOOPBACK, lp), 16});
    uint32_t s = ws("accept", {l, 0, 0});
    const uint16_t pc = port_of(cl);
    ws("send", {cl, gstr("hello"), 5, 0});
    ws("recv", {s, buf, 64, 0});
    ws("send", {cl, gstr("!"), 1, 0});
    ws("closesocket", {cl});
    ws("recv", {s, buf, 64, 0});
    ws("recv", {s, buf, 64, 0});
    ws("recv", {s, buf, 64, 0});
    ws("closesocket", {s});
    for (uint32_t k : {a, b, l})
        ws("closesocket", {k});
    winsock_set_pcap_for_test(nullptr);
    uint32_t linktype = 0;
    std::vector<Captured> got = read_capture(path, &linktype);
    remove(path);
    check(linktype == 101, "the capture is a raw-IP pcap (linktype %u)", linktype);
    check(got.size() == 9, "nine packets recorded (%zu)", got.size());
    if (got.size() != 9)
        return;
    bool sums = true, times = true;
    const uint32_t now = (uint32_t)(os_wall_time_us() / 1000000u);
    for (const Captured &c : got) {
        sums = sums && c.header_sum_ok;
        times = times && c.sec + 60 > now && c.sec <= now + 1;
    }
    check(sums && times, "every IP header's length and checksum are right; wall-clock times");
    check(got[0].proto == 17 && got[0].src == 0 && got[0].sport == pb && got[0].dst == LOOPBACK &&
              got[0].dport == pa && got[0].data == "ping",
          "a sent datagram: from the wildcard socket's port to the address named");
    check(got[1].proto == 17 && got[1].src == LOOPBACK && got[1].sport == pb &&
              got[1].dst == LOOPBACK && got[1].dport == pa && got[1].data == "ping",
          "the datagram received, once (the peek is not recorded): its sender to the socket");
    check(got[2].proto == 6 && got[2].src == LOOPBACK && got[2].sport == pc && got[2].dport == lp &&
              got[2].seq == 1 && got[2].flags == 0x18 && got[2].data == "hello",
          "a stream's send: a data segment from the client to the listening port, seq 1");
    check(got[3].sport == pc && got[3].dport == lp && got[3].seq == 1 && got[3].data == "hello",
          "and the server's receive of it, the same segment");
    check(got[4].sport == pc && got[4].seq == 6 && got[4].data == "!", "the next send: seq 6");
    check(got[5].sport == pc && got[5].dport == lp && got[5].flags == 0x11 && got[5].seq == 7 &&
              got[5].data.empty(),
          "the client's closesocket: a FIN after its 6 bytes");
    check(got[6].sport == pc && got[6].dport == lp && got[6].seq == 6 && got[6].data == "!",
          "the server reads the rest");
    check(got[7].sport == pc && got[7].flags == 0x11 && got[7].seq == 7,
          "and the end of the stream once, however often it reads it");
    check(got[8].sport == lp && got[8].dport == pc && got[8].flags == 0x11 && got[8].seq == 1,
          "the server's closesocket: a FIN from its side");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (!os_net_supported()) {
        printf("winsock_tests: this platform has no sockets; skipped\n");
        return 0;
    }
    mem_init();
    imports_init();
    win32_init(".");
    loader_init_context(&g_main);
    sched_set_guest_thread(true);
    g_scratch = heap_alloc(0x10000, true, 16);
    if (!g_scratch) {
        fprintf(stderr, "cannot allocate scratch\n");
        return 1;
    }
    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"start-up and clean-up", test_startup},
        {"errors, byte order and addresses", test_errors_and_addresses},
        {"invalid sockets", test_invalid_sockets},
        {"UDP on the loopback interface", test_udp_echo},
        {"TCP on the loopback interface", test_tcp_echo},
        {"non-blocking calls and select", test_nonblocking},
        {"WSAEventSelect", test_event_select},
        {"blocking calls in guest threads", test_blocking_threads},
        {"the adapter list", test_adapter_rules},
        {"the IP helper API and Netbios", test_iphlpapi},
        {"broadcast fan-out", test_broadcast},
        {"names", test_names},
        {"the capture (RECOMP_NET_PCAP)", test_capture},
        {"the two DLLs", test_registration},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    printf("winsock_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
