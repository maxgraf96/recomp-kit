// net_adapters.cpp - the network adapters a guest is shown (net_adapters.h).
#include "net_adapters.h"

#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <mutex>

namespace {

bool starts_with(const std::string &s, const char *prefix) {
    return s.compare(0, strlen(prefix), prefix) == 0;
}

NetAdapterKind kind_of(const std::string &name) {
    // ZeroTier first: its macOS interfaces are feth*, which no other prefix
    // below claims, and zt* elsewhere.
    if (starts_with(name, "feth") || starts_with(name, "zt"))
        return NET_KIND_OVERLAY;
    if (starts_with(name, "en") || starts_with(name, "eth") || starts_with(name, "wl"))
        return NET_KIND_LAN;
    for (const char *p : {"utun", "tun", "tap", "wg", "ppp", "ipsec", "tailscale"})
        if (starts_with(name, p))
            return NET_KIND_TUNNEL;
    return NET_KIND_OTHER;
}

std::vector<std::string> split_names(const char *only) {
    std::vector<std::string> out;
    std::string cur;
    for (const char *p = only;; ++p) {
        if (!*p || *p == ',' || *p == ' ') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
            if (!*p)
                break;
        } else {
            cur += *p;
        }
    }
    return out;
}

struct Cache {
    std::mutex m;
    bool test = false;
    std::vector<OsNetInterface> test_ifs;
    unsigned test_default = 0;
    uint32_t test_gateway = 0;
    std::vector<NetAdapter> adapters;
    uint64_t read_at = 0; // os_monotonic_ns of the last host reading, 0 for none
};
Cache &cache() {
    static Cache *c = new Cache(); // read by guest threads to the very end
    return *c;
}

} // namespace

std::string net_dotted(uint32_t ip) {
    char b[16];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    return b;
}

std::vector<NetAdapter> net_adapters_from(const OsNetInterface *ifs, int n,
                                          unsigned default_ifindex, uint32_t default_gateway,
                                          const char *only) {
    std::vector<NetAdapter> all;
    for (int i = 0; i < n; ++i) {
        const OsNetInterface &f = ifs[i];
        if (f.loopback || !f.up || !f.running || !f.ip)
            continue;
        NetAdapter *a = nullptr;
        for (NetAdapter &e : all)
            if (e.name == f.name)
                a = &e;
        if (!a) {
            all.emplace_back();
            a = &all.back();
            a->name = f.name;
            a->index = f.index;
            a->kind = kind_of(a->name);
            a->can_broadcast = f.can_broadcast != 0;
            a->point_to_point = f.point_to_point != 0;
            a->broadcast = f.broadcast;
            a->peer = f.peer;
            memcpy(a->mac, f.mac, sizeof a->mac);
            a->mac_len = f.mac_len;
            a->mtu = f.mtu;
            a->default_route = default_ifindex && f.index == default_ifindex;
            a->gateway = a->default_route ? default_gateway : 0;
        }
        NetAddress addr;
        addr.ip = f.ip;
        addr.mask = f.netmask;
        a->addresses.push_back(addr);
    }
    std::vector<NetAdapter> ordered;
    if (only && *only) {
        for (const std::string &name : split_names(only))
            for (const NetAdapter &a : all)
                if (a.name == name)
                    ordered.push_back(a);
    } else {
        ordered = all;
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const NetAdapter &a, const NetAdapter &b) {
                             if (a.kind != b.kind)
                                 return a.kind < b.kind;
                             if (a.default_route != b.default_route)
                                 return a.default_route;
                             return a.index < b.index;
                         });
    }
    std::vector<NetAdapter> out;
    uint32_t size = 0;
    for (const NetAdapter &a : ordered) {
        uint32_t need =
            NET_ADAPTER_INFO_SIZE + NET_ADDR_STRING_SIZE * (uint32_t)(a.addresses.size() - 1);
        if ((int)out.size() >= NET_MAX_ADAPTERS || size + need > NET_ADAPTER_INFO_BUDGET)
            break;
        size += need;
        out.push_back(a);
    }
    return out;
}

std::vector<NetAdapter> net_adapters() {
    Cache &c = cache();
    std::lock_guard<std::mutex> lock(c.m);
    const char *only = recomp_env("NET_ADAPTERS");
    if (c.test)
        return net_adapters_from(c.test_ifs.data(), (int)c.test_ifs.size(), c.test_default,
                                 c.test_gateway, only);
    uint64_t now = os_monotonic_ns();
    if (c.read_at && now - c.read_at < 2000000000ull)
        return c.adapters;
    std::vector<OsNetInterface> ifs(64);
    int n = os_net_interfaces(ifs.data(), (int)ifs.size());
    if (n > (int)ifs.size()) {
        ifs.resize((size_t)n);
        n = os_net_interfaces(ifs.data(), (int)ifs.size());
    }
    if (n < 0)
        n = 0;
    uint32_t gateway = 0;
    unsigned def = 0;
    if (os_net_default_route(&gateway, &def) != 0)
        gateway = 0, def = 0;
    c.adapters = net_adapters_from(ifs.data(), std::min(n, (int)ifs.size()), def, gateway, only);
    c.read_at = now;
    return c.adapters;
}

std::vector<uint32_t> net_local_addresses() {
    std::vector<uint32_t> out;
    for (const NetAdapter &a : net_adapters())
        for (const NetAddress &addr : a.addresses)
            out.push_back(addr.ip);
    return out;
}

void net_set_interfaces_for_test(const OsNetInterface *ifs, int n, unsigned default_ifindex,
                                 uint32_t default_gateway) {
    Cache &c = cache();
    std::lock_guard<std::mutex> lock(c.m);
    c.test = n >= 0;
    c.test_ifs.assign(ifs, ifs + (n > 0 ? n : 0));
    c.test_default = default_ifindex;
    c.test_gateway = default_gateway;
    c.read_at = 0;
}
