// os.h - the platform layer: what the runtime, the DirectX shims, the mod
// foundation and the shared host code need from the operating system. One
// POSIX implementation (macOS, Linux) and one Win32 implementation; nothing
// above this header includes a platform header, nothing here knows the guest.
//
// Every function is plain C with C linkage so C plugins and C++ hosts share
// it. Errors are reported the C way: -1 or NULL, errno where POSIX sets it.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Threads. A created thread is an opaque handle; identity is a 64-bit id that
// compares with ==, because the scheduler asks "is the caller this thread"
// far more often than it joins anything.
// ---------------------------------------------------------------------------
typedef struct OsThread OsThread;
typedef uint64_t OsThreadId;
// `stack_bytes` 0 means the platform default. NULL when the thread could not
// be created; nothing has started in that case.
OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes);
void os_thread_join(OsThread *t);   // waits, then frees the handle
void os_thread_detach(OsThread *t); // frees the handle; the thread runs on
void os_thread_exit(void);          // ends the calling thread; never returns
// Asks the scheduler to keep the calling thread on the fastest cores: the
// guest's threads draw every frame. A no-op where there is no such control.
void os_thread_prefer_performance(void);
OsThreadId os_thread_self(void);
OsThreadId os_thread_id_of(const OsThread *t);

// ---------------------------------------------------------------------------
// Virtual memory: a zero-filled, readable, writable, page-aligned region.
// ---------------------------------------------------------------------------
void *os_vm_reserve(size_t bytes); // NULL on failure
void os_vm_release(void *p, size_t bytes);
// Address space nothing may touch yet: every page faults until os_vm_commit
// opens it. Backs no memory, so a reservation far larger than what is used
// costs nothing. NULL when the platform cannot reserve that much (a 32-bit or
// WebAssembly host, a device with a small address space). Released with
// os_vm_release.
void *os_vm_reserve_guarded(size_t bytes);
// Makes [p, p+bytes) of a guarded reservation readable and writable, zero
// filled on first touch. Page aligned. 0 or -1.
int os_vm_commit(void *p, size_t bytes);
// Makes [p, p+bytes) fault again on every access; the contents are kept. 0 or -1.
int os_vm_protect_none(void *p, size_t bytes);
// The host's page size: 16 KB on Apple silicon, 4 KB on most others.
size_t os_vm_page_size(void);
// Maps `bytes` of the open file `fd` from `offset` over the committed memory at
// `p`, privately: reads come from the file and are paged in as touched, a write
// changes only this process's copy (and faults when `writable` is 0). `p`,
// `bytes` and `offset` are multiples of os_vm_page_size(). 0, or -1 when the
// host cannot map files there, in which case the caller reads the bytes in.
int os_vm_map_file(void *p, size_t bytes, int fd, uint64_t offset, int writable);
// Zeroed read-write memory at `p` again, replacing a file mapping. 0 or -1.
int os_vm_reset(void *p, size_t bytes);

// ---------------------------------------------------------------------------
// Plugins.
// ---------------------------------------------------------------------------
void *os_dlopen(const char *path);
void *os_dlopen_noload(const char *path); // a handle only if already loaded
void *os_dlsym(void *handle, const char *name);
int os_dlclose(void *handle);          // 0 on success
const char *os_dlerror(void);          // text for the last failure
const char *os_plugin_extension(void); // ".dylib", ".so" or ".dll"

