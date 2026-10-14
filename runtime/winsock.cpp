// winsock.cpp - Winsock 1.1 (WSOCK32) and 2.2 (WS2_32) on the host's sockets.
//
// Both DLLs share one implementation, as on Windows, where WSOCK32 forwards to
// WS2_32. They differ only where their ABIs do: the IPPROTO_IP option numbers
// (Winsock 1.1 has its own) and the ordinals 10-12 (import_argc.cpp).
//
// Sockets. A guest SOCKET is a small integer (0x100, 0x104, ...) naming a
// record here, which owns a host descriptor from platform/os.h. The host
// descriptor is always non-blocking; a guest socket is blocking until the
// guest says otherwise (FIONBIO, WSAEventSelect), and a blocking call is a
// loop of non-blocking attempts with a scheduler wait between them. The wait
// is a real guest wait (guest_wait_objects on a per-thread event): it hands
// the baton to the other guest threads, and the monitor thread below signals
// the event when the descriptor becomes ready. SO_RCVTIMEO and SO_SNDTIMEO
// bound it.
//
// WSAEventSelect. The guest's event is a kernel32 event. Network events are
// recorded and the event set the way Windows does it: an event is recorded
// once and then stays quiet until the call that re-enables it (recv and
// recvfrom re-enable FD_READ, accept FD_ACCEPT, a send that would block
// FD_WRITE), and the re-enabling call records it again at once if its
// condition still holds - a host that reads one datagram per wake-up is woken
// again while datagrams are queued. Conditions that come true later are seen
// by one host thread (the monitor) polling every descriptor with an armed
// event, which signals through guest_event_signal_from_host. WSAEventSelect
// itself records whatever already holds.
//
// Broadcast. macOS sends 255.255.255.255 out of one interface only. A
// datagram to it from a socket bound to the wildcard address is sent once per
// adapter instead (net_adapters.h), to that adapter's directed broadcast
// address, or out of a point-to-point tunnel by interface index: a LAN game's
// discovery then reaches the local network and ZeroTier alike. A socket bound
// to one adapter's address broadcasts on that adapter only. BSD also delivers
// broadcasts only to sockets bound to the wildcard address, where Windows
// delivers them to a socket bound to the receiving interface's address, so a
// datagram socket the guest binds to an adapter's address is bound to the
// wildcard on the host and filters what arrives as Windows would.
//
// Names. gethostname is the host's name up to its first dot. gethostbyname
// answers without DNS for a dotted address, "localhost" and the own host name
// (every adapter address, in adapter order, so that the list agrees with
// GetAdaptersInfo); a name in [network] offline_hosts or
// RECOMP_NET_OFFLINE_HOSTS, and every other name under RECOMP_NET_DNS=0,
// fails at once with WSAHOST_NOT_FOUND. Any other name goes to the system
// resolver on a host thread while the guest thread waits as above. The HOSTENT
// and inet_ntoa's string are per thread, overwritten by that thread's next
// call, as Windows documents them.
//
// Errors are per guest thread (WSAGetLastError), and also GetLastError's.
// RECOMP_NET_TRACE=1 writes every socket call and its outcome to stderr.
#include "winsock.h"
#include "game_config.h"
#include "imports.h"
#include "memory.h"
#include "net_adapters.h"
#include "win32.h"
#include "../platform/os.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef RECOMP_NET_OFFLINE_HOSTS
#define RECOMP_NET_OFFLINE_HOSTS {0}
#endif

namespace {

// ---------------------------------------------------------------------------
// The Winsock ABI.
// ---------------------------------------------------------------------------
const uint32_t INVALID_SOCKET_ = 0xffffffffu, SOCKET_ERROR_ = 0xffffffffu;
const uint32_t INADDR_NONE_ = 0xffffffffu, INADDR_BROADCAST_ = 0xffffffffu;
const uint16_t AF_INET_ = 2;
const uint32_t SOCK_STREAM_ = 1, SOCK_DGRAM_ = 2;
const uint32_t IPPROTO_IP_ = 0, IPPROTO_TCP_ = 6, IPPROTO_UDP_ = 17, SOL_SOCKET_ = 0xffff;
const uint32_t MAX_DATAGRAM = 65507;

const uint32_t WSAEINTR = 10004, WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022,
               WSAEMFILE = 10024, WSAEWOULDBLOCK = 10035, WSAEINPROGRESS = 10036,
               WSAEALREADY = 10037, WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039,
               WSAEMSGSIZE = 10040, WSAEPROTOTYPE = 10041, WSAENOPROTOOPT = 10042,
               WSAEPROTONOSUPPORT = 10043, WSAESOCKTNOSUPPORT = 10044, WSAEOPNOTSUPP = 10045,
               WSAEPFNOSUPPORT = 10046, WSAEAFNOSUPPORT = 10047, WSAEADDRINUSE = 10048,
               WSAEADDRNOTAVAIL = 10049, WSAENETDOWN = 10050, WSAENETUNREACH = 10051,
               WSAENETRESET = 10052, WSAECONNABORTED = 10053, WSAECONNRESET = 10054,
               WSAENOBUFS = 10055, WSAEISCONN = 10056, WSAENOTCONN = 10057, WSAESHUTDOWN = 10058,
               WSAETOOMANYREFS = 10059, WSAETIMEDOUT = 10060, WSAECONNREFUSED = 10061,
               WSAELOOP = 10062, WSAENAMETOOLONG = 10063, WSAEHOSTDOWN = 10064,
               WSAEHOSTUNREACH = 10065, WSAVERNOTSUPPORTED = 10092, WSANOTINITIALISED = 10093,
               WSAHOST_NOT_FOUND = 11001, WSATRY_AGAIN = 11002, WSANO_RECOVERY = 11003,
               WSANO_DATA = 11004;

// Network events (WSAEventSelect), and their bit numbers in WSANETWORKEVENTS.
const uint32_t FD_READ_ = 0x01, FD_WRITE_ = 0x02, FD_OOB_ = 0x04, FD_ACCEPT_ = 0x08,
               FD_CONNECT_ = 0x10, FD_CLOSE_ = 0x20, FD_ALL_ = 0x3f;
int fd_bit(uint32_t event) {
    int b = 0;
    while (event > 1)
        event >>= 1, ++b;
    return b;
}

const uint32_t MSG_OOB_ = 1, MSG_PEEK_ = 2, MSG_DONTROUTE_ = 4;
const uint32_t FIONREAD_ = 0x4004667f, FIONBIO_ = 0x8004667e, SIOCATMARK_ = 0x40047307;

// ---------------------------------------------------------------------------
// Per-thread state: the error, and the blocks Windows keeps per thread.
// Only guest threads call in, one at a time (the baton), so none of this
// needs a lock.
// ---------------------------------------------------------------------------
struct ThreadNet {
    uint32_t error = 0;
    uint32_t hostent = 0;    // HOSTENT block (gethostbyname, gethostbyaddr)
    uint32_t ntoa = 0;       // inet_ntoa's string
    uint32_t wait_event = 0; // what a blocking call waits on
};
std::map<uint32_t, ThreadNet> &thread_nets() {
    static auto *m = new std::map<uint32_t, ThreadNet>();
    return *m;
}
ThreadNet &tnet() {
    return thread_nets()[guest_current_thread_id()];
}
void set_wsa(uint32_t e) {
    tnet().error = e;
    set_last_error(e);
}
// A guest block of `size` bytes for this thread, allocated once.
uint32_t thread_block(uint32_t &slot, uint32_t size) {
    if (!slot)
        slot = heap_alloc(size, true);
    return slot;
}
// The thread's private auto-reset event for blocking waits, unsignalled.
uint32_t thread_wait_event() {
    ThreadNet &t = tnet();
    if (!t.wait_event || !win32_reset_event(t.wait_event))
        t.wait_event = win32_create_event(false, false);
    return t.wait_event;
}

bool g_trace = false;
void trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char *fmt, ...) {
    if (!g_trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[net] t%u %s\n", guest_current_thread_id(), line);
}

// ---------------------------------------------------------------------------
// Sockets.
// ---------------------------------------------------------------------------
struct Waiter {
    uint32_t event;
    int want; // OS_NET_POLL_* bits
};

struct Sock {
    int fd = -1;
    int type = 0; // OS_NET_STREAM / OS_NET_DGRAM
    bool nonblocking = false;
    bool bound = false, listening = false, connected = false, connecting = false;
    bool shut_send = false, shut_recv = false;
    int connect_error = 0; // a finished connect's errno, until SO_ERROR reads it
    // Options the guest set that the host either cannot keep or keeps
    // differently.
    bool broadcast = false, reuse = false, exclusive = false, debug = false;
    bool use_loopback = false, dont_fragment = false;
    uint32_t rcvtimeo = 0, sndtimeo = 0;
    // A datagram socket the guest bound to one adapter's address: the host
    // socket is on the wildcard address and receives what that interface
    // would deliver to it on Windows.
    bool iface_bound = false;
    uint32_t guest_ip = 0, guest_broadcast = 0;
    unsigned guest_ifindex = 0;
    // WSAEventSelect: the guest event, the events selected, which of them may
    // be recorded now, which have been since the last WSAEnumNetworkEvents,
    // and each one's error code.
    uint32_t es_event = 0, es_mask = 0, es_armed = 0, es_recorded = 0;
    uint32_t es_error[10] = {};
    // Readiness the monitor has seen and found nothing to record for: not
    // polled again until a call changes something, or the poll would spin.
    bool quiet_in = false, quiet_out = false;
    std::vector<Waiter> waiters; // blocked guest calls, one-shot
};

std::mutex g_m; // the socket table, between guest threads and the monitor
std::map<uint32_t, Sock> &socks() {
    static auto *m = new std::map<uint32_t, Sock>();
    return *m;
}
uint32_t g_next_handle = 0x100;
uint32_t g_startups = 0; // WSAStartup calls not yet matched by WSACleanup

Sock *find_locked(uint32_t h) {
    auto it = socks().find(h);
    return it == socks().end() ? nullptr : &it->second;
}

uint32_t new_handle_locked() {
    for (;;) {
        uint32_t h = g_next_handle;
        g_next_handle = g_next_handle >= 0xfff0u ? 0x100u : g_next_handle + 4;
        if (!socks().count(h))
            return h;
    }
}

// ---------------------------------------------------------------------------
// Errors.
// ---------------------------------------------------------------------------
uint32_t wsa_error(int e) {
    switch (e) {
    case 0:
        return 0;
    case EINTR:
        return WSAEINTR;
    case EBADF:
    case ENOTSOCK:
        return WSAENOTSOCK;
    case EACCES:
    case EPERM:
        return WSAEACCES;
    case EFAULT:
        return WSAEFAULT;
    case EINVAL:
        return WSAEINVAL;
    case EMFILE:
    case ENFILE:
        return WSAEMFILE;
#if EAGAIN != EWOULDBLOCK
    case EAGAIN:
#endif
    case EWOULDBLOCK:
        return WSAEWOULDBLOCK;
    case EINPROGRESS:
        return WSAEINPROGRESS;
    case EALREADY:
        return WSAEALREADY;
    case EDESTADDRREQ:
        return WSAEDESTADDRREQ;
    case EMSGSIZE:
        return WSAEMSGSIZE;
    case EPROTOTYPE:
        return WSAEPROTOTYPE;
    case ENOPROTOOPT:
        return WSAENOPROTOOPT;
    case EPROTONOSUPPORT:
        return WSAEPROTONOSUPPORT;
#ifdef ESOCKTNOSUPPORT
    case ESOCKTNOSUPPORT:
        return WSAESOCKTNOSUPPORT;
#endif
    case EOPNOTSUPP:
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
    case ENOTSUP:
#endif
        return WSAEOPNOTSUPP;
#ifdef EPFNOSUPPORT
    case EPFNOSUPPORT:
        return WSAEPFNOSUPPORT;
#endif
    case EAFNOSUPPORT:
        return WSAEAFNOSUPPORT;
    case EADDRINUSE:
        return WSAEADDRINUSE;
    case EADDRNOTAVAIL:
        return WSAEADDRNOTAVAIL;
    case ENETDOWN:
        return WSAENETDOWN;
    case ENETUNREACH:
        return WSAENETUNREACH;
    case ENETRESET:
        return WSAENETRESET;
    case ECONNABORTED:
        return WSAECONNABORTED;
    case ECONNRESET:
    case EPIPE:
        return WSAECONNRESET;
    case ENOBUFS:
    case ENOMEM:
        return WSAENOBUFS;
    case EISCONN:
        return WSAEISCONN;
    case ENOTCONN:
        return WSAENOTCONN;
#ifdef ESHUTDOWN
    case ESHUTDOWN:
        return WSAESHUTDOWN;
#endif
#ifdef ETOOMANYREFS
    case ETOOMANYREFS:
        return WSAETOOMANYREFS;
#endif
    case ETIMEDOUT:
        return WSAETIMEDOUT;
    case ECONNREFUSED:
        return WSAECONNREFUSED;
    case ELOOP:
        return WSAELOOP;
    case ENAMETOOLONG:
        return WSAENAMETOOLONG;
#ifdef EHOSTDOWN
    case EHOSTDOWN:
        return WSAEHOSTDOWN;
#endif
    case EHOSTUNREACH:
        return WSAEHOSTUNREACH;
    case ENOSYS:
        return WSAENETDOWN;
    default:
        return WSAEINVAL;
    }
}

void fail(X86 *c, uint32_t wsa, uint32_t result = SOCKET_ERROR_) {
    set_wsa(wsa);
    set_eax(c, result);
}

// ---------------------------------------------------------------------------
// Guest addresses: SOCKADDR_IN is family (u16), port and address in network
// order, eight bytes of zero.
// ---------------------------------------------------------------------------
uint32_t ip_at(uint32_t p) { // a network-order IPv4 address in guest memory
    return (uint32_t)rd8(p) << 24 | (uint32_t)rd8(p + 1) << 16 | (uint32_t)rd8(p + 2) << 8 |
           rd8(p + 3);
}
void put_ip(uint32_t p, uint32_t ip) {
    wr8(p, (uint8_t)(ip >> 24));
    wr8(p + 1, (uint8_t)(ip >> 16));
    wr8(p + 2, (uint8_t)(ip >> 8));
    wr8(p + 3, (uint8_t)ip);
}
// The address at `p` (namelen bytes). False with the Winsock error otherwise.
bool read_sockaddr(uint32_t p, uint32_t namelen, uint32_t *ip, uint16_t *port, uint32_t *err) {
    if (!p || (int32_t)namelen < 16 || !gm_valid(p, 16)) {
        *err = WSAEFAULT;
        return false;
    }
    uint16_t family = rd16(p);
    if (family != AF_INET_) {
        *err = WSAEAFNOSUPPORT;
        return false;
    }
    *port = (uint16_t)(rd8(p + 2) << 8 | rd8(p + 3));
    *ip = ip_at(p + 4);
    return true;
}
// Writes an address to a guest SOCKADDR whose length is at *lenp (both may be
// null for "not wanted"). False when the buffer is too small.
bool write_sockaddr(uint32_t p, uint32_t lenp, uint32_t ip, uint16_t port) {
    if (!p && !lenp)
        return true;
    if (!p || !lenp || !gm_valid(lenp, 4) || (int32_t)rd32(lenp) < 16 || !gm_valid(p, 16))
        return false;
    wr16(p, AF_INET_);
    wr8(p + 2, (uint8_t)(port >> 8));
    wr8(p + 3, (uint8_t)port);
    put_ip(p + 4, ip);
    memset(g_mem + p + 8, 0, 8);
    wr32(lenp, 16);
    return true;
}

// A guest buffer, clamped to the arena: a length larger than what follows the
// address is the arena's end, not a fault, as long as the start is valid.
bool guest_buffer(uint32_t p, uint32_t len, uint32_t *usable) {
    if (!len) {
        *usable = 0;
        return true;
    }
    if (!p || !gm_valid(p, 1))
        return false;
    uint32_t room = (uint32_t)(GUEST_SIZE - p);
    *usable = len < room ? len : room;
    return true;
}

// ---------------------------------------------------------------------------
// The monitor: one host thread polling every descriptor some guest call or
// event selection is waiting on.
// ---------------------------------------------------------------------------
int g_wake[2] = {-1, -1};
bool g_monitor_started = false;

int probe(int fd, int want) {
    OsNetPoll p = {fd, want, 0};
    return os_net_poll(&p, 1, 0) > 0 ? p.got : 0;
}

void wake_monitor() {
    if (g_wake[1] >= 0)
        os_net_wakeup_signal(g_wake[1]);
}

// Records one network event (under g_m) and queues the guest event.
void record_locked(Sock &s, uint32_t event, uint32_t error, std::vector<uint32_t> &signal) {
    s.es_recorded |= event;
    s.es_error[fd_bit(event)] = error;
    s.es_armed &= ~event;
    if (s.es_event && std::find(signal.begin(), signal.end(), s.es_event) == signal.end())
        signal.push_back(s.es_event);
}
bool armed(const Sock &s, uint32_t event) {
    return s.es_event && (s.es_armed & event) != 0;
}

// A connecting stream socket's outcome, once the host has one (under g_m).
void settle_connect_locked(Sock &s) {
    if (!s.connecting)
        return;
    int got = probe(s.fd, OS_NET_POLL_OUT);
    if (!got)
        return;
    int err = os_net_pending_error(s.fd);
    if (!err && !(got & OS_NET_POLL_OUT))
        err = ECONNREFUSED;
    s.connecting = false;
    s.connected = err == 0;
    s.connect_error = err;
    if (s.connected)
        s.bound = true;
}

// The network events a socket's readiness `got` gives, recorded for its
// WSAEventSelect (under g_m). True when one was recorded.
bool evaluate_locked(Sock &s, int got, std::vector<uint32_t> &signal) {
    if (!s.es_event || !s.es_mask)
        return false;
    uint32_t before = s.es_recorded;
    const bool err = (got & (OS_NET_POLL_ERR | OS_NET_POLL_HUP)) != 0;
    if (s.connecting && (got & (OS_NET_POLL_OUT | OS_NET_POLL_ERR | OS_NET_POLL_HUP))) {
        settle_connect_locked(s);
        if (!s.connecting) {
            if (armed(s, FD_CONNECT_))
                record_locked(s, FD_CONNECT_, wsa_error(s.connect_error), signal);
            if (s.connected && armed(s, FD_WRITE_))
                record_locked(s, FD_WRITE_, 0, signal);
        }
        return s.es_recorded != before;
    }
    if (got & (OS_NET_POLL_IN | OS_NET_POLL_HUP | OS_NET_POLL_ERR)) {
        if (s.listening) {
            if ((got & OS_NET_POLL_IN) && armed(s, FD_ACCEPT_))
                record_locked(s, FD_ACCEPT_, 0, signal);
        } else if (s.type == OS_NET_DGRAM) {
            if ((got & OS_NET_POLL_IN) && armed(s, FD_READ_))
                record_locked(s, FD_READ_, 0, signal);
            if (err)
                os_net_pending_error(s.fd); // an ICMP error, which Windows does not report
        } else if (s.connected) {
            uint32_t queued = 0;
            os_net_readable(s.fd, &queued);
            if (queued) {
                if (armed(s, FD_READ_))
                    record_locked(s, FD_READ_, 0, signal);
            } else if (armed(s, FD_CLOSE_)) {
                int e = os_net_pending_error(s.fd);
                record_locked(s, FD_CLOSE_,
                              e == 0            ? 0
                              : e == ECONNRESET ? WSAECONNRESET
                                                : WSAECONNABORTED,
                              signal);
            }
        }
    }
    if ((got & OS_NET_POLL_OUT) && (s.connected || s.type == OS_NET_DGRAM) && armed(s, FD_WRITE_))
        record_locked(s, FD_WRITE_, 0, signal);
    if ((got & OS_NET_POLL_PRI) && armed(s, FD_OOB_))
        record_locked(s, FD_OOB_, 0, signal);
    return s.es_recorded != before;
}

// What the monitor polls a socket for (under g_m).
int interest_locked(const Sock &s) {
    int want = 0;
    if (s.es_event && s.es_mask) {
        if (!s.quiet_in && (s.es_armed & (FD_READ_ | FD_ACCEPT_ | FD_CLOSE_)))
            want |= OS_NET_POLL_IN;
        if (!s.quiet_out &&
            (((s.es_armed & FD_CONNECT_) && s.connecting) || (s.es_armed & FD_WRITE_)))
            want |= OS_NET_POLL_OUT;
        if (s.es_armed & FD_OOB_)
            want |= OS_NET_POLL_PRI;
    }
    for (const Waiter &w : s.waiters)
        want |= w.want;
    return want;
}

void *monitor_main(void *) {
    std::vector<OsNetPoll> polls;
    std::vector<uint32_t> handles, signal;
    for (;;) {
        polls.clear();
        handles.clear();
        polls.push_back({g_wake[0], OS_NET_POLL_IN, 0});
        handles.push_back(0);
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (auto &kv : socks()) {
                int want = interest_locked(kv.second);
                if (!want)
                    continue;
                polls.push_back({kv.second.fd, want, 0});
                handles.push_back(kv.first);
            }
        }
        if (os_net_poll(polls.data(), polls.size(), 1000) <= 0)
            continue;
        os_net_wakeup_drain(g_wake[0]);
        signal.clear();
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (size_t i = 1; i < polls.size(); ++i) {
                const int got = polls[i].got;
                if (!got)
                    continue;
                Sock *s = find_locked(handles[i]);
                if (!s || s->fd != polls[i].fd)
                    continue; // closed while the poll ran
                for (const Waiter &w : s->waiters)
                    if (got & (w.want | OS_NET_POLL_ERR | OS_NET_POLL_HUP))
                        signal.push_back(w.event);
                s->waiters.erase(std::remove_if(s->waiters.begin(), s->waiters.end(),
                                                [got](const Waiter &w) {
                                                    return (got & (w.want | OS_NET_POLL_ERR |
                                                                   OS_NET_POLL_HUP)) != 0;
                                                }),
                                 s->waiters.end());
                if (!evaluate_locked(*s, got, signal)) {
                    // Readiness no armed event wants: keep it out of the poll
                    // until a guest call changes the picture (waiters are
                    // polled regardless).
                    if (got & (OS_NET_POLL_IN | OS_NET_POLL_HUP | OS_NET_POLL_ERR))
                        s->quiet_in = true;
                    if (got & (OS_NET_POLL_OUT | OS_NET_POLL_HUP | OS_NET_POLL_ERR))
                        s->quiet_out = true;
                }
            }
        }
        for (uint32_t h : signal)
            guest_event_signal_from_host(h);
    }
    return nullptr;
}

