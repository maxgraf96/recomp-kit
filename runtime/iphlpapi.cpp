// iphlpapi.cpp - the IP helper API (IPHLPAPI) and NETAPI32's Netbios, from
// the adapters net_adapters.h chooses: the same list, in the same order, that
// Winsock answers a lookup of the own host name with.
//
// Every structure is the 32-bit guest's layout. Addresses inside them are in
// network order, as Windows stores an IN_ADDR in a DWORD.
#include "imports.h"
#include "memory.h"
#include "net_adapters.h"
#include "win32.h"
#include "winsock.h"
#include "../platform/os.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>

namespace {

const uint32_t NO_ERROR_ = 0, ERROR_INVALID_PARAMETER_ = 87, ERROR_BUFFER_OVERFLOW_ = 111,
               ERROR_INSUFFICIENT_BUFFER_ = 122, ERROR_NO_DATA_ = 232,
               ERROR_CAN_NOT_COMPLETE_ = 1003;
const uint32_t LOOPBACK_INDEX = 1; // the loopback interface's index on Windows
const uint16_t AF_INET_ = 2;

void put_ip(uint32_t p, uint32_t ip) {
    wr8(p, (uint8_t)(ip >> 24));
    wr8(p + 1, (uint8_t)(ip >> 16));
    wr8(p + 2, (uint8_t)(ip >> 8));
    wr8(p + 3, (uint8_t)ip);
}
uint32_t swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24);
}
uint32_t prefix_bits(uint32_t mask) {
    uint32_t n = 0;
    while (n < 32 && (mask & (0x80000000u >> n)))
        ++n;
    return n;
}

// A stable registry-style GUID for an interface name, as Windows names an
// adapter: "{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}".
std::string adapter_guid(const std::string &name) {
    uint64_t h1 = 1469598103934665603ull, h2 = 0x9e3779b97f4a7c15ull;
    for (unsigned char ch : name) {
        h1 = (h1 ^ ch) * 1099511628211ull;
        h2 = (h2 ^ ch) * 0x100000001b3ull + 0x7f4a7c15ull;
    }
    char b[40];
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%04X-%012llX}", (unsigned)(h1 >> 32),
             (unsigned)(h1 >> 16) & 0xffff, (unsigned)h1 & 0xffff, (unsigned)(h2 >> 48),
             (unsigned long long)(h2 & 0xffffffffffffull));
    return b;
}

std::string adapter_description(const NetAdapter &a) {
    const char *what = a.kind == NET_KIND_LAN       ? "Ethernet or Wi-Fi"
                       : a.kind == NET_KIND_OVERLAY ? "ZeroTier virtual network"
                       : a.kind == NET_KIND_TUNNEL  ? "VPN tunnel"
                                                    : "Network interface";
    return std::string(what) + " (" + a.name + ")";
}

uint32_t adapter_type(const NetAdapter &a) {
    return a.point_to_point || a.kind == NET_KIND_TUNNEL ? 23u /* PPP */ : 6u /* Ethernet */;
}

// IP_ADDR_STRING {Next, IpAddress[16], IpMask[16], Context}.
void put_addr_string(uint32_t p, uint32_t next, uint32_t ip, uint32_t mask, uint32_t context) {
    wr32(p, next);
    gm_put_str(p + 4, net_dotted(ip).c_str(), 16);
    gm_put_str(p + 20, net_dotted(mask).c_str(), 16);
    wr32(p + 36, context);
}