// ---------------------------------------------------------------------------
// Paths. UTF-8 everywhere; the Win32 implementation converts.
// ---------------------------------------------------------------------------
typedef struct OsStat {
    uint64_t size;
    int64_t atime, mtime, ctime; // seconds since the epoch
    int is_dir, is_regular, is_symlink, is_readonly;
} OsStat;
int os_stat(const char *path, OsStat *out);      // follows symlinks; 0 or -1
int os_lstat(const char *path, OsStat *out);     // does not follow
int os_mkdir(const char *path);                  // 0, or -1 (an existing directory is -1, as mkdir)
int os_rename(const char *from, const char *to); // replaces an existing destination file
int os_unlink(const char *path);
int os_rmdir(const char *path);       // the directory must be empty
int os_getcwd(char *buf, size_t cap); // 0 or -1
int os_chdir(const char *path);
// Calls `fn` for every entry except "." and "..", in directory order. A
// nonzero return from `fn` stops the walk. -1 when the directory cannot be
// opened, 0 otherwise.
typedef int (*OsListDirFn)(const char *name, void *user);
int os_listdir(const char *dir, OsListDirFn fn, void *user);
// The trailing XXXXXX of `template_path` is replaced in place; returns an open
// read-write descriptor for the new file, or -1.
int os_mkstemp(char *template_path);
// mkdtemp: replaces the trailing XXXXXX and creates the directory; 0 or -1.
int os_mkdtemp(char *template_path);
// A writable temporary directory without a trailing separator ("/tmp", %TEMP%).
const char *os_temp_dir(void);
// "/dev/null" or "NUL".
const char *os_null_device(void);
// Bytes a new file may still take on the volume holding `path` (an existing
// file or directory). 0 or -1.
int os_free_space(const char *path, uint64_t *bytes_out);
// Sets a file's modification (and access) time, in seconds since the epoch.
int os_set_mtime(const char *path, int64_t mtime);
// A string value from the Windows registry. `key` starts with "HKLM\" or
// "HKCU\" and is read from the 64- and then the 32-bit view. -1 elsewhere.
int os_registry_read(const char *key, const char *value, char *buf, size_t cap);
// Per-user data directory for `app` (not created): ~/Library/Application Support/<app>,
// %APPDATA%\<app>, $XDG_DATA_HOME/<app> or ~/.local/share/<app>. 0 or -1.
int os_user_data_dir(const char *app, char *buf, size_t cap);
// The directories the platform keeps its installed fonts in, most specific
// first, as one string with the entries separated by '\n' (not created, not
// checked). macOS: ~/Library/Fonts, /Library/Fonts,
// /System/Library/Fonts/Supplemental, /System/Library/Fonts; Windows:
// %WINDIR%\Fonts; elsewhere ~/.fonts, ~/.local/share/fonts and
// /usr/share/fonts (searched recursively by the caller). 0 or -1.
int os_font_dirs(char *buf, size_t cap);
// Runs argv[0] with argv (NULL-terminated), inheriting stdio; 0 and a pid, or -1.
int os_spawn(const char *const argv[], int64_t *pid_out);
// Waits for the child. exit_code receives the exit status, or 128 + signal on
// POSIX when it died by a signal (so an abort reads as 134 everywhere); 0 or -1.
int os_wait(int64_t pid, int *exit_code);

// ---------------------------------------------------------------------------
// Descriptors. Binary mode always; created files are mode 0644.
// ---------------------------------------------------------------------------
enum {
    OS_O_RDONLY = 0,
    OS_O_WRONLY = 1,
    OS_O_RDWR = 2,
    OS_O_CREAT = 0x40,
    OS_O_EXCL = 0x80,
    OS_O_TRUNC = 0x200
};
enum { OS_SEEK_SET = 0, OS_SEEK_CUR = 1, OS_SEEK_END = 2 };
int os_fd_open(const char *path, int flags);
int64_t os_fd_read(int fd, void *buf, size_t n);        // bytes read, 0 at EOF, -1 on error
int64_t os_fd_write(int fd, const void *buf, size_t n); // bytes written, -1 on error
int64_t os_fd_seek(int fd, int64_t off, int whence);    // new offset or -1
int os_fd_close(int fd);
int os_fd_dup(int fd);
int os_fd_fsync(int fd);
int os_fd_truncate(int fd, int64_t length);
int os_fd_stat(int fd, OsStat *out);
// A FILE over an open descriptor, which then owns it.
void *os_fdopen(int fd, const char *mode);

// ---------------------------------------------------------------------------
// Process.
// ---------------------------------------------------------------------------
int os_exe_path(char *buf, size_t cap); // 0 or -1; NUL-terminated
// Report a fatal signal inside guest code. `what` is "SIGSEGV", "SIGBUS" or
// "an abort from the runtime". Returns 1 when handlers were installed and 0
// where the platform has no equivalent yet.
typedef void (*OsFaultFn)(const char *what);
int os_install_fault_handlers(OsFaultFn fn);
// An access fault, offered first to a hook that knows the faulting address.
// The hook returns nonzero when the fault was its own: it has reported it and
// must then end the process (os_exit_immediately), because returning would
// retry the access - unless it moved the thread on (os_fault_continue_in). Zero passes the fault on to the os_install_fault_handlers
// report, or to whatever handled the signal before. One hook; installing
// another replaces it. Returns 1 when installed, 0 where unsupported.
typedef int (*OsFaultAddressFn)(const char *what, uintptr_t address);
int os_install_fault_address_hook(OsFaultAddressFn fn);
// Only from inside that hook: instead of ending the process, continue the
// faulting thread in fn(arg) once the fault handler returns - on the thread's
// own stack and outside the handler, as if the faulting instruction had
// called fn. fn must not return. The hook then returns nonzero. Returns 1
// when arranged, 0 where the platform cannot (the hook must end the process).
int os_fault_continue_in(void (*fn)(uintptr_t), uintptr_t arg);
// Only from inside that hook: 1 when the faulting access was a write, 0 a
// read, -1 when the platform does not say.
int os_fault_was_write(void);
// For use inside a fault handler: a raw write to the error stream and an
// immediate process exit, neither of which touches stdio or runs destructors.
void os_write_stderr_raw(const char *s, size_t n);
void os_exit_immediately(int code);