void start_monitor() {
    if (g_monitor_started)
        return;
    if (os_net_wakeup_pair(g_wake) != 0)
        return;
    OsThread *t = os_thread_create(monitor_main, nullptr, 0);
    if (!t)
        return;
    os_thread_detach(t);
    g_monitor_started = true;
}

// A deadline `timeout_ms` from now on the monotonic clock; 0 for none
// (0xffffffff, Windows' INFINITE and its "no timeout set").
uint64_t deadline_after(uint32_t timeout_ms) {
    return timeout_ms == 0xffffffffu ? 0 : os_monotonic_ns() + (uint64_t)timeout_ms * 1000000ull;
}
// What is left of a deadline, in milliseconds, for one wait of at most a
// second: a blocked guest thread looks again at least that often, so a wait
// never depends on one wake-up arriving, and the scheduler never sees a wait
// with no end. False once the deadline has passed.
bool wait_slice(uint64_t deadline, uint32_t *ms) {
    uint64_t now = os_monotonic_ns();
    if (deadline && now >= deadline)
        return false;
    uint64_t left = deadline ? (deadline - now + 999999ull) / 1000000ull : 1000;
    *ms = left > 1000 ? 1000u : (uint32_t)left;
    return true;
}

// Waits, as a guest wait, until the socket is ready for `want` (or fails, or
// closes), or the deadline (0 for none) passes. False once it has. A wake is
// a reason to try again, not a promise: callers loop.
bool wait_ready(uint32_t h, int want, uint64_t deadline) {
    uint32_t slice;
    if (!wait_slice(deadline, &slice))
        return false;
    start_monitor();
    uint32_t ev = thread_wait_event();
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s)
            return true;
        s->waiters.push_back({ev, want});
    }
    wake_monitor();
    uint32_t r = guest_wait_objects(&ev, 1, false, slice);
    std::lock_guard<std::mutex> lock(g_m);
    if (Sock *s = find_locked(h))
        s->waiters.erase(std::remove_if(s->waiters.begin(), s->waiters.end(),
                                        [ev](const Waiter &w) { return w.event == ev; }),
                         s->waiters.end());
    return r == 0 || !deadline || os_monotonic_ns() < deadline;
}

// Signals the guest events gathered under the lock. Called by the baton
// holder, so the handle table is its to touch.
void signal_now(const std::vector<uint32_t> &events) {
    for (uint32_t h : events)
        win32_signal_event(h, false);
}

// Re-enables an event after the call that re-enables it, recording it again at
// once while its condition still holds.
void reenable(uint32_t h, uint32_t event) {
    std::vector<uint32_t> signal;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s || !s->es_event || !(s->es_mask & event))
            return;
        s->es_armed |= event;
        s->quiet_in = s->quiet_out = false;
        int want = event & (FD_READ_ | FD_ACCEPT_) ? OS_NET_POLL_IN
                   : event & FD_OOB_               ? OS_NET_POLL_PRI
                                                   : OS_NET_POLL_OUT;
        if (event == FD_WRITE_) {
            // Re-armed only after a send that would block: the buffer is full
            // now by definition, so the monitor reports when it drains.
        } else {
            int got = probe(s->fd, want);
            if (got)
                evaluate_locked(*s, got, signal);
        }
    }
    signal_now(signal);
    wake_monitor();
}

// The handle's record, or null with WSAENOTSOCK set. The pointer is valid
// until the caller next waits.
Sock *sock_or_fail(X86 *c, uint32_t h) {
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s)
        fail(c, WSAENOTSOCK);
    return s;
}

bool started_or_fail(X86 *c, uint32_t result = SOCKET_ERROR_) {
    if (g_startups)
        return true;
    fail(c, WSANOTINITIALISED, result);
    return false;
}

void close_all_on_reset() {
    std::lock_guard<std::mutex> lock(g_m);
    for (auto &kv : socks())
        os_net_close(kv.second.fd);
    socks().clear();
    thread_nets().clear();
    g_startups = 0;
}