// GetAdaptersInfo(pAdapterInfo, pOutBufLen): IP_ADAPTER_INFO records (640
// bytes) chained by Next, then the IP_ADDR_STRINGs of adapters with more than
// one address.
void GetAdaptersInfo(X86 *c) {
    uint32_t out = arg(c, 0), lenp = arg(c, 1);
    if (!lenp || !gm_valid(lenp, 4)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    std::vector<NetAdapter> adapters = net_adapters();
    if (adapters.empty()) {
        set_eax(c, ERROR_NO_DATA_);
        return;
    }
    uint32_t extra = 0;
    for (const NetAdapter &a : adapters)
        extra += (uint32_t)a.addresses.size() - 1;
    uint32_t need =
        (uint32_t)adapters.size() * NET_ADAPTER_INFO_SIZE + extra * NET_ADDR_STRING_SIZE;
    if (!out || rd32(lenp) < need || !gm_valid(out, need)) {
        wr32(lenp, need);
        set_eax(c, ERROR_BUFFER_OVERFLOW_);
        return;
    }
    memset(g_mem + out, 0, need);
    uint32_t tail = out + (uint32_t)adapters.size() * NET_ADAPTER_INFO_SIZE;
    for (size_t i = 0; i < adapters.size(); ++i) {
        const NetAdapter &a = adapters[i];
        uint32_t r = out + (uint32_t)i * NET_ADAPTER_INFO_SIZE;
        wr32(r + 0, i + 1 < adapters.size() ? r + NET_ADAPTER_INFO_SIZE : 0);
        wr32(r + 4, a.index); // ComboIndex
        gm_put_str(r + 8, adapter_guid(a.name).c_str(), 260);
        gm_put_str(r + 268, adapter_description(a).c_str(), 132);
        wr32(r + 400, (uint32_t)a.mac_len);
        memcpy(g_mem + r + 404, a.mac, 8);
        wr32(r + 412, a.index);
        wr32(r + 416, adapter_type(a));
        wr32(r + 420, 0);       // DhcpEnabled: not known here
        wr32(r + 424, r + 428); // CurrentIpAddress
        // IpAddressList: the first address in place, the rest in the tail.
        uint32_t slot = r + 428;
        for (size_t k = 0; k < a.addresses.size(); ++k) {
            uint32_t next = k + 1 < a.addresses.size() ? tail : 0;
            put_addr_string(slot, next, a.addresses[k].ip, a.addresses[k].mask,
                            a.index * 16 + (uint32_t)k);
            if (next) {
                slot = tail;
                tail += NET_ADDR_STRING_SIZE;
            }
        }
        put_addr_string(r + 468, 0, a.gateway, a.gateway ? 0xffffffffu : 0, 0);
        put_addr_string(r + 508, 0, 0, 0, 0); // DhcpServer
        put_addr_string(r + 552, 0, 0, 0, 0); // PrimaryWinsServer
        put_addr_string(r + 592, 0, 0, 0, 0); // SecondaryWinsServer
    }
    set_eax(c, NO_ERROR_);
}

// GetAdaptersAddresses(Family, Flags, Reserved, AdapterAddresses,
// SizePointer), IPv4 only. Each IP_ADAPTER_ADDRESSES has the XP layout
// (Length 144) in a zeroed 384-byte record, so a program built for a later
// SDK that reads past Length finds zeros; IP_ADAPTER_UNICAST_ADDRESS is 48
// bytes with OnLinkPrefixLength.
const uint32_t GAA_RECORD = 384, GAA_UNICAST = 48;
void GetAdaptersAddresses(X86 *c) {
    uint32_t family = arg(c, 0), flags = arg(c, 1), out = arg(c, 3), lenp = arg(c, 4);
    if (!lenp || !gm_valid(lenp, 4) || (family != 0 && family != 2 && family != 23)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    std::vector<NetAdapter> adapters = net_adapters();
    if (family == 23 || adapters.empty()) {
        set_eax(c, ERROR_NO_DATA_);
        return;
    }
    const bool unicast = !(flags & 0x1); // GAA_FLAG_SKIP_UNICAST
    struct Strings {
        std::string guid, friendly, description;
    };
    std::vector<Strings> strings;
    uint32_t need = 0;
    for (const NetAdapter &a : adapters) {
        Strings s{adapter_guid(a.name), a.name, adapter_description(a)};
        need += GAA_RECORD;
        if (unicast)
            need += (uint32_t)a.addresses.size() * (GAA_UNICAST + 16);
        need += (uint32_t)((s.guid.size() + 1 + 3) & ~3u);
        need += (uint32_t)(2 * (s.friendly.size() + 1) + 3) & ~3u;
        need += (uint32_t)(2 * (s.description.size() + 1) + 3) & ~3u;
        need += 4; // the empty DnsSuffix
        strings.push_back(s);
    }
    if (!out || rd32(lenp) < need || !gm_valid(out, need)) {
        wr32(lenp, need);
        set_eax(c, ERROR_BUFFER_OVERFLOW_);
        return;
    }
    memset(g_mem + out, 0, need);
    // The records first, then everything they point at.
    uint32_t heap = out + (uint32_t)adapters.size() * GAA_RECORD;
    auto take = [&](uint32_t n) {
        uint32_t p = heap;
        heap += (n + 3) & ~3u;
        return p;
    };
    for (size_t i = 0; i < adapters.size(); ++i) {
        const NetAdapter &a = adapters[i];
        const Strings &s = strings[i];
        uint32_t r = out + (uint32_t)i * GAA_RECORD;
        wr32(r + 0, 144); // Length
        wr32(r + 4, a.index);
        wr32(r + 8, i + 1 < adapters.size() ? r + GAA_RECORD : 0);
        uint32_t name = take((uint32_t)s.guid.size() + 1);
        gm_put_str(name, s.guid.c_str(), (uint32_t)s.guid.size() + 1);
        wr32(r + 12, name);
        if (unicast) {
            uint32_t prev = r + 16; // FirstUnicastAddress
            for (const NetAddress &addr : a.addresses) {
                uint32_t u = take(GAA_UNICAST), sa = take(16);
                wr32(u + 0, GAA_UNICAST);
                wr32(u + 4, 0);
                wr32(prev, u);
                prev = u + 8; // Next
                wr32(u + 12, sa);
                wr32(u + 16, 16);
                wr32(u + 20, 3); // PrefixOrigin: DHCP
                wr32(u + 24, 3); // SuffixOrigin: DHCP
                wr32(u + 28, 4); // DadState: preferred
                wr32(u + 32, 0xffffffffu);
                wr32(u + 36, 0xffffffffu);
                wr32(u + 40, 0xffffffffu);
                wr8(u + 44, (uint8_t)prefix_bits(addr.mask)); // OnLinkPrefixLength
                wr16(sa, AF_INET_);
                put_ip(sa + 4, addr.ip);
            }
        }
        uint32_t suffix = take(4);
        wr32(r + 32, suffix); // DnsSuffix: L""
        uint32_t desc = take((uint32_t)(2 * (s.description.size() + 1)));
        gm_put_wstr(desc, s.description, (uint32_t)s.description.size() + 1);
        wr32(r + 36, desc);
        uint32_t friendly = take((uint32_t)(2 * (s.friendly.size() + 1)));
        gm_put_wstr(friendly, s.friendly, (uint32_t)s.friendly.size() + 1);
        wr32(r + 40, friendly);
        memcpy(g_mem + r + 44, a.mac, 8);
        wr32(r + 52, (uint32_t)a.mac_len);
        wr32(r + 56, 0x80); // Flags: IP_ADAPTER_IPV4_ENABLED
        wr32(r + 60, a.mtu ? (uint32_t)a.mtu : 1500);
        wr32(r + 64, adapter_type(a));
        wr32(r + 68, 1); // OperStatus: up
    }
    set_eax(c, NO_ERROR_);
}

// MIB_IPFORWARDROW: dest, mask, policy, next hop, interface, type (3 direct, 4
// indirect), protocol (2 local, 3 network management), age, next hop AS,
// metrics 1-5; 56 bytes.
struct Route {
    uint32_t dest, mask, next, ifindex, type, proto, metric;
};
std::vector<Route> routes() {
    std::vector<Route> v;
    std::vector<NetAdapter> adapters = net_adapters();
    for (const NetAdapter &a : adapters) {
        uint32_t metric = 20 + 5 * (uint32_t)a.kind;
        if (a.default_route && a.gateway)
            v.push_back({0, 0, a.gateway, a.index, 4, 3, metric});
        for (const NetAddress &addr : a.addresses) {
            v.push_back({addr.ip & addr.mask, addr.mask, addr.ip, a.index, 3, 2, metric});
            v.push_back({addr.ip, 0xffffffffu, 0x7f000001u, LOOPBACK_INDEX, 3, 2, metric});
            if (a.can_broadcast && addr.mask != 0xffffffffu)
                v.push_back({addr.ip | ~addr.mask, 0xffffffffu, addr.ip, a.index, 3, 2, metric});
        }
        v.push_back({0xe0000000u, 0xf0000000u, a.addresses[0].ip, a.index, 3, 2, metric});
        v.push_back({0xffffffffu, 0xffffffffu, a.addresses[0].ip, a.index, 3, 2, 1});
    }
    v.push_back({0x7f000000u, 0xff000000u, 0x7f000001u, LOOPBACK_INDEX, 3, 2, 1});
    return v;
}
void put_route(uint32_t p, const Route &r) {
    memset(g_mem + p, 0, 56);
    put_ip(p + 0, r.dest);
    put_ip(p + 4, r.mask);
    put_ip(p + 12, r.next);
    wr32(p + 16, r.ifindex);
    wr32(p + 20, r.type);
    wr32(p + 24, r.proto);
    wr32(p + 36, r.metric);
    for (int k = 1; k < 5; ++k)
        wr32(p + 36 + 4 * (uint32_t)k, 0xffffffffu); // unused metrics are -1
}

// GetIpForwardTable(pIpForwardTable, pdwSize, bOrder).
void GetIpForwardTable(X86 *c) {
    uint32_t out = arg(c, 0), lenp = arg(c, 1), order = arg(c, 2);
    if (!lenp || !gm_valid(lenp, 4)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    std::vector<Route> v = routes();
    if (order)
        std::sort(v.begin(), v.end(), [](const Route &a, const Route &b) {
            if (a.dest != b.dest)
                return a.dest < b.dest;
            if (a.mask != b.mask)
                return a.mask < b.mask;
            return a.next < b.next;
        });
    uint32_t need = 4 + 56 * (uint32_t)v.size();
    if (!out || rd32(lenp) < need || !gm_valid(out, need)) {
        wr32(lenp, need);
        set_eax(c, ERROR_INSUFFICIENT_BUFFER_);
        return;
    }
    wr32(out, (uint32_t)v.size());
    for (size_t i = 0; i < v.size(); ++i)
        put_route(out + 4 + 56 * (uint32_t)i, v[i]);
    set_eax(c, NO_ERROR_);
}

// The route to `dest`: the longest matching prefix, the lowest metric among
// equals, preferring the interface the host's own routing table chooses.
bool best_route(uint32_t dest, Route *out) {
    uint32_t source = 0;
    unsigned preferred = 0;
    if (os_net_route_source(dest, &source) == 0)
        for (const NetAdapter &a : net_adapters())
            for (const NetAddress &addr : a.addresses)
                if (addr.ip == source)
                    preferred = a.index;
    bool found = false;
    for (const Route &r : routes()) {
        if ((dest & r.mask) != r.dest)
            continue;
        bool better = !found || r.mask > out->mask ||
                      (r.mask == out->mask && preferred && r.ifindex == preferred &&
                       out->ifindex != preferred) ||
                      (r.mask == out->mask && r.metric < out->metric &&
                       !(preferred && out->ifindex == preferred));
        if (better) {
            *out = r;
            found = true;
        }
    }
    return found;
}
// GetBestInterface(dwDestAddr, pdwBestIfIndex).
void GetBestInterface(X86 *c) {
    uint32_t dest = swap32(arg(c, 0)), out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    Route r;
    if (!best_route(dest, &r)) {
        set_eax(c, ERROR_CAN_NOT_COMPLETE_);
        return;
    }
    wr32(out, r.ifindex);
    set_eax(c, NO_ERROR_);
}
// GetBestRoute(dwDestAddr, dwSourceAddr, pBestRoute).
void GetBestRoute(X86 *c) {
    uint32_t dest = swap32(arg(c, 0)), out = arg(c, 2);
    if (!out || !gm_valid(out, 56)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    Route r;
    if (!best_route(dest, &r)) {
        set_eax(c, ERROR_CAN_NOT_COMPLETE_);
        return;
    }
    put_route(out, r);
    set_eax(c, NO_ERROR_);
}

// GetIpAddrTable(pIpAddrTable, pdwSize, bOrder): MIB_IPADDRROW {addr, index,
// mask, broadcast (1: all ones), reassembly size, unused, type}; 24 bytes.
void GetIpAddrTable(X86 *c) {
    uint32_t out = arg(c, 0), lenp = arg(c, 1), order = arg(c, 2);
    if (!lenp || !gm_valid(lenp, 4)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    struct Row {
        uint32_t ip, index, mask;
    };
    std::vector<Row> v;
    for (const NetAdapter &a : net_adapters())
        for (const NetAddress &addr : a.addresses)
            v.push_back({addr.ip, a.index, addr.mask});
    v.push_back({0x7f000001u, LOOPBACK_INDEX, 0xff000000u});
    if (order)
        std::sort(v.begin(), v.end(), [](const Row &a, const Row &b) { return a.ip < b.ip; });
    uint32_t need = 4 + 24 * (uint32_t)v.size();
    if (!out || rd32(lenp) < need || !gm_valid(out, need)) {
        wr32(lenp, need);
        set_eax(c, ERROR_INSUFFICIENT_BUFFER_);
        return;
    }
    wr32(out, (uint32_t)v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        uint32_t p = out + 4 + 24 * (uint32_t)i;
        put_ip(p, v[i].ip);
        wr32(p + 4, v[i].index);
        put_ip(p + 8, v[i].mask);
        wr32(p + 12, 1);
        wr32(p + 16, 65535);
        wr16(p + 20, 0);
        wr16(p + 22, 1); // MIB_IPADDR_PRIMARY
    }
    set_eax(c, NO_ERROR_);
}

// GetNumberOfInterfaces(pdwNumIf): the adapters and the loopback interface.
void GetNumberOfInterfaces(X86 *c) {
    uint32_t out = arg(c, 0);
    if (!out || !gm_valid(out, 4)) {
        set_eax(c, ERROR_INVALID_PARAMETER_);
        return;
    }
    wr32(out, (uint32_t)net_adapters().size() + 1);
    set_eax(c, NO_ERROR_);
}

// ---------------------------------------------------------------------------
// Netbios(pncb). NCB: command, retcode, lsn, num, buffer, length (u16),
// callname[16], name[16], rto, sto, post, lana_num, cmd_cplt, reserve[10],
// event; 64 bytes. Enough of NetBIOS for what programs of the period used it
// for - listing the LAN adapters (NCBENUM), resetting one (NCBRESET) and
// reading its hardware address (NCBASTAT) - with one LANA per adapter that has
// a hardware address, in adapter order.
// ---------------------------------------------------------------------------
const uint8_t NCBRESET = 0x32, NCBASTAT = 0x33, NCBENUM = 0x37, ASYNCH = 0x80;
const uint8_t NRC_GOODRET = 0x00, NRC_BUFLEN = 0x01, NRC_ILLCMD = 0x03, NRC_INCOMP = 0x06,
              NRC_BRIDGE = 0x23;

std::vector<NetAdapter> netbios_lanas() {
    std::vector<NetAdapter> v;
    for (const NetAdapter &a : net_adapters())
        if (a.mac_len == 6)
            v.push_back(a);
    return v;
}

uint8_t netbios(uint32_t ncb) {
    uint8_t cmd = rd8(ncb) & 0x7f;
    uint32_t buf = rd32(ncb + 4);
    uint16_t len = rd16(ncb + 8);
    uint8_t lana = rd8(ncb + 48);
    std::vector<NetAdapter> lanas = netbios_lanas();
    switch (cmd) {
    case NCBENUM: {
        // LANA_ENUM {length, lana[MAX_LANA + 1]}.
        if (!buf || !len || !gm_valid(buf, len))
            return NRC_BUFLEN;
        uint32_t n = std::min<uint32_t>((uint32_t)lanas.size(), len - 1u);
        wr8(buf, (uint8_t)n);
        for (uint32_t i = 0; i < n; ++i)
            wr8(buf + 1 + i, (uint8_t)i);
        return NRC_GOODRET;
    }
    case NCBRESET:
        return lana < lanas.size() ? NRC_GOODRET : NRC_BRIDGE;
    case NCBASTAT: {
        if (lana >= lanas.size())
            return NRC_BRIDGE;
        if (!buf || !len || !gm_valid(buf, len))
            return NRC_BUFLEN;
        // ADAPTER_STATUS (60 bytes) and no NAME_BUFFERs.
        uint8_t st[60];
        memset(st, 0, sizeof st);
        memcpy(st, lanas[lana].mac, 6);
        st[6] = 3;    // rev_major
        st[8] = 0xfe; // adapter_type: Ethernet
        uint16_t max_dgram = 1464, max_sess_pkt = 1460, sess = 64;
        memcpy(st + 48, &max_dgram, 2); // max_dgram_size
        memcpy(st + 52, &sess, 2);      // max_cfg_sess
        memcpy(st + 54, &sess, 2);      // max_sess
        memcpy(st + 56, &max_sess_pkt, 2);
        uint32_t n = std::min<uint32_t>(len, 60);
        memcpy(g_mem + buf, st, n);
        wr16(ncb + 8, (uint16_t)n);
        return n < 60 ? NRC_INCOMP : NRC_GOODRET;
    }
    default:
        return NRC_ILLCMD;
    }
}

void Netbios(X86 *c) {
    uint32_t ncb = arg(c, 0);
    if (!ncb || !gm_valid(ncb, 64)) {
        set_eax(c, NRC_BUFLEN);
        return;
    }
    const bool async = (rd8(ncb) & ASYNCH) != 0;
    uint8_t rc = netbios(ncb);
    wr8(ncb + 1, rc);  // ncb_retcode
    wr8(ncb + 49, rc); // ncb_cmd_cplt
    if (async) {
        // Completed at once; the completion is reported the way it was asked for.
        if (uint32_t ev = rd32(ncb + 60))
            win32_signal_event(ev, false);
        if (uint32_t post = rd32(ncb + 44))
            guest_call(c, post, ncb);
        set_eax(c, NRC_GOODRET);
        return;
    }
    set_eax(c, rc);
}

const ImportShim kShims[] = {
    {"IPHLPAPI.DLL", "GetAdaptersInfo", 2, GetAdaptersInfo},
    {"IPHLPAPI.DLL", "GetAdaptersAddresses", 5, GetAdaptersAddresses},
    {"IPHLPAPI.DLL", "GetIpForwardTable", 3, GetIpForwardTable},
    {"IPHLPAPI.DLL", "GetBestInterface", 2, GetBestInterface},
    {"IPHLPAPI.DLL", "GetBestRoute", 3, GetBestRoute},
    {"IPHLPAPI.DLL", "GetIpAddrTable", 3, GetIpAddrTable},
    {"IPHLPAPI.DLL", "GetNumberOfInterfaces", 1, GetNumberOfInterfaces},
    {"NETAPI32.dll", "Netbios", 1, Netbios},
};

} // namespace

void iphlpapi_register() {
    imports_register(kShims, sizeof kShims / sizeof kShims[0]);
}
