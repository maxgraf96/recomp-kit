// net_adapters.h - the network adapters a guest is shown.
//
// Winsock (the own host's addresses, broadcast fan-out) and the IP helper API
// (GetAdaptersInfo and friends) answer from this one list, so every view a
// program has of "this machine's addresses" agrees, in the same order: a
// program that finds its address in gethostbyname(gethostname()) by position
// in GetAdaptersInfo's list finds the same one.
//
// The list is the host's IPv4 interfaces, filtered and ordered for a LAN game
// of the Windows 98-XP era (README.md, "Networking"):
//   - loopback, down interfaces and interfaces without IPv4 are left out;
//   - the local network first (en*, eth*, wl*), the one the default route
//     uses ahead of the rest; then overlay networks (ZeroTier's feth* and
//     zt*), then tunnels (utun*, tun*, tap*, wg*, ppp*, ipsec*), then
//     anything else; within a group by interface index;
//   - at most NET_MAX_ADAPTERS, and no more than fit a 6,400-byte
//     GetAdaptersInfo buffer (ten IP_ADAPTER_INFO records), which programs
//     of the period allocated as a fixed size.
// RECOMP_NET_ADAPTERS=name,name,... replaces the choice: those interfaces, in
// that order, whatever their kind.
#pragma once
#include "../platform/os.h"

#include <stdint.h>
#include <string>
#include <vector>

enum NetAdapterKind {
    NET_KIND_LAN = 0,
    NET_KIND_OVERLAY = 1, // a virtual Ethernet that carries broadcasts (ZeroTier)
    NET_KIND_TUNNEL = 2,  // a point-to-point VPN tunnel
    NET_KIND_OTHER = 3
};

struct NetAddress {
    uint32_t ip = 0, mask = 0; // host byte order
};

struct NetAdapter {
    std::string name;   // the host's interface name
    unsigned index = 0; // the host's interface index
    NetAdapterKind kind = NET_KIND_OTHER;
    std::vector<NetAddress> addresses; // IPv4, in the host's order; never empty
    uint32_t broadcast = 0;            // the first address's directed broadcast, 0 for none
    uint32_t peer = 0;                 // a point-to-point link's far end
    bool can_broadcast = false, point_to_point = false;
    uint8_t mac[8] = {};
    int mac_len = 0;
    int mtu = 0;
    bool default_route = false; // the IPv4 default route leaves through it
    uint32_t gateway = 0;       // that route's gateway, when it does
};

const int NET_MAX_ADAPTERS = 10;
// IP_ADAPTER_INFO on a 32-bit guest, an IP_ADDR_STRING, and the buffer the
// list must fit.
const uint32_t NET_ADAPTER_INFO_SIZE = 640, NET_ADDR_STRING_SIZE = 40;
const uint32_t NET_ADAPTER_INFO_BUDGET = 6400;

// The adapters a list of interfaces gives: filtered, grouped by interface,
// ordered and limited as above. Pure, for the tests. `only` is a comma
// separated list of interface names that replaces the ordering, or null.
std::vector<NetAdapter> net_adapters_from(const OsNetInterface *ifs, int n,
                                          unsigned default_ifindex, uint32_t default_gateway,
                                          const char *only);
// The adapters now: the host's, re-read when the last reading is more than two
// seconds old, or the list a test installed.
std::vector<NetAdapter> net_adapters();
// Every address of every adapter, in adapter order: what a lookup of the own
// host name answers.
std::vector<uint32_t> net_local_addresses();
// Replaces the host's interfaces with a fixed list until cleared (n < 0).
void net_set_interfaces_for_test(const OsNetInterface *ifs, int n, unsigned default_ifindex,
                                 uint32_t default_gateway);
// "a.b.c.d" for an address in host byte order.
std::string net_dotted(uint32_t ip);