// ---------------------------------------------------------------------------
// Start-up.
// ---------------------------------------------------------------------------
// WSAStartup(wVersionRequested, lpWSAData): any version from 1.0; the one
// granted is min(requested, 2.2). WSADATA is 400 bytes on a 32-bit guest.
void ws_WSAStartup(X86 *c) {
    uint32_t version = arg(c, 0) & 0xffff, pdata = arg(c, 1);
    uint32_t major = version & 0xff, minor = version >> 8;
    if (major < 1) {
        set_eax(c, WSAVERNOTSUPPORTED);
        return;
    }
    if (!pdata || !gm_valid(pdata, 400)) {
        set_eax(c, WSAEFAULT);
        return;
    }
    if (major > 2 || (major == 2 && minor > 2))
        version = 0x0202;
    memset(g_mem + pdata, 0, 400);
    wr16(pdata + 0, (uint16_t)version);
    wr16(pdata + 2, 0x0202);
    gm_put_str(pdata + 4, "WinSock 2.0", 257);
    gm_put_str(pdata + 4 + 257, os_net_supported() ? "Running" : "Network unavailable", 129);
    // iMaxSockets and iMaxUdpDg, which Winsock 2 leaves to the caller to
    // ignore and Winsock 1.1 programs still read.
    wr16(pdata + 4 + 257 + 129, 32767);
    wr16(pdata + 4 + 257 + 129 + 2, 65467);
    ++g_startups;
    g_trace = recomp_env("NET_TRACE") && atoi(recomp_env("NET_TRACE")) != 0;
    trace("WSAStartup(%u.%u) -> %u start-ups", version & 0xff, version >> 8, g_startups);
    set_eax(c, 0);
}
// A start-up count that reaches zero would close every socket on Windows.
// Here the sockets survive it: a module that cleans up and starts again
// mid-session (as some network plugins do) keeps them either way, since
// another module's start-up usually holds the count above zero there.
void ws_WSACleanup(X86 *c) {
    if (!g_startups) {
        fail(c, WSANOTINITIALISED);
        return;
    }
    --g_startups;
    set_eax(c, 0);
}
void ws_WSAGetLastError(X86 *c) {
    set_eax(c, tnet().error);
}
void ws_WSASetLastError(X86 *c) {
    set_wsa(arg(c, 0));
}
// The Winsock 1.1 blocking hooks: there is no blocking call to cancel or hook.
void ws_WSAIsBlocking(X86 *c) {
    set_eax(c, 0);
}
void ws_WSACancelBlockingCall(X86 *c) {
    fail(c, WSAEINVAL);
}
void ws_WSASetBlockingHook(X86 *c) {
    set_eax(c, 0); // no previous hook
}
void ws_WSAUnhookBlockingHook(X86 *c) {
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Byte order and addresses.
// ---------------------------------------------------------------------------
uint32_t swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
void ws_htons(X86 *c) {
    uint32_t v = arg(c, 0) & 0xffff;
    set_eax(c, ((v & 0xff) << 8) | (v >> 8));
}
void ws_htonl(X86 *c) {
    set_eax(c, swap32(arg(c, 0)));
}
// WSAHtons(s, hostshort, *netshort) and friends: the socket only has to exist.
void ws_WSAHtons(X86 *c) {
    if (!sock_or_fail(c, arg(c, 0)))
        return;
    uint32_t v = arg(c, 1) & 0xffff, out = arg(c, 2);
    if (!out || !gm_valid(out, 2)) {
        fail(c, WSAEFAULT);
        return;
    }
    wr16(out, (uint16_t)(((v & 0xff) << 8) | (v >> 8)));
    set_eax(c, 0);
}
void ws_WSAHtonl(X86 *c) {
    if (!sock_or_fail(c, arg(c, 0)))
        return;
    uint32_t out = arg(c, 2);
    if (!out || !gm_valid(out, 4)) {
        fail(c, WSAEFAULT);
        return;
    }
    wr32(out, swap32(arg(c, 1)));
    set_eax(c, 0);
}

// inet_addr's grammar (BSD inet_aton, which Winsock follows): one to four
// parts separated by dots, each decimal, octal (leading 0) or hex (0x); the
// last part fills the bytes the others left. Trailing white space ends it.
bool parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4];
    int n = 0;
    const char *p = s;
    for (;;) {
        if (!(*p >= '0' && *p <= '9'))
            return false;
        uint32_t base = 10;
        if (*p == '0') {
            base = 8;
            ++p;
            if (*p == 'x' || *p == 'X') {
                base = 16;
                ++p;
            }
        }
        uint64_t v = 0;
        bool digits = base == 8; // "0" alone is a digit
        for (;; ++p) {
            int d;
            if (*p >= '0' && *p <= '9')
                d = *p - '0';
            else if (base == 16 && *p >= 'a' && *p <= 'f')
                d = *p - 'a' + 10;
            else if (base == 16 && *p >= 'A' && *p <= 'F')
                d = *p - 'A' + 10;
            else
                break;
            if (d >= (int)base)
                return false;
            v = v * base + (uint64_t)d;
            if (v > 0xffffffffull)
                return false;
            digits = true;
        }
        if (!digits || n == 4)
            return false;
        parts[n++] = (uint32_t)v;
        if (*p != '.')
            break;
        ++p;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    if (*p)
        return false;
    uint32_t ip;
    switch (n) {
    case 1:
        ip = parts[0];
        break;
    case 2:
        if (parts[0] > 0xff || parts[1] > 0xffffff)
            return false;
        ip = parts[0] << 24 | parts[1];
        break;
    case 3:
        if (parts[0] > 0xff || parts[1] > 0xff || parts[2] > 0xffff)
            return false;
        ip = parts[0] << 24 | parts[1] << 16 | parts[2];
        break;
    default:
        for (int i = 0; i < 4; ++i)
            if (parts[i] > 0xff)
                return false;
        ip = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
        break;
    }
    *out = ip;
    return true;
}
// inet_addr(cp): the address in network order, INADDR_NONE when cp is not one.
void ws_inet_addr(X86 *c) {
    uint32_t p = arg(c, 0), ip = 0;
    if (!p || !parse_ipv4(gm_str(p, 64).c_str(), &ip)) {
        set_eax(c, INADDR_NONE_);
        return;
    }
    set_eax(c, swap32(ip));
}
// inet_ntoa(in): a per-thread string, overwritten by the thread's next call.
void ws_inet_ntoa(X86 *c) {
    uint32_t in = arg(c, 0); // network order, as stored
    uint32_t buf = thread_block(tnet().ntoa, 32);
    if (!buf) {
        set_eax(c, 0);
        return;
    }
    gm_put_str(buf, net_dotted(swap32(in)).c_str(), 32);
    set_eax(c, buf);
}

// ---------------------------------------------------------------------------
// Socket life.
// ---------------------------------------------------------------------------
uint32_t make_socket(X86 *c, uint32_t af, uint32_t type, uint32_t protocol) {
    if (!started_or_fail(c, INVALID_SOCKET_))
        return INVALID_SOCKET_;
    if (af != AF_INET_) {
        fail(c, WSAEAFNOSUPPORT, INVALID_SOCKET_);
        return INVALID_SOCKET_;
    }
    int t;
    if (type == SOCK_STREAM_ && (protocol == 0 || protocol == IPPROTO_TCP_))
        t = OS_NET_STREAM;
    else if (type == SOCK_DGRAM_ && (protocol == 0 || protocol == IPPROTO_UDP_))
        t = OS_NET_DGRAM;
    else if (type == SOCK_STREAM_ || type == SOCK_DGRAM_) {
        fail(c, WSAEPROTONOSUPPORT, INVALID_SOCKET_);
        return INVALID_SOCKET_;
    } else {
        fail(c, WSAESOCKTNOSUPPORT, INVALID_SOCKET_);
        return INVALID_SOCKET_;
    }
    int fd = os_net_socket(t);
    if (fd < 0) {
        fail(c, wsa_error(errno), INVALID_SOCKET_);
        return INVALID_SOCKET_;
    }
    // Windows lets a listening port be bound again while old connections on
    // it linger in TIME_WAIT; BSD does only with SO_REUSEADDR. That alone
    // does not let two sockets share an address and port.
    if (t == OS_NET_STREAM)
        os_net_set_option(fd, OS_NET_OPT_REUSEADDR, 1);
    uint32_t h;
    {
        std::lock_guard<std::mutex> lock(g_m);
        h = new_handle_locked();
        Sock &s = socks()[h];
        s.fd = fd;
        s.type = t;
    }
    trace("socket(%s) -> %#x (fd %d)", t == OS_NET_STREAM ? "TCP" : "UDP", h, fd);
    set_eax(c, h);
    return h;
}
void ws_socket(X86 *c) {
    make_socket(c, arg(c, 0), arg(c, 1), arg(c, 2));
}
// WSASocketA/W(af, type, protocol, lpProtocolInfo, g, dwFlags): a socket like
// socket()'s; overlapped I/O is not modelled, so the flag is accepted and the
// socket behaves as an ordinary one.
void ws_WSASocket(X86 *c) {
    if (arg(c, 3)) {
        fail(c, WSAEINVAL, INVALID_SOCKET_);
        return;
    }
    make_socket(c, arg(c, 0), arg(c, 1), arg(c, 2));
}

void ws_closesocket(X86 *c) {
    uint32_t h = arg(c, 0);
    std::vector<uint32_t> waiters;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s) {
            fail(c, WSAENOTSOCK);
            return;
        }
        os_net_close(s->fd);
        for (const Waiter &w : s->waiters)
            waiters.push_back(w.event);
        trace("closesocket(%#x)", h);
        socks().erase(h);
    }
    signal_now(waiters); // a call blocked on it returns WSAEINTR
    wake_monitor();
    set_eax(c, 0);
}

void ws_shutdown(X86 *c) {
    uint32_t h = arg(c, 0), how = arg(c, 1);
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    if (how > 2) {
        fail(c, WSAEINVAL);
        return;
    }
    if (s->type == OS_NET_STREAM && !s->connected) {
        fail(c, WSAENOTCONN);
        return;
    }
    if (os_net_shutdown(s->fd, (int)how) != 0 && errno != ENOTCONN) {
        fail(c, wsa_error(errno));
        return;
    }
    if (how != 1)
        s->shut_recv = true;
    if (how != 0)
        s->shut_send = true;
    set_eax(c, 0);
}

// The adapter owning a local address, for a socket bound to it.
bool adapter_of(uint32_t ip, NetAdapter *out) {
    for (const NetAdapter &a : net_adapters())
        for (const NetAddress &addr : a.addresses)
            if (addr.ip == ip) {
                *out = a;
                return true;
            }
    return false;
}

void ws_bind(X86 *c) {
    uint32_t h = arg(c, 0), err = 0, ip = 0;
    uint16_t port = 0;
    if (!read_sockaddr(arg(c, 1), arg(c, 2), &ip, &port, &err)) {
        fail(c, err);
        return;
    }
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    if (s->bound) {
        fail(c, WSAEINVAL);
        return;
    }
    // A datagram socket on one adapter's address is the wildcard on the host,
    // so that the broadcasts Windows would deliver to it arrive (see the top).
    NetAdapter a;
    bool iface = s->type == OS_NET_DGRAM && ip != 0 && (ip >> 24) != 127 && adapter_of(ip, &a) &&
                 a.can_broadcast;
    if (os_net_bind(s->fd, iface ? 0 : ip, port) != 0) {
        uint32_t e = wsa_error(errno);
        trace("bind(%#x, %s:%u) -> error %u", h, net_dotted(ip).c_str(), port, e);
        fail(c, e);
        return;
    }
    if (iface) {
        os_net_set_option(s->fd, OS_NET_OPT_RECV_DESTINATION, 1);
        s->iface_bound = true;
        s->guest_ip = ip;
        s->guest_ifindex = a.index;
        s->guest_broadcast = a.broadcast;
    }
    s->bound = true;
    trace("bind(%#x, %s:%u)%s", h, net_dotted(ip).c_str(), port,
          iface ? " (wildcard on the host; filtered to its interface)" : "");
    set_eax(c, 0);
}

void ws_listen(X86 *c) {
    uint32_t h = arg(c, 0);
    int32_t backlog = (int32_t)arg(c, 1);
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    if (s->type != OS_NET_STREAM) {
        fail(c, WSAEOPNOTSUPP);
        return;
    }
    if (s->connected || s->connecting) {
        fail(c, WSAEISCONN);
        return;
    }
    if (!s->bound) {
        fail(c, WSAEINVAL);
        return;
    }
    // SOMAXCONN (0x7fffffff) and anything silly become the host's maximum.
    if (backlog <= 0)
        backlog = 1;
    if (backlog > 128)
        backlog = 128;
    if (os_net_listen(s->fd, backlog) != 0) {
        fail(c, wsa_error(errno));
        return;
    }
    s->listening = true;
    s->quiet_in = false;
    trace("listen(%#x)", h);
    set_eax(c, 0);
}