// ---------------------------------------------------------------------------
// Time.
// ---------------------------------------------------------------------------
uint64_t os_monotonic_ns(void); // never goes backwards; arbitrary origin
// Broken-down local and UTC time for a Unix timestamp; 0 or -1.
int os_localtime(int64_t seconds, struct tm *out);
int os_gmtime(int64_t seconds, struct tm *out);
uint64_t os_wall_time_us(void); // microseconds since the Unix epoch
void os_sleep_us(uint64_t us);

// ---------------------------------------------------------------------------
// Strings.
// ---------------------------------------------------------------------------
int os_strcasecmp(const char *a, const char *b);

// ---------------------------------------------------------------------------
// Networking: IPv4 sockets, the host's interfaces and name lookup - what a
// guest's sockets library needs from the host, and nothing shaped like any
// one of them. Addresses and ports are in host byte order (a.b.c.d is
// a<<24 | b<<16 | c<<8 | d). A socket is a descriptor, created non-blocking
// and close-on-exec, with SIGPIPE suppressed: waiting belongs to the caller
// (os_net_poll). Failures are -1 with errno set to the POSIX code
// (EWOULDBLOCK, EINPROGRESS, ECONNREFUSED, ...). os_net_supported() is 0 where
// the layer has no sockets; every other call then fails with ENOSYS.
// ---------------------------------------------------------------------------
int os_net_supported(void);
enum { OS_NET_STREAM = 1, OS_NET_DGRAM = 2 };
int os_net_socket(int type);
int os_net_close(int s);
int os_net_bind(int s, uint32_t ip, uint16_t port);
int os_net_listen(int s, int backlog);
// The connection's own descriptor, non-blocking like every other; the peer's
// address through ip/port when they are not NULL.
int os_net_accept(int s, uint32_t *ip, uint16_t *port);
// -1 with EINPROGRESS while a stream connects: poll for OS_NET_POLL_OUT and
// read the outcome from os_net_pending_error.
int os_net_connect(int s, uint32_t ip, uint16_t port);
int os_net_local_address(int s, uint32_t *ip, uint16_t *port);
int os_net_peer_address(int s, uint32_t *ip, uint16_t *port);
int os_net_shutdown(int s, int how); // 0 receiving, 1 sending, 2 both
enum { OS_NET_MSG_OOB = 1, OS_NET_MSG_PEEK = 2, OS_NET_MSG_DONTROUTE = 4 };
int64_t os_net_send(int s, const void *buf, size_t n, int flags);
int64_t os_net_recv(int s, void *buf, size_t n, int flags);
// A datagram to ip:port. A nonzero `ifindex` sends it out of that interface
// whatever the routing table prefers (a broadcast to 255.255.255.255 leaves
// through the default route's interface otherwise); -1 with ENOTSUP where the
// platform cannot.
int64_t os_net_sendto(int s, const void *buf, size_t n, int flags, uint32_t ip, uint16_t port,
                      unsigned ifindex);
