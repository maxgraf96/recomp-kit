// winsock.h - the runtime's Winsock (winsock.cpp) and IP helper API
// (iphlpapi.cpp), as the import registry and the tests see them.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Registers WSOCK32 and WS2_32 (winsock.cpp), IPHLPAPI and NETAPI32's Netbios
// (iphlpapi.cpp). Called by imports_init().
void winsock_register();
void iphlpapi_register();

// The Winsock error (WSAE*) for a POSIX errno.
uint32_t winsock_error_from_errno(int e);

// Replaces the system resolver behind gethostbyname for names that are not
// answered locally; null restores it. The function has os_net_resolve's
// contract and runs on a host thread of its own.
typedef int (*WinsockResolver)(const char *name, uint32_t *ips, int cap, char *canonical,
                               size_t canonical_cap);
void winsock_set_resolver_for_test(WinsockResolver fn);
// How long a guest lookup waits for the resolver before reporting
// WSATRY_AGAIN; the default is 10 seconds.
void winsock_set_resolve_timeout_for_test(uint32_t ms);
// Starts the capture RECOMP_NET_PCAP starts at WSAStartup, into `path`
// (closing any capture open); null or "" stops it.
void winsock_set_pcap_for_test(const char *path);
// Sockets the guest has open, for leak checks.
size_t winsock_open_sockets();