void ws_accept(X86 *c) {
    uint32_t h = arg(c, 0), addr = arg(c, 1), lenp = arg(c, 2);
    if (addr && (!lenp || !gm_valid(lenp, 4) || (int32_t)rd32(lenp) < 16)) {
        fail(c, WSAEFAULT, INVALID_SOCKET_);
        return;
    }
    uint64_t deadline = 0;
    bool deadline_set = false;
    for (;;) {
        int fd, err;
        uint32_t ip = 0, timeout;
        uint16_t port = 0;
        bool blocking;
        {
            std::lock_guard<std::mutex> lock(g_m);
            Sock *s = find_locked(h);
            if (!s) {
                fail(c, WSAENOTSOCK, INVALID_SOCKET_);
                return;
            }
            if (!s->listening) {
                fail(c, WSAEINVAL, INVALID_SOCKET_);
                return;
            }
            fd = os_net_accept(s->fd, &ip, &port);
            err = errno;
            blocking = !s->nonblocking;
            timeout = s->rcvtimeo ? s->rcvtimeo : 0xffffffffu;
            if (fd >= 0) {
                uint32_t nh = new_handle_locked();
                Sock &n = socks()[nh];
                Sock &l = *find_locked(h);
                n.fd = fd;
                n.type = OS_NET_STREAM;
                n.bound = n.connected = true;
                // The new socket has the listening socket's properties, its
                // event selection included.
                n.nonblocking = l.nonblocking;
                n.rcvtimeo = l.rcvtimeo;
                n.sndtimeo = l.sndtimeo;
                n.es_event = l.es_event;
                n.es_mask = l.es_mask;
                n.es_armed = l.es_mask;
                std::vector<uint32_t> signal;
                if (n.es_event) {
                    int got = probe(fd, OS_NET_POLL_IN | OS_NET_POLL_OUT);
                    evaluate_locked(n, got, signal);
                }
                write_sockaddr(addr, lenp, ip, port);
                trace("accept(%#x) -> %#x from %s:%u", h, nh, net_dotted(ip).c_str(), port);
                set_eax(c, nh);
                l.quiet_in = false;
                // FD_ACCEPT again if another connection is waiting.
                signal_now(signal);
                break;
            }
        }
        if ((err == EWOULDBLOCK || err == EAGAIN || err == ECONNABORTED) && blocking) {
            if (!deadline_set) {
                deadline = deadline_after(timeout);
                deadline_set = true;
            }
            if (!wait_ready(h, OS_NET_POLL_IN, deadline)) {
                fail(c, WSAETIMEDOUT, INVALID_SOCKET_);
                return;
            }
            continue;
        }
        fail(c, err == ECONNABORTED ? WSAEWOULDBLOCK : wsa_error(err), INVALID_SOCKET_);
        reenable(h, FD_ACCEPT_);
        return;
    }
    reenable(h, FD_ACCEPT_);
    wake_monitor();
}

void ws_connect(X86 *c) {
    uint32_t h = arg(c, 0), err = 0, ip = 0;
    uint16_t port = 0;
    if (!read_sockaddr(arg(c, 1), arg(c, 2), &ip, &port, &err)) {
        fail(c, err);
        return;
    }
    bool blocking;
    uint32_t timeout;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s) {
            fail(c, WSAENOTSOCK);
            return;
        }
        if (s->listening) {
            fail(c, WSAEINVAL);
            return;
        }
        if (s->type == OS_NET_STREAM) {
            settle_connect_locked(*s);
            if (s->connected) {
                fail(c, WSAEISCONN);
                return;
            }
            if (s->connecting) {
                fail(c, WSAEALREADY);
                return;
            }
        }
        if (port == 0 || (s->type == OS_NET_STREAM && (ip == 0 || ip == INADDR_BROADCAST_))) {
            fail(c, WSAEADDRNOTAVAIL);
            return;
        }
        if (os_net_connect(s->fd, ip, port) == 0) {
            s->connected = s->bound = true;
            s->quiet_in = s->quiet_out = false;
            std::vector<uint32_t> signal;
            if (s->type == OS_NET_STREAM && armed(*s, FD_CONNECT_))
                record_locked(*s, FD_CONNECT_, 0, signal);
            if (armed(*s, FD_WRITE_))
                record_locked(*s, FD_WRITE_, 0, signal);
            signal_now(signal);
            trace("connect(%#x, %s:%u) -> connected", h, net_dotted(ip).c_str(), port);
            set_eax(c, 0);
            return;
        }
        int e = errno;
        if (e != EINPROGRESS) {
            uint32_t w = wsa_error(e);
            trace("connect(%#x, %s:%u) -> error %u", h, net_dotted(ip).c_str(), port, w);
            fail(c, w);
            return;
        }
        s->connecting = true;
        s->bound = true;
        s->quiet_out = false;
        blocking = !s->nonblocking;
        timeout = s->sndtimeo ? s->sndtimeo : 0xffffffffu;
    }
    wake_monitor();
    if (!blocking) {
        // Windows reports a connection in progress as WSAEWOULDBLOCK.
        trace("connect(%#x, %s:%u) -> in progress", h, net_dotted(ip).c_str(), port);
        fail(c, WSAEWOULDBLOCK);
        return;
    }
    const uint64_t deadline = deadline_after(timeout);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_m);
            Sock *s = find_locked(h);
            if (!s) {
                fail(c, WSAEINTR);
                return;
            }
            settle_connect_locked(*s);
            if (!s->connecting) {
                int e = s->connect_error;
                s->connect_error = 0;
                trace("connect(%#x, %s:%u) -> %s", h, net_dotted(ip).c_str(), port,
                      e ? "failed" : "connected");
                if (e) {
                    fail(c, wsa_error(e));
                    return;
                }
                set_eax(c, 0);
                return;
            }
        }
        if (!wait_ready(h, OS_NET_POLL_OUT, deadline)) {
            fail(c, WSAETIMEDOUT);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Sending and receiving.
// ---------------------------------------------------------------------------
int host_flags(uint32_t flags) {
    return ((flags & MSG_OOB_) ? OS_NET_MSG_OOB : 0) | ((flags & MSG_PEEK_) ? OS_NET_MSG_PEEK : 0) |
           ((flags & MSG_DONTROUTE_) ? OS_NET_MSG_DONTROUTE : 0);
}
bool flags_ok(uint32_t flags) {
    return (flags & ~(MSG_OOB_ | MSG_PEEK_ | MSG_DONTROUTE_)) == 0;
}

// Whether a datagram that arrived on an interface-bound socket is one Windows
// would have delivered to it.
bool accepts(const Sock &s, const OsNetDatagram &d) {
    if (!s.iface_bound || !d.to_ip)
        return true;
    if (d.to_ip == s.guest_ip)
        return true;
    if ((d.to_ip >> 28) == 0xe) // multicast: the groups it joined
        return true;
    bool broadcast =
        d.to_ip == INADDR_BROADCAST_ || (s.guest_broadcast && d.to_ip == s.guest_broadcast);
    return broadcast && (!d.ifindex || d.ifindex == s.guest_ifindex);
}

// recv/recvfrom: one attempt (under no lock: the descriptor is the caller's
// for as long as it holds the baton). Returns bytes, or -1 with *err set.
int64_t receive_once(uint32_t h, uint32_t buf, uint32_t len, uint32_t flags, uint32_t from,
                     uint32_t fromlen, uint32_t *err, bool *would_block, bool *blocking,
                     uint32_t *timeout) {
    Sock snap;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s) {
            *err = WSAENOTSOCK;
            return -1;
        }
        snap.fd = s->fd;
        snap.type = s->type;
        snap.iface_bound = s->iface_bound;
        snap.guest_ip = s->guest_ip;
        snap.guest_broadcast = s->guest_broadcast;
        snap.guest_ifindex = s->guest_ifindex;
        *blocking = !s->nonblocking;
        *timeout = s->rcvtimeo ? s->rcvtimeo : 0xffffffffu;
        if (s->listening) {
            *err = WSAENOTCONN;
            return -1;
        }
        if (s->shut_recv) {
            *err = WSAESHUTDOWN;
            return -1;
        }
        if (s->type == OS_NET_STREAM && !s->connected) {
            settle_connect_locked(*s);
            if (!s->connected) {
                *err = s->connecting ? WSAEWOULDBLOCK : WSAENOTCONN;
                *would_block = s->connecting;
                return -1;
            }
        }
        if (s->type == OS_NET_DGRAM && !s->bound) {
            *err = WSAEINVAL; // Windows: recvfrom before the socket has an address
            return -1;
        }
    }
    *would_block = false;
    uint8_t *dst = g_mem + buf;
    for (;;) {
        OsNetDatagram info;
        int64_t n = snap.type == OS_NET_DGRAM
                        ? os_net_recvfrom(snap.fd, dst, len, host_flags(flags), &info)
                        : os_net_recv(snap.fd, dst, len, host_flags(flags));
        if (n < 0) {
            int e = errno;
            if (e == EWOULDBLOCK || e == EAGAIN) {
                *would_block = true;
                *err = WSAEWOULDBLOCK;
                return -1;
            }
            if (snap.type == OS_NET_DGRAM && (e == ECONNREFUSED || e == EHOSTUNREACH)) {
                continue; // an ICMP report for an earlier datagram: not an error on Windows
            }
            *err = wsa_error(e);
            return -1;
        }
        if (snap.type == OS_NET_DGRAM) {
            if (!accepts(snap, info)) {
                if (flags & MSG_PEEK_) // drop it for real before looking again
                    os_net_recvfrom(snap.fd, dst, len, 0, &info);
                continue;
            }
            if (from || fromlen) {
                if (!write_sockaddr(from, fromlen, info.from_ip, info.from_port)) {
                    *err = WSAEFAULT;
                    return -1;
                }
            }
            if (info.truncated) {
                *err = WSAEMSGSIZE; // Windows keeps the part that fit
                return -1;
            }
        }
        // A stream ignores recvfrom's address arguments, as on Windows.
        return n;
    }
}

void do_receive(X86 *c, uint32_t h, uint32_t buf, uint32_t len, uint32_t flags, uint32_t from,
                uint32_t fromlen) {
    uint32_t usable = 0;
    if (!flags_ok(flags)) {
        fail(c, WSAEOPNOTSUPP);
        return;
    }
    if (!guest_buffer(buf, len, &usable)) {
        fail(c, WSAEFAULT);
        return;
    }
    if (from && (!fromlen || !gm_valid(fromlen, 4) || (int32_t)rd32(fromlen) < 16)) {
        fail(c, WSAEFAULT);
        return;
    }
    uint64_t deadline = 0;
    bool deadline_set = false;
    for (;;) {
        uint32_t err = 0, timeout = 0;
        bool would_block = false, blocking = false;
        int64_t n = receive_once(h, buf, usable, flags, from, fromlen, &err, &would_block,
                                 &blocking, &timeout);
        if (n >= 0) {
            trace("recv(%#x, %u) -> %lld", h, usable, (long long)n);
            set_eax(c, (uint32_t)n);
            reenable(h, (flags & MSG_OOB_) ? FD_OOB_ : FD_READ_);
            return;
        }
        if (would_block && blocking) {
            if (!deadline_set) {
                deadline = deadline_after(timeout);
                deadline_set = true;
            }
            if (!wait_ready(h, OS_NET_POLL_IN, deadline)) {
                fail(c, WSAETIMEDOUT);
                return;
            }
            std::lock_guard<std::mutex> lock(g_m);
            if (!find_locked(h)) {
                fail(c, WSAEINTR);
                return;
            }
            continue;
        }
        if (err == WSAEMSGSIZE)
            trace("recv(%#x, %u) -> truncated", h, usable);
        fail(c, err);
        reenable(h, (flags & MSG_OOB_) ? FD_OOB_ : FD_READ_);
        return;
    }
}
void ws_recv(X86 *c) {
    do_receive(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), 0, 0);
}
void ws_recvfrom(X86 *c) {
    do_receive(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5));
}

// Sends a datagram to 255.255.255.255 once per adapter it should reach (see
// the top). Returns the length, or -1 with errno.
int64_t send_broadcast(const Sock &s, const uint8_t *data, uint32_t len, int flags, uint32_t ip,
                       uint16_t port) {
    std::vector<NetAdapter> targets;
    for (const NetAdapter &a : net_adapters())
        if (!s.iface_bound || a.index == s.guest_ifindex)
            targets.push_back(a);
    if (targets.empty())
        return os_net_sendto(s.fd, data, len, flags, ip, port, 0);
    int64_t result = -1;
    int last = 0;
    for (const NetAdapter &a : targets) {
        int64_t n;
        if (a.can_broadcast && a.broadcast)
            n = os_net_sendto(s.fd, data, len, flags, a.broadcast, port, 0);
        else
            n = os_net_sendto(s.fd, data, len, flags, INADDR_BROADCAST_, port, a.index);
        trace("  broadcast via %s to %s:%u -> %lld", a.name.c_str(),
              net_dotted(a.can_broadcast && a.broadcast ? a.broadcast : INADDR_BROADCAST_).c_str(),
              port, (long long)n);
        if (n >= 0)
            result = n;
        else
            last = errno;
    }
    if (result < 0)
        errno = last;
    return result;
}