// What os_net_recvfrom learnt about a datagram besides its bytes. to_ip and
// ifindex are known (nonzero) only on a socket with OS_NET_OPT_RECV_DESTINATION.
typedef struct OsNetDatagram {
    uint32_t from_ip;
    uint16_t from_port;
    uint32_t to_ip;   // the address it was sent to, a broadcast address included
    unsigned ifindex; // the interface it arrived on
    int truncated;    // longer than the buffer: the rest is lost
} OsNetDatagram;
int64_t os_net_recvfrom(int s, void *buf, size_t n, int flags, OsNetDatagram *info);
int os_net_readable(int s, uint32_t *bytes); // bytes queued to read (FIONREAD); 0 or -1
// The socket's pending error (SO_ERROR), cleared by the read: 0, or an errno.
int os_net_pending_error(int s);
enum {
    OS_NET_OPT_BROADCAST = 1,
    OS_NET_OPT_REUSEADDR,
    OS_NET_OPT_REUSEPORT,
    OS_NET_OPT_KEEPALIVE,
    OS_NET_OPT_DONTROUTE,
    OS_NET_OPT_OOBINLINE,
    OS_NET_OPT_RCVBUF,
    OS_NET_OPT_SNDBUF,
    OS_NET_OPT_LINGER, // -1 off, otherwise the seconds a close may linger
    OS_NET_OPT_NODELAY,
    OS_NET_OPT_TTL,
    OS_NET_OPT_TOS,
    OS_NET_OPT_MULTICAST_TTL,
    OS_NET_OPT_MULTICAST_LOOP,
    OS_NET_OPT_MULTICAST_IF,     // an interface address
    OS_NET_OPT_RECV_DESTINATION, // fill OsNetDatagram's to_ip and ifindex
    OS_NET_OPT_TYPE,             // read only: OS_NET_STREAM or OS_NET_DGRAM
    OS_NET_OPT_ACCEPTCONN        // read only: listening
};
int os_net_set_option(int s, int option, int64_t value);
int os_net_get_option(int s, int option, int64_t *value);
// Joins (join 1) or leaves a multicast group on the interface with address
// `iface` (0: the system's choice).
int os_net_membership(int s, int join, uint32_t group, uint32_t iface);
enum {
    OS_NET_POLL_IN = 1,
    OS_NET_POLL_OUT = 2,
    OS_NET_POLL_PRI = 4,
    OS_NET_POLL_ERR = 8,
    OS_NET_POLL_HUP = 16
};
typedef struct OsNetPoll {
    int fd;
    int want; // OS_NET_POLL_IN | OUT | PRI; ERR and HUP are always reported
    int got;
} OsNetPoll;
// How many items have something in `got`; 0 when the timeout (milliseconds,
// -1 for none) passed first; -1 on failure.
int os_net_poll(OsNetPoll *items, size_t n, int timeout_ms);
// A wake-up channel for a thread parked in os_net_poll: fds[0] polls
// OS_NET_POLL_IN once os_net_wakeup_signal(fds[1]) has written to it. Both
// ends are non-blocking. 0 or -1.
int os_net_wakeup_pair(int fds[2]);
void os_net_wakeup_signal(int write_fd);
void os_net_wakeup_drain(int read_fd);

// One IPv4 address of one interface. An interface with several addresses
// appears once per address, in the system's order; one without IPv4 does not
// appear at all.
typedef struct OsNetInterface {
    char name[32];
    unsigned index;
    uint32_t ip, netmask;
    uint32_t broadcast; // the directed broadcast address, 0 when it has none
    uint32_t peer;      // the far end of a point-to-point link, 0 when none
    uint8_t mac[8];
    int mac_len; // 0 for an interface without a hardware address
    int mtu;
    int up, running, loopback, point_to_point, can_broadcast;
} OsNetInterface;
// Fills up to `cap` entries and returns how many there are (which may be more
// than `cap`), or -1.
int os_net_interfaces(OsNetInterface *out, int cap);
// The IPv4 default route's gateway and interface; -1 when there is none.
int os_net_default_route(uint32_t *gateway, unsigned *ifindex);
// The local address the routing table would send to `dest` from. Sends
// nothing. 0 or -1.
int os_net_route_source(uint32_t dest, uint32_t *source_ip);
// This host's name, NUL-terminated and truncated to `cap`. 0 or -1.
int os_net_hostname(char *buf, size_t cap);
// The IPv4 addresses `name` resolves to through the system resolver, which
// may block for as long as it likes: the count found (at most `cap` stored),
// the canonical name through `canonical`, or an OS_NET_RESOLVE_* failure.
enum {
    OS_NET_RESOLVE_NOT_FOUND = -1, // no such name
    OS_NET_RESOLVE_AGAIN = -2,     // a temporary failure: no answer yet
    OS_NET_RESOLVE_NO_DATA = -3,   // the name exists without an IPv4 address
    OS_NET_RESOLVE_FAIL = -4       // the resolver failed
};
int os_net_resolve(const char *name, uint32_t *ips, int cap, char *canonical, size_t canonical_cap);

// ---------------------------------------------------------------------------
// Environment. Values are copied; 0 or -1.
// ---------------------------------------------------------------------------
int os_setenv(const char *name, const char *value);
int os_unsetenv(const char *name);
// The kit's switches: `recomp_env("PIN_CLOCK")` reads RECOMP_PIN_CLOCK.  The
// prefix names the kit, never a game; a switch is unset when NULL.
const char *recomp_env(const char *name);
// Switches from a file, for a platform with no shell to set them in (the iPad
// app reads Documents/switches.txt at start). One NAME=VALUE per line, spaces
// around either trimmed; blank lines, # comments and lines without '=' are
// skipped. Returns how many were set; 0 when there is no such file.
int recomp_env_apply_file(const char *path);

#ifdef __cplusplus
}
#endif