void do_send(X86 *c, uint32_t h, uint32_t buf, uint32_t len, uint32_t flags, uint32_t to,
             uint32_t tolen) {
    uint32_t usable = 0, err = 0, ip = 0;
    uint16_t port = 0;
    if (!flags_ok(flags)) {
        fail(c, WSAEOPNOTSUPP);
        return;
    }
    if (!guest_buffer(buf, len, &usable) || usable < len) {
        fail(c, WSAEFAULT);
        return;
    }
    bool has_to = to != 0;
    if (has_to && !read_sockaddr(to, tolen, &ip, &port, &err)) {
        fail(c, err);
        return;
    }
    uint32_t sent = 0;
    uint64_t deadline = 0;
    bool deadline_set = false;
    for (;;) {
        Sock snap;
        bool blocking;
        uint32_t timeout;
        {
            std::lock_guard<std::mutex> lock(g_m);
            Sock *s = find_locked(h);
            if (!s) {
                fail(c, sent ? WSAEINTR : WSAENOTSOCK);
                return;
            }
            if (s->shut_send) {
                fail(c, WSAESHUTDOWN);
                return;
            }
            snap = *s;
            snap.waiters.clear();
            blocking = !s->nonblocking;
            timeout = s->sndtimeo ? s->sndtimeo : 0xffffffffu;
            if (s->type == OS_NET_STREAM) {
                settle_connect_locked(*s);
                if (!s->connected) {
                    fail(c, s->connecting ? WSAEWOULDBLOCK : WSAENOTCONN);
                    return;
                }
                has_to = false; // a stream ignores the address, as on Windows
            } else if (!has_to && !s->connected) {
                fail(c, WSAENOTCONN);
                return;
            }
        }
        int64_t n;
        if (snap.type == OS_NET_DGRAM) {
            if (len > MAX_DATAGRAM) {
                fail(c, WSAEMSGSIZE);
                return;
            }
            if (has_to && port == 0) {
                fail(c, WSAEADDRNOTAVAIL);
                return;
            }
            if (has_to && ip == INADDR_BROADCAST_) {
                if (!snap.broadcast) {
                    fail(c, WSAEACCES);
                    return;
                }
                n = send_broadcast(snap, g_mem + buf, len, host_flags(flags), ip, port);
            } else if (has_to) {
                // A directed broadcast goes out once, through its own subnet's
                // interface, and needs SO_BROADCAST on the host as on Windows.
                n = os_net_sendto(snap.fd, g_mem + buf, len, host_flags(flags), ip, port, 0);
            } else {
                n = os_net_send(snap.fd, g_mem + buf, len, host_flags(flags));
            }
            if (n >= 0) {
                std::lock_guard<std::mutex> lock(g_m);
                if (Sock *s = find_locked(h))
                    s->bound = true; // a send gives an unbound socket its address
            }
        } else {
            n = os_net_send(snap.fd, g_mem + buf + sent, len - sent, host_flags(flags));
        }
        if (n < 0) {
            int e = errno;
            if (snap.type == OS_NET_DGRAM && (e == ECONNREFUSED || e == EHOSTUNREACH))
                continue; // an ICMP report about an earlier datagram
            if ((e == EWOULDBLOCK || e == EAGAIN || e == ENOBUFS) && blocking) {
                if (!deadline_set) {
                    deadline = deadline_after(timeout);
                    deadline_set = true;
                }
                if (!wait_ready(h, OS_NET_POLL_OUT, deadline)) {
                    fail(c, WSAETIMEDOUT);
                    return;
                }
                continue;
            }
            if (sent) // a blocking stream send that got part of the way
                break;
            uint32_t w = e == EPIPE && snap.shut_send ? WSAESHUTDOWN : wsa_error(e);
            if (e == ENOBUFS && snap.type == OS_NET_DGRAM)
                w = WSAEWOULDBLOCK;
            trace("send(%#x, %u) -> error %u", h, len, w);
            fail(c, w);
            if (w == WSAEWOULDBLOCK)
                reenable(h, FD_WRITE_);
            return;
        }
        sent += (uint32_t)n;
        if (snap.type == OS_NET_DGRAM || sent >= len || !blocking)
            break;
    }
    trace("send(%#x, %u%s%s) -> %u", h, len, has_to ? " to " : "",
          has_to ? (net_dotted(ip) + ":" + std::to_string(port)).c_str() : "", sent);
    set_eax(c, sent);
}
void ws_send(X86 *c) {
    do_send(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), 0, 0);
}
void ws_sendto(X86 *c) {
    do_send(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5));
}

// WSARecv/WSARecvFrom/WSASend/WSASendTo without overlapped I/O: the buffers
// (WSABUF {len, buf}) are gathered through a bounce buffer, so a datagram
// spans them as it would. With an OVERLAPPED or a completion routine the call
// fails with WSAEOPNOTSUPP, logged once: overlapped sockets are not modelled.
bool wsabufs(uint32_t bufs, uint32_t count, std::vector<std::pair<uint32_t, uint32_t>> *out) {
    if (!count || count > 1024 || !bufs || !gm_valid(bufs, count * 8))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = rd32(bufs + 8 * i), p = rd32(bufs + 8 * i + 4);
        if (len && (!p || !gm_valid(p, len)))
            return false;
        out->push_back({p, len});
    }
    return true;
}
uint32_t g_bounce = 0, g_bounce_size = 0;
uint32_t bounce(uint32_t size) {
    if (g_bounce_size < size || !g_bounce) {
        if (g_bounce)
            heap_free(g_bounce);
        g_bounce = heap_alloc(size ? size : 16, true);
        g_bounce_size = g_bounce ? size : 0;
    }
    return g_bounce;
}
void wsa_transfer(X86 *c, bool receive, bool addressed) {
    // WSARecv(s, bufs, count, *received, *flags, overlapped, routine)
    // WSARecvFrom(s, bufs, count, *received, *flags, from, *fromlen, overlapped, routine)
    // WSASend(s, bufs, count, *sent, flags, overlapped, routine)
    // WSASendTo(s, bufs, count, *sent, flags, to, tolen, overlapped, routine)
    uint32_t h = arg(c, 0), bufs = arg(c, 1), count = arg(c, 2), done = arg(c, 3);
    uint32_t flags_arg = arg(c, 4);
    uint32_t overlapped = arg(c, addressed ? 7 : 5), routine = arg(c, addressed ? 8 : 6);
    if (overlapped || routine) {
        log_once("wsa-overlapped", "Winsock: overlapped I/O is not modelled (WSAEOPNOTSUPP)");
        fail(c, WSAEOPNOTSUPP);
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> v;
    if (!wsabufs(bufs, count, &v) || (receive && (!flags_arg || !gm_valid(flags_arg, 4)))) {
        fail(c, WSAEFAULT);
        return;
    }
    uint32_t total = 0;
    for (auto &b : v)
        total += b.second;
    uint32_t tmp = bounce(total);
    if (!tmp) {
        fail(c, WSAENOBUFS);
        return;
    }
    if (receive) {
        uint32_t flags = rd32(flags_arg);
        do_receive(c, h, tmp, total, flags, addressed ? arg(c, 5) : 0, addressed ? arg(c, 6) : 0);
        uint32_t got = c->r[R_EAX];
        bool truncated = got == SOCKET_ERROR_ && tnet().error == WSAEMSGSIZE;
        if (got == SOCKET_ERROR_ && !truncated)
            return;
        uint32_t n = truncated ? total : got, off = 0;
        for (auto &b : v) {
            uint32_t k = std::min(b.second, n - off);
            memcpy(g_mem + b.first, g_mem + tmp + off, k);
            off += k;
        }
        if (done && gm_valid(done, 4))
            wr32(done, n);
        wr32(flags_arg, truncated ? 0x0100 : 0); // MSG_PARTIAL on Windows means truncated
        if (!truncated)
            set_eax(c, 0);
        return;
    }
    uint32_t off = 0;
    for (auto &b : v) {
        memcpy(g_mem + tmp + off, g_mem + b.first, b.second);
        off += b.second;
    }
    do_send(c, h, tmp, total, flags_arg, addressed ? arg(c, 5) : 0, addressed ? arg(c, 6) : 0);
    if (c->r[R_EAX] == SOCKET_ERROR_)
        return;
    if (done && gm_valid(done, 4))
        wr32(done, c->r[R_EAX]);
    set_eax(c, 0);
}
void ws_WSARecv(X86 *c) {
    wsa_transfer(c, true, false);
}
void ws_WSARecvFrom(X86 *c) {
    wsa_transfer(c, true, true);
}
void ws_WSASend(X86 *c) {
    wsa_transfer(c, false, false);
}
void ws_WSASendTo(X86 *c) {
    wsa_transfer(c, false, true);
}
// WSARecvEx(s, buf, len, *flags): recv, with MSG_PARTIAL for a datagram that
// did not fit.
void ws_WSARecvEx(X86 *c) {
    uint32_t flags_arg = arg(c, 3);
    uint32_t flags = flags_arg && gm_valid(flags_arg, 4) ? rd32(flags_arg) : 0;
    do_receive(c, arg(c, 0), arg(c, 1), arg(c, 2), flags, 0, 0);
    if (c->r[R_EAX] == SOCKET_ERROR_ && tnet().error == WSAEMSGSIZE) {
        if (flags_arg && gm_valid(flags_arg, 4))
            wr32(flags_arg, 0x8000); // MSG_PARTIAL
        set_eax(c, arg(c, 2));
    }
}

// ---------------------------------------------------------------------------
// Addresses of a socket.
// ---------------------------------------------------------------------------
void ws_getsockname(X86 *c) {
    uint32_t h = arg(c, 0), addr = arg(c, 1), lenp = arg(c, 2);
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    if (!s->bound) {
        fail(c, WSAEINVAL); // Windows: an unbound socket has no name
        return;
    }
    uint32_t ip = 0;
    uint16_t port = 0;
    if (os_net_local_address(s->fd, &ip, &port) != 0) {
        fail(c, wsa_error(errno));
        return;
    }
    if (s->iface_bound)
        ip = s->guest_ip;
    if (!addr || !write_sockaddr(addr, lenp, ip, port)) {
        fail(c, WSAEFAULT);
        return;
    }
    set_eax(c, 0);
}
void ws_getpeername(X86 *c) {
    uint32_t h = arg(c, 0), addr = arg(c, 1), lenp = arg(c, 2);
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    settle_connect_locked(*s);
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!s->connected || os_net_peer_address(s->fd, &ip, &port) != 0) {
        fail(c, WSAENOTCONN);
        return;
    }
    if (!addr || !write_sockaddr(addr, lenp, ip, port)) {
        fail(c, WSAEFAULT);
        return;
    }
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Options.
// ---------------------------------------------------------------------------
const uint32_t SO_DEBUG_ = 0x0001, SO_ACCEPTCONN_ = 0x0002, SO_REUSEADDR_ = 0x0004,
               SO_KEEPALIVE_ = 0x0008, SO_DONTROUTE_ = 0x0010, SO_BROADCAST_ = 0x0020,
               SO_USELOOPBACK_ = 0x0040, SO_LINGER_ = 0x0080, SO_OOBINLINE_ = 0x0100,
               SO_DONTLINGER_ = 0xff7f, SO_EXCLUSIVEADDRUSE_ = 0xfffffffb, SO_SNDBUF_ = 0x1001,
               SO_RCVBUF_ = 0x1002, SO_SNDLOWAT_ = 0x1003, SO_RCVLOWAT_ = 0x1004,
               SO_SNDTIMEO_ = 0x1005, SO_RCVTIMEO_ = 0x1006, SO_ERROR_ = 0x1007, SO_TYPE_ = 0x1008,
               SO_MAX_MSG_SIZE_ = 0x2003, SO_CONNECT_TIME_ = 0x700c,
               SO_UPDATE_ACCEPT_CONTEXT_ = 0x700b, SO_UPDATE_CONNECT_CONTEXT_ = 0x7010;
const uint32_t TCP_NODELAY_ = 0x0001;
// IPPROTO_IP options, which Winsock 1.1 numbered differently.
enum IpOpt {
    IP_NONE,
    IP_TTL_,
    IP_TOS_,
    IP_MCAST_IF,
    IP_MCAST_TTL,
    IP_MCAST_LOOP,
    IP_ADD,
    IP_DROP,
    IP_DONTFRAG_,
    IP_OPTIONS_,
    IP_HDRINCL_
};
IpOpt ip_option(uint32_t name, bool winsock2) {
    if (winsock2) {
        switch (name) {
        case 1:
            return IP_OPTIONS_;
        case 2:
            return IP_HDRINCL_;
        case 3:
            return IP_TOS_;
        case 4:
            return IP_TTL_;
        case 9:
            return IP_MCAST_IF;
        case 10:
            return IP_MCAST_TTL;
        case 11:
            return IP_MCAST_LOOP;
        case 12:
            return IP_ADD;
        case 13:
            return IP_DROP;
        case 14:
            return IP_DONTFRAG_;
        default:
            return IP_NONE;
        }
    }
    switch (name) {
    case 1:
        return IP_OPTIONS_;
    case 2:
        return IP_MCAST_IF;
    case 3:
        return IP_MCAST_TTL;
    case 4:
        return IP_MCAST_LOOP;
    case 5:
        return IP_ADD;
    case 6:
        return IP_DROP;
    case 7:
        return IP_TTL_;
    case 8:
        return IP_TOS_;
    case 9:
        return IP_DONTFRAG_;
    default:
        return IP_NONE;
    }
}

// An option value: up to four little-endian bytes (Windows takes a BOOL in a
// char as readily as in an int).
bool option_value(uint32_t p, uint32_t len, uint32_t *v) {
    if (!p || (int32_t)len < 1 || !gm_valid(p, len < 4 ? len : 4))
        return false;
    *v = len >= 4 ? rd32(p) : len >= 2 ? rd16(p) : rd8(p);
    return true;
}

void sockopt(X86 *c, bool set, bool winsock2) {
    uint32_t h = arg(c, 0), level = arg(c, 1), name = arg(c, 2), p = arg(c, 3), lenarg = arg(c, 4);
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    uint32_t len = 0;
    if (set) {
        len = lenarg;
    } else {
        if (!lenarg || !gm_valid(lenarg, 4)) {
            fail(c, WSAEFAULT);
            return;
        }
        len = rd32(lenarg);
    }
    uint32_t v = 0;
    if (set && name != SO_DONTLINGER_ &&
        !(level == IPPROTO_IP_ && ip_option(name, winsock2) == IP_ADD) &&
        !(level == IPPROTO_IP_ && ip_option(name, winsock2) == IP_DROP) &&
        !option_value(p, len, &v)) {
        fail(c, WSAEFAULT);
        return;
    }
    if (!set && (!p || (int32_t)len < 1 || !gm_valid(p, len))) {
        fail(c, WSAEFAULT);
        return;
    }
    // Writes a get's answer: an int, or a BOOL squeezed into a shorter buffer.
    auto answer = [&](uint32_t value) {
        if (len >= 4) {
            wr32(p, value);
            wr32(lenarg, 4);
        } else if (len == 1) {
            wr8(p, (uint8_t)value);
            wr32(lenarg, 1);
        } else {
            wr16(p, (uint16_t)value);
            wr32(lenarg, 2);
        }
        set_eax(c, 0);
    };
    auto host = [&](int option) {
        if (set) {
            if (os_net_set_option(s->fd, option, (int64_t)v) != 0) {
                fail(c, wsa_error(errno));
                return;
            }
            set_eax(c, 0);
            return;
        }
        int64_t got = 0;
        if (os_net_get_option(s->fd, option, &got) != 0) {
            fail(c, wsa_error(errno));
            return;
        }
        answer((uint32_t)got);
    };
    auto flag = [&](bool &field) {
        if (set) {
            field = v != 0;
            set_eax(c, 0);
        } else {
            answer(field ? 1 : 0);
        }
    };
    if (level == SOL_SOCKET_) {
        switch (name) {
        case SO_BROADCAST_:
            if (s->type != OS_NET_DGRAM && set) {
                s->broadcast = v != 0; // meaningless on a stream, but accepted
                set_eax(c, 0);
                return;
            }
            if (set && os_net_set_option(s->fd, OS_NET_OPT_BROADCAST, v != 0) != 0) {
                fail(c, wsa_error(errno));
                return;
            }
            flag(s->broadcast);
            return;
        case SO_REUSEADDR_:
            if (set && s->type == OS_NET_DGRAM) {
                // Windows lets several datagram sockets share a port with it;
                // BSD wants SO_REUSEPORT for that.
                os_net_set_option(s->fd, OS_NET_OPT_REUSEADDR, v != 0);
                os_net_set_option(s->fd, OS_NET_OPT_REUSEPORT, v != 0);
            }
            flag(s->reuse);
            return;
        case SO_EXCLUSIVEADDRUSE_:
            flag(s->exclusive);
            return;
        case SO_DEBUG_:
            flag(s->debug);
            return;
        case SO_USELOOPBACK_:
            flag(s->use_loopback);
            return;
        case SO_KEEPALIVE_:
            host(OS_NET_OPT_KEEPALIVE);
            return;
        case SO_DONTROUTE_:
            host(OS_NET_OPT_DONTROUTE);
            return;
        case SO_OOBINLINE_:
            host(OS_NET_OPT_OOBINLINE);
            return;
        case SO_SNDBUF_:
            host(OS_NET_OPT_SNDBUF);
            return;
        case SO_RCVBUF_:
            host(OS_NET_OPT_RCVBUF);
            return;
        case SO_ACCEPTCONN_:
            if (set) {
                fail(c, WSAENOPROTOOPT);
                return;
            }
            answer(s->listening ? 1 : 0);
            return;
        case SO_TYPE_:
            if (set) {
                fail(c, WSAENOPROTOOPT);
                return;
            }
            answer(s->type == OS_NET_STREAM ? SOCK_STREAM_ : SOCK_DGRAM_);
            return;
        case SO_ERROR_: {
            if (set) {
                fail(c, WSAENOPROTOOPT);
                return;
            }
            settle_connect_locked(*s);
            int e = s->connect_error;
            s->connect_error = 0;
            if (!e && !s->connecting)
                e = os_net_pending_error(s->fd);
            if (s->type == OS_NET_DGRAM && (e == ECONNREFUSED || e == EHOSTUNREACH))
                e = 0;
            answer(wsa_error(e));
            return;
        }
        case SO_SNDTIMEO_:
        case SO_RCVTIMEO_: {
            uint32_t &field = name == SO_SNDTIMEO_ ? s->sndtimeo : s->rcvtimeo;
            if (set) {
                field = v;
                set_eax(c, 0);
            } else {
                answer(field);
            }
            return;
        }
        case SO_LINGER_: {
            // LINGER is two u_shorts on Windows: l_onoff, l_linger.
            if (set) {
                if ((int32_t)len < 4) {
                    fail(c, WSAEFAULT);
                    return;
                }
                uint16_t on = rd16(p), secs = rd16(p + 2);
                if (os_net_set_option(s->fd, OS_NET_OPT_LINGER, on ? secs : -1) != 0) {
                    fail(c, wsa_error(errno));
                    return;
                }
                set_eax(c, 0);
                return;
            }
            if ((int32_t)len < 4) {
                fail(c, WSAEFAULT);
                return;
            }
            int64_t l = -1;
            os_net_get_option(s->fd, OS_NET_OPT_LINGER, &l);
            wr16(p, l >= 0 ? 1 : 0);
            wr16(p + 2, (uint16_t)(l >= 0 ? l : 0));
            wr32(lenarg, 4);
            set_eax(c, 0);
            return;
        }
        case SO_DONTLINGER_:
            if (set) {
                uint32_t off = 1;
                if (p && (int32_t)len >= 1)
                    option_value(p, len, &off);
                if (off)
                    os_net_set_option(s->fd, OS_NET_OPT_LINGER, -1);
                set_eax(c, 0);
                return;
            } else {
                int64_t l = -1;
                os_net_get_option(s->fd, OS_NET_OPT_LINGER, &l);
                answer(l < 0 ? 1 : 0);
                return;
            }
        case SO_MAX_MSG_SIZE_:
            if (set || s->type != OS_NET_DGRAM) {
                fail(c, set ? WSAENOPROTOOPT : WSAENOPROTOOPT);
                return;
            }
            answer(MAX_DATAGRAM);
            return;
        case SO_SNDLOWAT_:
        case SO_RCVLOWAT_:
            if (set) {
                fail(c, WSAENOPROTOOPT); // as on Windows
                return;
            }
            answer(1);
            return;
        case SO_CONNECT_TIME_:
            if (set) {
                fail(c, WSAENOPROTOOPT);
                return;
            }
            answer(s->connected ? 0 : 0xffffffffu);
            return;
        case SO_UPDATE_ACCEPT_CONTEXT_:
        case SO_UPDATE_CONNECT_CONTEXT_:
            set_eax(c, 0);
            return;
        default:
            fail(c, WSAENOPROTOOPT);
            return;
        }
    }
    if (level == IPPROTO_TCP_) {
        if (s->type != OS_NET_STREAM) {
            fail(c, WSAENOPROTOOPT);
            return;
        }
        if (name == TCP_NODELAY_) {
            host(OS_NET_OPT_NODELAY);
            return;
        }
        fail(c, WSAENOPROTOOPT);
        return;
    }
    if (level == IPPROTO_IP_) {
        switch (ip_option(name, winsock2)) {
        case IP_TTL_:
            host(OS_NET_OPT_TTL);
            return;
        case IP_TOS_:
            host(OS_NET_OPT_TOS);
            return;
        case IP_MCAST_TTL:
            host(OS_NET_OPT_MULTICAST_TTL);
            return;
        case IP_MCAST_LOOP:
            host(OS_NET_OPT_MULTICAST_LOOP);
            return;
        case IP_MCAST_IF:
            if (set) {
                if (os_net_set_option(s->fd, OS_NET_OPT_MULTICAST_IF, swap32(v)) != 0) {
                    fail(c, wsa_error(errno));
                    return;
                }
                set_eax(c, 0);
            } else {
                int64_t got = 0;
                os_net_get_option(s->fd, OS_NET_OPT_MULTICAST_IF, &got);
                answer(swap32((uint32_t)got));
            }
            return;
        case IP_ADD:
        case IP_DROP:
            // ip_mreq: the group and the interface, both network order.
            if (!set || !p || (int32_t)len < 8 || !gm_valid(p, 8)) {
                fail(c, set ? WSAEFAULT : WSAENOPROTOOPT);
                return;
            }
            if (os_net_membership(s->fd, ip_option(name, winsock2) == IP_ADD, ip_at(p),
                                  ip_at(p + 4)) != 0) {
                fail(c, wsa_error(errno));
                return;
            }
            set_eax(c, 0);
            return;
        case IP_DONTFRAG_:
            flag(s->dont_fragment);
            return;
        case IP_OPTIONS_:
            if (set) {
                set_eax(c, 0); // no IP options are sent
            } else {
                wr32(lenarg, 0);
                set_eax(c, 0);
            }
            return;
        case IP_HDRINCL_:
            fail(c, WSAEINVAL); // raw sockets only
            return;
        default:
            fail(c, WSAENOPROTOOPT);
            return;
        }
    }
    fail(c, WSAEINVAL);
}
void ws1_setsockopt(X86 *c) {
    sockopt(c, true, false);
}
void ws1_getsockopt(X86 *c) {
    sockopt(c, false, false);
}
void ws2_setsockopt(X86 *c) {
    sockopt(c, true, true);
}
void ws2_getsockopt(X86 *c) {
    sockopt(c, false, true);
}

// ioctlsocket(s, cmd, *argp).
uint32_t socket_ioctl(Sock &s, uint32_t cmd, uint32_t argp) {
    if (!argp || !gm_valid(argp, 4))
        return WSAEFAULT;
    switch (cmd) {
    case FIONBIO_: {
        bool on = rd32(argp) != 0;
        if (!on && s.es_event && s.es_mask)
            return WSAEINVAL; // an event selection keeps it non-blocking
        s.nonblocking = on;
        return 0;
    }
    case FIONREAD_: {
        uint32_t n = 0;
        if (s.listening) {
            wr32(argp, 0);
            return 0;
        }
        if (os_net_readable(s.fd, &n) != 0)
            return wsa_error(errno);
        wr32(argp, n);
        return 0;
    }
    case SIOCATMARK_:
        wr32(argp, 1); // no urgent data waiting
        return 0;
    default:
        return WSAEINVAL;
    }
}
void ws_ioctlsocket(X86 *c) {
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(arg(c, 0));
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    uint32_t e = socket_ioctl(*s, arg(c, 1), arg(c, 2));
    if (e) {
        fail(c, e);
        return;
    }
    set_eax(c, 0);
}

// SIO_GET_INTERFACE_LIST: an INTERFACE_INFO (76 bytes: flags, then address,
// broadcast address and netmask as 24-byte sockaddr_gen) per adapter, and the
// loopback address, as Windows lists it.
uint32_t interface_list(uint32_t out, uint32_t outlen, uint32_t *written) {
    std::vector<NetAdapter> adapters = net_adapters();
    struct Row {
        uint32_t flags, ip, broadcast, mask;
    };
    std::vector<Row> rows;
    for (const NetAdapter &a : adapters)
        for (const NetAddress &addr : a.addresses)
            rows.push_back({1u | (a.can_broadcast ? 2u : 0u) | (a.point_to_point ? 8u : 0u) | 16u,
                            addr.ip, a.can_broadcast ? (addr.ip | ~addr.mask) : 0, addr.mask});
    rows.push_back({1u | 4u | 16u, 0x7f000001u, 0, 0xff000000u});
    uint32_t need = (uint32_t)rows.size() * 76;
    if (!out || outlen < need || !gm_valid(out, need)) {
        *written = need;
        return WSAEFAULT;
    }
    memset(g_mem + out, 0, need);
    for (size_t i = 0; i < rows.size(); ++i) {
        uint32_t r = out + (uint32_t)i * 76;
        wr32(r, rows[i].flags);
        const uint32_t addrs[3] = {rows[i].ip, rows[i].broadcast, rows[i].mask};
        for (int k = 0; k < 3; ++k) {
            uint32_t sa = r + 4 + 24 * (uint32_t)k;
            wr16(sa, AF_INET_);
            put_ip(sa + 4, addrs[k]);
        }
    }
    *written = need;
    return 0;
}
// WSAIoctl(s, code, in, inlen, out, outlen, *returned, overlapped, routine).
void ws_WSAIoctl(X86 *c) {
    uint32_t h = arg(c, 0), code = arg(c, 1), in = arg(c, 2), inlen = arg(c, 3);
    uint32_t out = arg(c, 4), outlen = arg(c, 5), returned = arg(c, 6);
    if (arg(c, 7) || arg(c, 8)) {
        log_once("wsa-overlapped", "Winsock: overlapped I/O is not modelled (WSAEOPNOTSUPP)");
        fail(c, WSAEOPNOTSUPP);
        return;
    }
    std::lock_guard<std::mutex> lock(g_m);
    Sock *s = find_locked(h);
    if (!s) {
        fail(c, WSAENOTSOCK);
        return;
    }
    uint32_t e = 0, written = 0;
    switch (code) {
    case FIONBIO_:
        e = inlen >= 4 ? socket_ioctl(*s, code, in) : WSAEFAULT;
        break;
    case FIONREAD_:
    case SIOCATMARK_:
        e = outlen >= 4 ? socket_ioctl(*s, code, out) : WSAEFAULT;
        written = 4;
        break;
    case 0x4004747f: // SIO_GET_INTERFACE_LIST
        e = interface_list(out, outlen, &written);
        break;
    case 0x9800000c: // SIO_UDP_CONNRESET: ICMP reports are never surfaced here anyway
    case 0x98000004: // SIO_KEEPALIVE_VALS
        break;
    default:
        e = WSAEOPNOTSUPP;
        break;
    }
    if (returned && gm_valid(returned, 4))
        wr32(returned, written);
    if (e) {
        fail(c, e);
        return;
    }
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// select: fd_set is {u_int fd_count; SOCKET fd_array[fd_count]}.
// ---------------------------------------------------------------------------
struct SelectEntry {
    uint32_t handle;
    int set; // 0 read, 1 write, 2 except
};

void ws_select(X86 *c) {
    uint32_t sets[3] = {arg(c, 1), arg(c, 2), arg(c, 3)};
    uint32_t tv = arg(c, 4);
    std::vector<SelectEntry> entries;
    for (int k = 0; k < 3; ++k) {
        uint32_t fs = sets[k];
        if (!fs)
            continue;
        if (!gm_valid(fs, 4)) {
            fail(c, WSAEFAULT);
            return;
        }
        uint32_t n = rd32(fs);
        if (n > 0x10000 || !gm_valid(fs + 4, n * 4)) {
            fail(c, WSAEFAULT);
            return;
        }
        for (uint32_t i = 0; i < n; ++i)
            entries.push_back({rd32(fs + 4 + 4 * i), k});
    }
    if (entries.empty()) {
        fail(c, WSAEINVAL);
        return;
    }
    if (!started_or_fail(c))
        return;
    uint32_t timeout = 0xffffffffu;
    if (tv) {
        if (!gm_valid(tv, 8)) {
            fail(c, WSAEFAULT);
            return;
        }
        int32_t sec = (int32_t)rd32(tv), usec = (int32_t)rd32(tv + 4);
        if (sec < 0 || usec < 0) {
            fail(c, WSAEINVAL);
            return;
        }
        uint64_t ms = (uint64_t)sec * 1000u + ((uint64_t)usec + 999u) / 1000u;
        timeout = ms >= 0xfffffffeull ? 0xfffffffeu : (uint32_t)ms;
    }
    const uint64_t start = os_monotonic_ns();
    for (;;) {
        std::vector<bool> ready(entries.size(), false);
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(g_m);
            std::vector<OsNetPoll> polls;
            for (const SelectEntry &e : entries) {
                Sock *s = find_locked(e.handle);
                if (!s) {
                    fail(c, WSAENOTSOCK);
                    return;
                }
                int want = e.set == 0   ? OS_NET_POLL_IN
                           : e.set == 1 ? OS_NET_POLL_OUT
                                        : OS_NET_POLL_PRI;
                polls.push_back({s->fd, want, 0});
            }
            os_net_poll(polls.data(), polls.size(), 0);
            for (size_t i = 0; i < entries.size(); ++i) {
                Sock &s = *find_locked(entries[i].handle);
                int got = polls[i].got;
                bool r = false;
                switch (entries[i].set) {
                case 0:
                    if (s.listening)
                        r = (got & OS_NET_POLL_IN) != 0;
                    else if (s.type == OS_NET_DGRAM) {
                        r = (got & OS_NET_POLL_IN) != 0;
                        if ((got & OS_NET_POLL_ERR) && !r)
                            os_net_pending_error(s.fd); // ICMP: not readable on Windows
                    } else
                        r = s.connected &&
                            (got & (OS_NET_POLL_IN | OS_NET_POLL_HUP | OS_NET_POLL_ERR)) != 0;
                    break;
                case 1: // writable, or a connect that has succeeded
                    settle_connect_locked(s);
                    r = !s.connecting && (s.connected || s.type == OS_NET_DGRAM) &&
                        (got & OS_NET_POLL_OUT) != 0;
                    break;
                case 2: // a connect that has failed, or urgent data
                    settle_connect_locked(s);
                    r = (!s.connecting && !s.connected && s.connect_error != 0 &&
                         s.type == OS_NET_STREAM) ||
                        (got & OS_NET_POLL_PRI) != 0;
                    break;
                }
                ready[i] = r;
                count += r ? 1 : 0;
            }
        }
        uint32_t elapsed = (uint32_t)((os_monotonic_ns() - start) / 1000000ull);
        if (count || timeout == 0 || (timeout != 0xffffffffu && elapsed >= timeout)) {
            // Each set keeps only its ready sockets.
            for (int k = 0; k < 3; ++k) {
                uint32_t fs = sets[k];
                if (!fs)
                    continue;
                uint32_t n = 0;
                for (size_t i = 0; i < entries.size(); ++i)
                    if (entries[i].set == k && ready[i])
                        wr32(fs + 4 + 4 * n++, entries[i].handle);
                wr32(fs, n);
            }
            set_eax(c, (uint32_t)count);
            return;
        }
        // Wait for any of them, then look again.
        start_monitor();
        uint32_t ev = thread_wait_event();
        {
            std::lock_guard<std::mutex> lock(g_m);
            for (const SelectEntry &e : entries) {
                Sock *s = find_locked(e.handle);
                int want = e.set == 0   ? OS_NET_POLL_IN
                           : e.set == 1 ? OS_NET_POLL_OUT
                                        : OS_NET_POLL_PRI;
                s->waiters.push_back({ev, want});
            }
        }
        wake_monitor();
        uint32_t slice = timeout == 0xffffffffu ? 1000u : std::min(1000u, timeout - elapsed);
        guest_wait_objects(&ev, 1, false, slice);
        std::lock_guard<std::mutex> lock(g_m);
        for (const SelectEntry &e : entries)
            if (Sock *s = find_locked(e.handle))
                s->waiters.erase(std::remove_if(s->waiters.begin(), s->waiters.end(),
                                                [ev](const Waiter &w) { return w.event == ev; }),
                                 s->waiters.end());
    }
}

// __WSAFDIsSet(s, set): whether s is among set's first fd_count entries.
void ws___WSAFDIsSet(X86 *c) {
    uint32_t h = arg(c, 0), fs = arg(c, 1);
    if (!fs || !gm_valid(fs, 4)) {
        set_eax(c, 0);
        return;
    }
    uint32_t n = rd32(fs);
    if (n > 0x10000 || !gm_valid(fs + 4, n * 4)) {
        set_eax(c, 0);
        return;
    }
    for (uint32_t i = 0; i < n; ++i)
        if (rd32(fs + 4 + 4 * i) == h) {
            set_eax(c, 1);
            return;
        }
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// WSAEventSelect and the WSA event objects (kernel32 events, as on Windows).
// ---------------------------------------------------------------------------
void ws_WSAEventSelect(X86 *c) {
    uint32_t h = arg(c, 0), event = arg(c, 1), mask = arg(c, 2) & FD_ALL_;
    std::vector<uint32_t> signal;
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s) {
            fail(c, WSAENOTSOCK);
            return;
        }
        if (mask && !event) {
            fail(c, WSAEINVAL);
            return;
        }
        // A new selection replaces the old one and clears the record.
        s->es_event = mask ? event : 0;
        s->es_mask = mask;
        s->es_armed = mask;
        s->es_recorded = 0;
        memset(s->es_error, 0, sizeof s->es_error);
        s->quiet_in = s->quiet_out = false;
        s->nonblocking = true;
        if (mask) {
            // What already holds is recorded now. FD_CONNECT is recorded when
            // a connect finishes, and FD_WRITE on a stream that has not
            // connected stays armed for the moment it does.
            int got = probe(s->fd, OS_NET_POLL_IN | OS_NET_POLL_OUT | OS_NET_POLL_PRI);
            if (got)
                evaluate_locked(*s, got, signal);
        }
        trace("WSAEventSelect(%#x, event %#x, %#x)%s", h, event, mask,
              signal.empty() ? "" : " -> signalled at once");
    }
    signal_now(signal);
    if (mask)
        start_monitor();
    wake_monitor();
    set_eax(c, 0);
}

// WSAEnumNetworkEvents(s, hEvent, lpNetworkEvents): the record since the last
// call (WSANETWORKEVENTS: lNetworkEvents, iErrorCode[10]), cleared, and the
// event reset.
void ws_WSAEnumNetworkEvents(X86 *c) {
    uint32_t h = arg(c, 0), event = arg(c, 1), out = arg(c, 2);
    if (!out || !gm_valid(out, 44)) {
        fail(c, WSAEFAULT);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_m);
        Sock *s = find_locked(h);
        if (!s) {
            fail(c, WSAENOTSOCK);
            return;
        }
        wr32(out, s->es_recorded);
        for (int i = 0; i < 10; ++i)
            wr32(out + 4 + 4 * (uint32_t)i, (s->es_recorded >> i) & 1 ? s->es_error[i] : 0);
        s->es_recorded = 0;
        memset(s->es_error, 0, sizeof s->es_error);
    }
    if (event)
        win32_reset_event(event);
    set_eax(c, 0);
}

void ws_WSACreateEvent(X86 *c) {
    set_eax(c, win32_create_event(true, false)); // manual reset, as WSA events are
}
void ws_WSACloseEvent(X86 *c) {
    uint32_t r = guest_call(c, imports_resolve("KERNEL32.dll", "CloseHandle"), arg(c, 0));
    if (!r)
        set_wsa(6); // WSA_INVALID_HANDLE
    set_eax(c, r);
}
void ws_WSASetEvent(X86 *c) {
    bool ok = win32_signal_event(arg(c, 0), false);
    if (!ok)
        set_wsa(6);
    set_eax(c, ok ? 1 : 0);
}
void ws_WSAResetEvent(X86 *c) {
    bool ok = win32_reset_event(arg(c, 0));
    if (!ok)
        set_wsa(6);
    set_eax(c, ok ? 1 : 0);
}
// WSAWaitForMultipleEvents(count, events, waitAll, timeout, alertable): the
// kernel32 wait, whose return values it shares.
void ws_WSAWaitForMultipleEvents(X86 *c) {
    uint32_t n = arg(c, 0), p = arg(c, 1);
    if (!n || n > 64 || !p || !gm_valid(p, n * 4)) {
        fail(c, n ? WSAEFAULT : WSAEINVAL, 0xffffffffu);
        return;
    }
    std::vector<uint32_t> handles(n);
    for (uint32_t i = 0; i < n; ++i)
        handles[i] = rd32(p + 4 * i);
    set_eax(c, guest_wait_objects(handles.data(), n, arg(c, 2) != 0, arg(c, 3)));
}

// ---------------------------------------------------------------------------
// Names.
// ---------------------------------------------------------------------------
// The host's name up to its first dot, as a Windows host name is.
std::string host_name() {
    char buf[256] = {0};
    if (os_net_hostname(buf, sizeof buf) != 0 || !buf[0])
        return "localhost";
    std::string s(buf);
    size_t dot = s.find('.');
    if (dot != std::string::npos && dot > 0)
        s.resize(dot);
    return s;
}

bool is_own_name(const std::string &name) {
    if (name.empty())
        return true;
    std::string own = host_name();
    if (os_strcasecmp(name.c_str(), own.c_str()) == 0)
        return true;
    // The same name with a domain: "host.local", "host.example".
    return name.size() > own.size() && name[own.size()] == '.' &&
           os_strcasecmp(name.substr(0, own.size()).c_str(), own.c_str()) == 0;
}

// Names that must fail at once rather than reach a resolver: the game's
// [network] offline_hosts and RECOMP_NET_OFFLINE_HOSTS (comma separated).
bool is_offline_host(const std::string &name) {
    static const char *const configured[] = RECOMP_NET_OFFLINE_HOSTS;
    for (const char *const *p = configured; *p; ++p)
        if (os_strcasecmp(*p, name.c_str()) == 0)
            return true;
    if (const char *env = recomp_env("NET_OFFLINE_HOSTS")) {
        std::string list(env), cur;
        for (size_t i = 0; i <= list.size(); ++i) {
            if (i == list.size() || list[i] == ',' || list[i] == ' ') {
                if (!cur.empty() && os_strcasecmp(cur.c_str(), name.c_str()) == 0)
                    return true;
                cur.clear();
            } else {
                cur += list[i];
            }
        }
    }
    return false;
}

// HOSTENT {h_name, h_aliases, h_addrtype:16, h_length:16, h_addr_list} in the
// thread's block: the alias list (empty) at +16, up to 35 address pointers
// and a null at +20, the addresses at +164, the name at +304.
const uint32_t HOSTENT_BLOCK = 0x240, HOSTENT_MAX_ADDRS = 35;
uint32_t write_hostent(const std::string &name, const std::vector<uint32_t> &ips) {
    uint32_t b = thread_block(tnet().hostent, HOSTENT_BLOCK);
    if (!b)
        return 0;
    memset(g_mem + b, 0, HOSTENT_BLOCK);
    gm_put_str(b + 304, name.c_str(), 256);
    wr32(b + 0, b + 304);
    wr32(b + 4, b + 16);
    wr16(b + 8, AF_INET_);
    wr16(b + 10, 4);
    wr32(b + 12, b + 20);
    uint32_t n = std::min<uint32_t>((uint32_t)ips.size(), HOSTENT_MAX_ADDRS);
    for (uint32_t i = 0; i < n; ++i) {
        put_ip(b + 164 + 4 * i, ips[i]);
        wr32(b + 20 + 4 * i, b + 164 + 4 * i);
    }
    return b;
}

// The system resolver, off the baton: a host thread asks, the guest thread
// waits as for any other event, and a lookup that outlives the wait is left
// to finish on its own.
WinsockResolver g_resolver = nullptr;
uint32_t g_resolve_timeout_ms = 10000;
struct Lookup {
    std::string name;
    WinsockResolver fn;
    std::atomic<bool> done{false};
    int count = 0;
    uint32_t ips[HOSTENT_MAX_ADDRS] = {};
    char canonical[256] = {0};
    uint32_t event = 0;
};
void *lookup_main(void *arg) {
    auto *keep = (std::shared_ptr<Lookup> *)arg;
    std::shared_ptr<Lookup> l = *keep;
    delete keep;
    WinsockResolver fn = l->fn ? l->fn : os_net_resolve;
    l->count =
        fn(l->name.c_str(), l->ips, (int)HOSTENT_MAX_ADDRS, l->canonical, sizeof l->canonical);
    l->done.store(true);
    guest_event_signal_from_host(l->event);
    return nullptr;
}
// The addresses for a name the resolver must answer, or a WSA error.
uint32_t resolve(const std::string &name, std::vector<uint32_t> *ips, std::string *canonical) {
    auto l = std::make_shared<Lookup>();
    l->name = name;
    l->fn = g_resolver;
    l->event = thread_wait_event();
    OsThread *t = os_thread_create(lookup_main, new std::shared_ptr<Lookup>(l), 0);
    if (!t)
        return WSANO_RECOVERY;
    os_thread_detach(t);
    const uint64_t start = os_monotonic_ns();
    while (!l->done.load()) {
        uint32_t elapsed = (uint32_t)((os_monotonic_ns() - start) / 1000000ull);
        if (elapsed >= g_resolve_timeout_ms)
            return WSATRY_AGAIN;
        guest_wait_objects(&l->event, 1, false, std::min(1000u, g_resolve_timeout_ms - elapsed));
    }
    if (l->count < 0) {
        switch (l->count) {
        case OS_NET_RESOLVE_NOT_FOUND:
            return WSAHOST_NOT_FOUND;
        case OS_NET_RESOLVE_AGAIN:
            return WSATRY_AGAIN;
        case OS_NET_RESOLVE_NO_DATA:
            return WSANO_DATA;
        default:
            return WSANO_RECOVERY;
        }
    }
    for (int i = 0; i < l->count && i < (int)HOSTENT_MAX_ADDRS; ++i)
        ips->push_back(l->ips[i]);
    *canonical = l->canonical[0] ? l->canonical : name;
    return 0;
}

void ws_gethostname(X86 *c) {
    uint32_t buf = arg(c, 0), len = arg(c, 1);
    if (!started_or_fail(c))
        return;
    std::string name = host_name();
    if (!buf || (int32_t)len <= (int32_t)name.size() || !gm_valid(buf, (uint32_t)name.size() + 1)) {
        fail(c, WSAEFAULT);
        return;
    }
    gm_put_str(buf, name.c_str(), len);
    set_eax(c, 0);
}

void ws_gethostbyname(X86 *c) {
    if (!started_or_fail(c, 0))
        return;
    std::string name = arg(c, 0) ? gm_str(arg(c, 0), 256) : std::string();
    uint32_t ip = 0;
    std::vector<uint32_t> ips;
    std::string official = name;
    if (!name.empty() && parse_ipv4(name.c_str(), &ip)) {
        ips.push_back(ip);
    } else if (os_strcasecmp(name.c_str(), "localhost") == 0) {
        ips.push_back(0x7f000001u);
    } else if (is_own_name(name)) {
        official = host_name();
        ips = net_local_addresses();
        if (ips.empty())
            ips.push_back(0x7f000001u); // no network: the loopback address, as Windows answers
    } else if (is_offline_host(name) ||
               (recomp_env("NET_DNS") && atoi(recomp_env("NET_DNS")) == 0)) {
        trace("gethostbyname(\"%s\") -> not found at once", name.c_str());
        fail(c, WSAHOST_NOT_FOUND, 0);
        return;
    } else {
        uint32_t e = resolve(name, &ips, &official);
        if (e) {
            trace("gethostbyname(\"%s\") -> error %u", name.c_str(), e);
            fail(c, e, 0);
            return;
        }
    }
    uint32_t b = write_hostent(official, ips);
    if (!b) {
        fail(c, WSANO_RECOVERY, 0);
        return;
    }
    if (g_trace) {
        std::string list;
        for (uint32_t a : ips)
            list += (list.empty() ? "" : ",") + net_dotted(a);
        trace("gethostbyname(\"%s\") -> %s", name.c_str(), list.c_str());
    }
    set_eax(c, b);
}

// gethostbyaddr(addr, len, type): no reverse lookup. The own addresses are
// the own host, the loopback address is "localhost", and any other address is
// answered with itself as the name - what a lookup of a dotted address gives,
// and what a LAN program that falls back to gethostbyaddr wants: its address
// back, at once, rather than a query to a DNS server about a LAN address.
void ws_gethostbyaddr(X86 *c) {
    uint32_t p = arg(c, 0), len = arg(c, 1), type = arg(c, 2);
    if (!started_or_fail(c, 0))
        return;
    if (type != AF_INET_) {
        fail(c, WSAEAFNOSUPPORT, 0);
        return;
    }
    if (!p || len < 4 || !gm_valid(p, 4)) {
        fail(c, WSAEFAULT, 0);
        return;
    }
    uint32_t ip = ip_at(p);
    std::string name = net_dotted(ip);
    if ((ip >> 24) == 127)
        name = "localhost";
    for (uint32_t own : net_local_addresses())
        if (own == ip)
            name = host_name();
    uint32_t b = write_hostent(name, {ip});
    if (!b)
        set_wsa(WSANO_RECOVERY);
    set_eax(c, b);
}

} // namespace

uint32_t winsock_error_from_errno(int e) {
    return wsa_error(e);
}
void winsock_set_resolver_for_test(WinsockResolver fn) {
    g_resolver = fn;
}
void winsock_set_resolve_timeout_for_test(uint32_t ms) {
    g_resolve_timeout_ms = ms ? ms : 10000;
}
size_t winsock_open_sockets() {
    std::lock_guard<std::mutex> lock(g_m);
    return socks().size();
}

// ---------------------------------------------------------------------------
// Registration. Each export is registered under its name for both DLLs. An
// import by ordinal binds by name already (the loader asks
// imports_ordinal_name); GetProcAddress by ordinal asks for "#N", which is
// registered too.
// ---------------------------------------------------------------------------
namespace {
struct Export {
    const char *name;
    uint8_t argc;
    void (*fn)(X86 *);
    void (*fn_ws1)(X86 *); // WSOCK32's own, where its ABI differs; null: fn
    bool ws2_only;
};
const Export kExports[] = {
    {"WSAStartup", 2, ws_WSAStartup, nullptr, false},
    {"WSACleanup", 0, ws_WSACleanup, nullptr, false},
    {"WSAGetLastError", 0, ws_WSAGetLastError, nullptr, false},
    {"WSASetLastError", 1, ws_WSASetLastError, nullptr, false},
    {"WSAIsBlocking", 0, ws_WSAIsBlocking, nullptr, false},
    {"WSACancelBlockingCall", 0, ws_WSACancelBlockingCall, nullptr, false},
    {"WSASetBlockingHook", 1, ws_WSASetBlockingHook, nullptr, false},
    {"WSAUnhookBlockingHook", 0, ws_WSAUnhookBlockingHook, nullptr, false},
    {"__WSAFDIsSet", 2, ws___WSAFDIsSet, nullptr, false},
    {"accept", 3, ws_accept, nullptr, false},
    {"bind", 3, ws_bind, nullptr, false},
    {"closesocket", 1, ws_closesocket, nullptr, false},
    {"connect", 3, ws_connect, nullptr, false},
    {"getpeername", 3, ws_getpeername, nullptr, false},
    {"getsockname", 3, ws_getsockname, nullptr, false},
    {"getsockopt", 5, ws2_getsockopt, ws1_getsockopt, false},
    {"setsockopt", 5, ws2_setsockopt, ws1_setsockopt, false},
    {"htonl", 1, ws_htonl, nullptr, false},
    {"htons", 1, ws_htons, nullptr, false},
    {"ntohl", 1, ws_htonl, nullptr, false},
    {"ntohs", 1, ws_htons, nullptr, false},
    {"inet_addr", 1, ws_inet_addr, nullptr, false},
    {"inet_ntoa", 1, ws_inet_ntoa, nullptr, false},
    {"ioctlsocket", 3, ws_ioctlsocket, nullptr, false},
    {"listen", 2, ws_listen, nullptr, false},
    {"recv", 4, ws_recv, nullptr, false},
    {"recvfrom", 6, ws_recvfrom, nullptr, false},
    {"select", 5, ws_select, nullptr, false},
    {"send", 4, ws_send, nullptr, false},
    {"sendto", 6, ws_sendto, nullptr, false},
    {"shutdown", 2, ws_shutdown, nullptr, false},
    {"socket", 3, ws_socket, nullptr, false},
    {"gethostbyaddr", 3, ws_gethostbyaddr, nullptr, false},
    {"gethostbyname", 1, ws_gethostbyname, nullptr, false},
    {"gethostname", 2, ws_gethostname, nullptr, false},
    {"WSARecvEx", 4, ws_WSARecvEx, nullptr, false},
    {"WSAEventSelect", 3, ws_WSAEventSelect, nullptr, true},
    {"WSAEnumNetworkEvents", 3, ws_WSAEnumNetworkEvents, nullptr, true},
    {"WSACreateEvent", 0, ws_WSACreateEvent, nullptr, true},
    {"WSACloseEvent", 1, ws_WSACloseEvent, nullptr, true},
    {"WSASetEvent", 1, ws_WSASetEvent, nullptr, true},
    {"WSAResetEvent", 1, ws_WSAResetEvent, nullptr, true},
    {"WSAWaitForMultipleEvents", 5, ws_WSAWaitForMultipleEvents, nullptr, true},
    {"WSAIoctl", 9, ws_WSAIoctl, nullptr, true},
    {"WSASocketA", 6, ws_WSASocket, nullptr, true},
    {"WSASocketW", 6, ws_WSASocket, nullptr, true},
    {"WSARecv", 7, ws_WSARecv, nullptr, true},
    {"WSARecvFrom", 9, ws_WSARecvFrom, nullptr, true},
    {"WSASend", 7, ws_WSASend, nullptr, true},
    {"WSASendTo", 9, ws_WSASendTo, nullptr, true},
    {"WSAHtons", 3, ws_WSAHtons, nullptr, true},
    {"WSANtohs", 3, ws_WSAHtons, nullptr, true},
    {"WSAHtonl", 3, ws_WSAHtonl, nullptr, true},
    {"WSANtohl", 3, ws_WSAHtonl, nullptr, true},
};
} // namespace

void winsock_register() {
    static std::deque<std::string> names; // ImportShim keeps the pointers
    static std::vector<ImportShim> table;
    if (table.empty()) {
        for (const char *dll : {"WSOCK32.dll", "WS2_32.dll"}) {
            const bool ws1 = strcmp(dll, "WSOCK32.dll") == 0;
            for (const Export &e : kExports) {
                if (ws1 && e.ws2_only)
                    continue;
                void (*fn)(X86 *) = ws1 && e.fn_ws1 ? e.fn_ws1 : e.fn;
                table.push_back({dll, e.name, e.argc, fn});
            }
            // The ordinals (1-23, 51-57, 101-116, 151 and WSOCK32's 1107).
            for (uint32_t ord = 1; ord <= 1107; ++ord) {
                const char *name = imports_ordinal_name(dll, ord);
                if (ws1 && ord == 1107)
                    name = "WSARecvEx";
                if (!name)
                    continue;
                for (const Export &e : kExports) {
                    if (strcmp(e.name, name) != 0 || (ws1 && e.ws2_only))
                        continue;
                    names.push_back("#" + std::to_string(ord));
                    table.push_back(
                        {dll, names.back().c_str(), e.argc, ws1 && e.fn_ws1 ? e.fn_ws1 : e.fn});
                }
            }
        }
        // Overlapped I/O is not modelled; its completion query keeps its
        // stack cleanup.
        table.push_back({"WS2_32.dll", "WSAGetOverlappedResult", 5, nullptr});
        mem_add_reset_hook(close_all_on_reset);
    }
    imports_register(table.data(), table.size());
}
