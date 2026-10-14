// os_posix.cpp - the platform layer on macOS and Linux. The only file above
// third_party/ that may include a POSIX or Mach header besides os_win32.cpp.
#include "os.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <string>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#ifdef __APPLE__
#include <net/if_dl.h>
#include <net/route.h>
#include <sys/sysctl.h>
#endif
#ifdef __linux__
#include <netpacket/packet.h>
#endif
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

struct OsThread {
    pthread_t handle;
};

extern "C" {

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!t)
        return nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    int rc = pthread_create(&t->handle, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return nullptr;
    }
    return t;
}
void os_thread_join(OsThread *t) {
    pthread_join(t->handle, nullptr);
    free(t);
}
void os_thread_detach(OsThread *t) {
    pthread_detach(t->handle);
    free(t);
}
void os_thread_prefer_performance(void) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
void os_thread_exit(void) {
    pthread_exit(nullptr);
}
OsThreadId os_thread_self(void) {
    return (OsThreadId)(uintptr_t)pthread_self();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return (OsThreadId)(uintptr_t)t->handle;
}

void *os_vm_reserve(size_t bytes) {
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void os_vm_release(void *p, size_t bytes) {
    munmap(p, bytes);
}
void *os_vm_reserve_guarded(size_t bytes) {
#ifdef __EMSCRIPTEN__
    (void)bytes; // a wasm32 heap cannot hold a reservation of its own size
    return nullptr;
#else
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
    void *p = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
int os_vm_commit(void *p, size_t bytes) {
    return mprotect(p, bytes, PROT_READ | PROT_WRITE) == 0 ? 0 : -1;
}
int os_vm_protect_none(void *p, size_t bytes) {
    return mprotect(p, bytes, PROT_NONE) == 0 ? 0 : -1;
}
size_t os_vm_page_size(void) {
    long n = sysconf(_SC_PAGESIZE);
    return n > 0 ? (size_t)n : 4096;
}
int os_vm_map_file(void *p, size_t bytes, int fd, uint64_t offset, int writable) {
#ifdef __EMSCRIPTEN__
    (void)p, (void)bytes, (void)fd, (void)offset, (void)writable;
    return -1; // no fixed file mappings inside the wasm heap
#else
    int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    void *q = mmap(p, bytes, prot, MAP_PRIVATE | MAP_FIXED, fd, (off_t)offset);
    return q == p ? 0 : -1;
#endif
}
int os_vm_reset(void *p, size_t bytes) {
    void *q = mmap(p, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return q == p ? 0 : -1;
}

void *os_dlopen(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void *os_dlopen_noload(const char *path) {
    return dlopen(path, RTLD_NOLOAD | RTLD_LOCAL);
}
void *os_dlsym(void *handle, const char *name) {
    return dlsym(handle, name);
}
int os_dlclose(void *handle) {
    return dlclose(handle);
}
const char *os_dlerror(void) {
    const char *e = dlerror();
    return e ? e : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

static void fill_stat(const struct stat &st, OsStat *out) {
    out->size = (uint64_t)st.st_size;
    out->atime = (int64_t)st.st_atime;
    out->mtime = (int64_t)st.st_mtime;
    out->ctime = (int64_t)st.st_ctime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_regular = S_ISREG(st.st_mode) ? 1 : 0;
    out->is_symlink = S_ISLNK(st.st_mode) ? 1 : 0;
    out->is_readonly = (st.st_mode & S_IWUSR) ? 0 : 1;
}
int os_stat(const char *path, OsStat *out) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_lstat(const char *path, OsStat *out) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_mkdir(const char *path) {
    return mkdir(path, 0755);
}
int os_rename(const char *from, const char *to) {
    return rename(from, to);
}
int os_unlink(const char *path) {
    return unlink(path);
}
int os_rmdir(const char *path) {
    return rmdir(path);
}
int os_getcwd(char *buf, size_t cap) {
    return getcwd(buf, cap) ? 0 : -1;
}
int os_chdir(const char *path) {
    return chdir(path);
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fn(e->d_name, user) != 0)
            break;
    }
    closedir(d);
    return 0;
}
int os_mkstemp(char *template_path) {
    return mkstemp(template_path);
}

int os_mkdtemp(char *template_path) {
    return mkdtemp(template_path) ? 0 : -1;
}

const char *os_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    static char buf[4096];
    if (!t || !*t)
        return "/tmp";
    size_t n = strlen(t);
    if (n >= sizeof buf)
        return "/tmp";
    memcpy(buf, t, n + 1);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    return buf;
}

const char *os_null_device(void) {
    return "/dev/null";
}

int os_free_space(const char *path, uint64_t *bytes_out) {
    struct statvfs vfs;
    if (!bytes_out || statvfs(path, &vfs) != 0)
        return -1;
    *bytes_out = uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
    return 0;
}
int os_set_mtime(const char *path, int64_t mtime) {
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)mtime;
    times[0].tv_usec = times[1].tv_usec = 0;
    return utimes(path, times);
}
int os_registry_read(const char *, const char *, char *, size_t) {
    return -1;
}
int os_user_data_dir(const char *app, char *buf, size_t cap) {
    const char *home = getenv("HOME");
    char base[4096];
#ifdef __APPLE__
    if (!home || !*home)
        return -1;
    snprintf(base, sizeof base, "%s/Library/Application Support", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(base, sizeof base, "%s", xdg);
    else if (home && *home)
        snprintf(base, sizeof base, "%s/.local/share", home);
    else
        return -1;
#endif
    int n = snprintf(buf, cap, "%s/%s", base, app);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int os_font_dirs(char *buf, size_t cap) {
    const char *home = getenv("HOME");
    std::string dirs;
#ifdef __APPLE__
    if (home && *home)
        dirs += std::string(home) + "/Library/Fonts\n";
    dirs += "/Library/Fonts\n/System/Library/Fonts/Supplemental\n/System/Library/Fonts";
#else
    if (home && *home)
        dirs += std::string(home) + "/.fonts\n" + std::string(home) + "/.local/share/fonts\n";
    dirs += "/usr/local/share/fonts\n/usr/share/fonts";
#endif
    if (dirs.size() + 1 > cap)
        return -1;
    memcpy(buf, dirs.c_str(), dirs.size() + 1);
    return 0;
}

extern "C" char **environ;

int os_spawn(const char *const argv[], int64_t *pid_out) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0)
        return -1;
    *pid_out = pid;
    return 0;
}

int os_wait(int64_t pid, int *exit_code) {
    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    else
        *exit_code = -1;
    return 0;
}

static int native_flags(int flags) {
    int f = 0;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f = O_WRONLY;
        break;
    case OS_O_RDWR:
        f = O_RDWR;
        break;
    default:
        f = O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= O_CREAT;
    if (flags & OS_O_EXCL)
        f |= O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= O_TRUNC;
    return f;
}
int os_fd_open(const char *path, int flags) {
    return open(path, native_flags(flags), 0644);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    return (int64_t)read(fd, buf, n);
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    return (int64_t)write(fd, buf, n);
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return (int64_t)lseek(fd, (off_t)off, whence);
}
int os_fd_close(int fd) {
    return close(fd);
}
int os_fd_dup(int fd) {
    return dup(fd);
}
int os_fd_fsync(int fd) {
    return fsync(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return ftruncate(fd, (off_t)length);
}
int os_fd_stat(int fd, OsStat *out) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
#ifdef __EMSCRIPTEN__
    // The web build's files are packaged under /app: resources in /app/resources.
    snprintf(buf, cap, "/app/app");
    return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(buf, &size) == 0 ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return 0;
#endif
}

static OsFaultFn g_fault_fn = nullptr;
static OsFaultAddressFn g_fault_address_fn = nullptr;
static void fault_trampoline(int sig) {
    const char *what = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGBUS  ? "SIGBUS"
                       : sig == SIGABRT ? "an abort from the runtime"
                                        : "a fatal signal";
    g_fault_fn(what);
}
// SIGSEGV/SIGBUS with the faulting address. The address hook sees the fault
// first; then the report os_install_fault_handlers registered; then whatever
// handled the signal before either was installed, which is the default action
// unless something else (a debugger aid, a page tracker) was there first.
static struct sigaction g_previous_segv, g_previous_bus;
static bool g_access_handler_installed = false;
// The fault the address hook is looking at, and where it asked the thread to
// go next. Thread-local: each thread faults on its own.
static thread_local ucontext_t *t_fault_context = nullptr;
static thread_local void (*t_continue_fn)(uintptr_t) = nullptr;
static thread_local uintptr_t t_continue_arg = 0;

int os_fault_continue_in(void (*fn)(uintptr_t), uintptr_t arg) {
#if (defined(__APPLE__) || defined(__linux__)) && (defined(__aarch64__) || defined(__x86_64__))
    if (!t_fault_context || !fn)
        return 0;
    t_continue_fn = fn;
    t_continue_arg = arg;
    return 1;
#else
    (void)fn;
    (void)arg;
    return 0;
#endif
}

int os_fault_was_write(void) {
    ucontext_t *uc = t_fault_context;
    if (!uc)
        return -1;
#if defined(__APPLE__) && defined(__aarch64__)
    return (uc->uc_mcontext->__es.__esr >> 6) & 1; // ESR_ELx.WnR of the data abort
#elif defined(__APPLE__) && defined(__x86_64__)
    return (uc->uc_mcontext->__es.__err >> 1) & 1; // page-fault error code, W/R
#elif defined(__linux__) && defined(__x86_64__)
    return (int)((uc->uc_mcontext.gregs[REG_ERR] >> 1) & 1);
#else
    return -1;
#endif
}

// The interrupted thread resumes in fn(arg) when the handler returns: its
// program counter, first argument register and a stack that looks like the
// faulting instruction called fn (x86-64: the red zone skipped, a return
// slot pushed; arm64: no return address, fn never returns).
static void continue_faulted_thread(ucontext_t *uc, void (*fn)(uintptr_t), uintptr_t arg) {
#if defined(__APPLE__) && defined(__aarch64__)
    uc->uc_mcontext->__ss.__x[0] = arg;
    __darwin_arm_thread_state64_set_lr_fptr(uc->uc_mcontext->__ss, (void *)nullptr);
    __darwin_arm_thread_state64_set_pc_fptr(uc->uc_mcontext->__ss, (void *)fn);
#elif defined(__APPLE__) && defined(__x86_64__)
    uint64_t sp = ((uc->uc_mcontext->__ss.__rsp - 128) & ~(uint64_t)15) - 8;
    *(uint64_t *)sp = 0;
    uc->uc_mcontext->__ss.__rsp = sp;
    uc->uc_mcontext->__ss.__rdi = arg;
    uc->uc_mcontext->__ss.__rip = (uint64_t)fn;
#elif defined(__linux__) && defined(__aarch64__)
    uc->uc_mcontext.regs[0] = arg;
    uc->uc_mcontext.regs[30] = 0;
    uc->uc_mcontext.pc = (uint64_t)fn;
#elif defined(__linux__) && defined(__x86_64__)
    uint64_t sp = (((uint64_t)uc->uc_mcontext.gregs[REG_RSP] - 128) & ~(uint64_t)15) - 8;
    *(uint64_t *)sp = 0;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)sp;
    uc->uc_mcontext.gregs[REG_RDI] = (greg_t)arg;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)fn;
#else
    (void)uc;
    (void)fn;
    (void)arg;
#endif
}

static void access_fault(int sig, siginfo_t *info, void *ctx) {
    const char *what = sig == SIGSEGV ? "SIGSEGV" : "SIGBUS";
    if (g_fault_address_fn) {
        t_fault_context = (ucontext_t *)ctx;
        t_continue_fn = nullptr;
        int mine = g_fault_address_fn(what, (uintptr_t)(info ? info->si_addr : 0));
        void (*fn)(uintptr_t) = t_continue_fn;
        t_fault_context = nullptr;
        t_continue_fn = nullptr;
        if (mine) {
            if (fn)
                continue_faulted_thread((ucontext_t *)ctx, fn, t_continue_arg);
            return;
        }
    }
    if (g_fault_fn) {
        g_fault_fn(what);
        return;
    }
    struct sigaction *old = sig == SIGBUS ? &g_previous_bus : &g_previous_segv;
    if ((old->sa_flags & SA_SIGINFO) && old->sa_sigaction) {
        old->sa_sigaction(sig, info, ctx);
        return;
    }
    if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN && old->sa_handler) {
        old->sa_handler(sig);
        return;
    }
    // Default action: restore it, and the retried access takes it.
    sigaction(sig, old, nullptr);
}
static void install_access_handler() {
    if (g_access_handler_installed)
        return;
    g_access_handler_installed = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = access_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_previous_segv);
    sigaction(SIGBUS, &sa, &g_previous_bus);
}
int os_install_fault_handlers(OsFaultFn fn) {
    g_fault_fn = fn;
    install_access_handler();
    signal(SIGABRT, fault_trampoline);
    return 1;
}
int os_install_fault_address_hook(OsFaultAddressFn fn) {
    g_fault_address_fn = fn;
    install_access_handler();
    return 1;
}

void os_write_stderr_raw(const char *s, size_t n) {
    ssize_t ignored = write(2, s, n);
    (void)ignored;
}
void os_exit_immediately(int code) {
    _exit(code);
}

int os_localtime(int64_t seconds, struct tm *out) {
    time_t t = (time_t)seconds;
    return localtime_r(&t, out) ? 0 : -1;
}
int os_gmtime(int64_t seconds, struct tm *out) {
    time_t t = (time_t)seconds;
    return gmtime_r(&t, out) ? 0 : -1;
}

uint64_t os_monotonic_ns(void) {
#ifdef __APPLE__
    // CLOCK_MONOTONIC on Darwin works out the boot time on every call
    // (gettimeofday); the uptime clock is a plain mach_absolute_time read.
    // Every import call reads this clock, so the difference is large.
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t os_wall_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
void os_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, nullptr);
}

int os_strcasecmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

int os_setenv(const char *name, const char *value) {
    return setenv(name, value, 1);
}
int os_unsetenv(const char *name) {
    return unsetenv(name);
}

} // extern "C"

// ---------------------------------------------------------------------------
// Networking (os.h): BSD sockets, getifaddrs, the routing table, getaddrinfo.
// ---------------------------------------------------------------------------
#ifdef __EMSCRIPTEN__
// No sockets in a browser: every call fails with ENOSYS.
namespace {
int net_nosys() {
    errno = ENOSYS;
    return -1;
}
} // namespace
extern "C" {
int os_net_supported(void) {
    return 0;
}
int os_net_socket(int) {
    return net_nosys();
}
int os_net_close(int) {
    return net_nosys();
}
int os_net_bind(int, uint32_t, uint16_t) {
    return net_nosys();
}
int os_net_listen(int, int) {
    return net_nosys();
}
int os_net_accept(int, uint32_t *, uint16_t *) {
    return net_nosys();
}
int os_net_connect(int, uint32_t, uint16_t) {
    return net_nosys();
}
int os_net_local_address(int, uint32_t *, uint16_t *) {
    return net_nosys();
}
int os_net_peer_address(int, uint32_t *, uint16_t *) {
    return net_nosys();
}
int os_net_shutdown(int, int) {
    return net_nosys();
}
int64_t os_net_send(int, const void *, size_t, int) {
    return net_nosys();
}
int64_t os_net_recv(int, void *, size_t, int) {
    return net_nosys();
}
int64_t os_net_sendto(int, const void *, size_t, int, uint32_t, uint16_t, unsigned) {
    return net_nosys();
}
int64_t os_net_recvfrom(int, void *, size_t, int, OsNetDatagram *) {
    return net_nosys();
}
int os_net_readable(int, uint32_t *) {
    return net_nosys();
}
int os_net_pending_error(int) {
    return ENOSYS;
}
int os_net_set_option(int, int, int64_t) {
    return net_nosys();
}
int os_net_get_option(int, int, int64_t *) {
    return net_nosys();
}
int os_net_membership(int, int, uint32_t, uint32_t) {
    return net_nosys();
}
int os_net_poll(OsNetPoll *, size_t, int) {
    return net_nosys();
}
int os_net_wakeup_pair(int *) {
    return net_nosys();
}
void os_net_wakeup_signal(int) {}
void os_net_wakeup_drain(int) {}
int os_net_interfaces(OsNetInterface *, int) {
    return net_nosys();
}
int os_net_default_route(uint32_t *, unsigned *) {
    return net_nosys();
}
int os_net_route_source(uint32_t, uint32_t *) {
    return net_nosys();
}
int os_net_hostname(char *, size_t) {
    return net_nosys();
}
int os_net_resolve(const char *, uint32_t *, int, char *, size_t) {
    return OS_NET_RESOLVE_FAIL;
}
} // extern "C"
#else

namespace {

sockaddr_in net_sockaddr(uint32_t ip, uint16_t port) {
    sockaddr_in a;
    memset(&a, 0, sizeof a);
#ifdef __APPLE__
    a.sin_len = sizeof a;
#endif
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(ip);
    return a;
}

void net_unpack(const sockaddr_in &a, uint32_t *ip, uint16_t *port) {
    if (ip)
        *ip = ntohl(a.sin_addr.s_addr);
    if (port)
        *port = ntohs(a.sin_port);
}

uint32_t net_ip_of(const sockaddr *sa) {
    return sa && sa->sa_family == AF_INET ? ntohl(((const sockaddr_in *)sa)->sin_addr.s_addr) : 0;
}

// Every descriptor this layer hands out: non-blocking, not inherited by a
// child, and never a SIGPIPE for writing to a closed connection.
int net_prepare(int s) {
    if (s < 0)
        return -1;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return s;
}

int net_flags(int flags) {
    int f = 0;
    if (flags & OS_NET_MSG_OOB)
        f |= MSG_OOB;
    if (flags & OS_NET_MSG_PEEK)
        f |= MSG_PEEK;
    if (flags & OS_NET_MSG_DONTROUTE)
        f |= MSG_DONTROUTE;
#ifdef MSG_NOSIGNAL
    f |= MSG_NOSIGNAL;
#endif
    return f;
}

// The level and name a portable option maps to; false for none.
bool net_option(int option, int *level, int *name) {
    switch (option) {
    case OS_NET_OPT_BROADCAST:
        *level = SOL_SOCKET, *name = SO_BROADCAST;
        return true;
    case OS_NET_OPT_REUSEADDR:
        *level = SOL_SOCKET, *name = SO_REUSEADDR;
        return true;
    case OS_NET_OPT_REUSEPORT:
#ifdef SO_REUSEPORT
        *level = SOL_SOCKET, *name = SO_REUSEPORT;
        return true;
#else
        return false;
#endif
    case OS_NET_OPT_KEEPALIVE:
        *level = SOL_SOCKET, *name = SO_KEEPALIVE;
        return true;
    case OS_NET_OPT_DONTROUTE:
        *level = SOL_SOCKET, *name = SO_DONTROUTE;
        return true;
    case OS_NET_OPT_OOBINLINE:
        *level = SOL_SOCKET, *name = SO_OOBINLINE;
        return true;
    case OS_NET_OPT_RCVBUF:
        *level = SOL_SOCKET, *name = SO_RCVBUF;
        return true;
    case OS_NET_OPT_SNDBUF:
        *level = SOL_SOCKET, *name = SO_SNDBUF;
        return true;
    case OS_NET_OPT_NODELAY:
        *level = IPPROTO_TCP, *name = TCP_NODELAY;
        return true;
    case OS_NET_OPT_TTL:
        *level = IPPROTO_IP, *name = IP_TTL;
        return true;
    case OS_NET_OPT_TOS:
        *level = IPPROTO_IP, *name = IP_TOS;
        return true;
    case OS_NET_OPT_MULTICAST_TTL:
        *level = IPPROTO_IP, *name = IP_MULTICAST_TTL;
        return true;
    case OS_NET_OPT_MULTICAST_LOOP:
        *level = IPPROTO_IP, *name = IP_MULTICAST_LOOP;
        return true;
    case OS_NET_OPT_RECV_DESTINATION:
        *level = IPPROTO_IP, *name = IP_PKTINFO;
        return true;
    case OS_NET_OPT_TYPE:
        *level = SOL_SOCKET, *name = SO_TYPE;
        return true;
    case OS_NET_OPT_ACCEPTCONN:
        *level = SOL_SOCKET, *name = SO_ACCEPTCONN;
        return true;
    default:
        return false;
    }
}

} // namespace

extern "C" {

int os_net_supported(void) {
    return 1;
}
int os_net_socket(int type) {
    if (type != OS_NET_STREAM && type != OS_NET_DGRAM) {
        errno = EPROTOTYPE;
        return -1;
    }
    return net_prepare(socket(AF_INET, type == OS_NET_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0));
}
int os_net_close(int s) {
    return close(s) == 0 ? 0 : -1;
}
int os_net_bind(int s, uint32_t ip, uint16_t port) {
    sockaddr_in a = net_sockaddr(ip, port);
    return bind(s, (sockaddr *)&a, sizeof a) == 0 ? 0 : -1;
}
int os_net_listen(int s, int backlog) {
    return listen(s, backlog) == 0 ? 0 : -1;
}
int os_net_accept(int s, uint32_t *ip, uint16_t *port) {
    sockaddr_in a;
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    int c = accept(s, (sockaddr *)&a, &len);
    if (c < 0)
        return -1;
    net_unpack(a, ip, port);
    return net_prepare(c);
}
int os_net_connect(int s, uint32_t ip, uint16_t port) {
    sockaddr_in a = net_sockaddr(ip, port);
    return connect(s, (sockaddr *)&a, sizeof a) == 0 ? 0 : -1;
}
int os_net_local_address(int s, uint32_t *ip, uint16_t *port) {
    sockaddr_in a;
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    if (getsockname(s, (sockaddr *)&a, &len) != 0)
        return -1;
    net_unpack(a, ip, port);
    return 0;
}
int os_net_peer_address(int s, uint32_t *ip, uint16_t *port) {
    sockaddr_in a;
    socklen_t len = sizeof a;
    memset(&a, 0, sizeof a);
    if (getpeername(s, (sockaddr *)&a, &len) != 0)
        return -1;
    net_unpack(a, ip, port);
    return 0;
}
int os_net_shutdown(int s, int how) {
    return shutdown(s, how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR) == 0 ? 0 : -1;
}
int64_t os_net_send(int s, const void *buf, size_t n, int flags) {
    return (int64_t)send(s, buf, n, net_flags(flags));
}
int64_t os_net_recv(int s, void *buf, size_t n, int flags) {
    return (int64_t)recv(s, buf, n, net_flags(flags));
}
int64_t os_net_sendto(int s, const void *buf, size_t n, int flags, uint32_t ip, uint16_t port,
                      unsigned ifindex) {
    sockaddr_in a = net_sockaddr(ip, port);
    if (!ifindex)
        return (int64_t)sendto(s, buf, n, net_flags(flags), (sockaddr *)&a, sizeof a);
    // IP_PKTINFO names the interface for this one datagram (macOS and Linux
    // both honour it on send), leaving the socket's own routing untouched.
    union {
        char buf[CMSG_SPACE(sizeof(struct in_pktinfo))];
        struct cmsghdr align;
    } control;
    memset(&control, 0, sizeof control);
    iovec iov;
    iov.iov_base = (void *)buf;
    iov.iov_len = n;
    msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = &a;
    msg.msg_namelen = sizeof a;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof control.buf;
    cmsghdr *cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = IPPROTO_IP;
    cm->cmsg_type = IP_PKTINFO;
    cm->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
    struct in_pktinfo info;
    memset(&info, 0, sizeof info);
    info.ipi_ifindex = ifindex;
    memcpy(CMSG_DATA(cm), &info, sizeof info);
    return (int64_t)sendmsg(s, &msg, net_flags(flags));
}
int64_t os_net_recvfrom(int s, void *buf, size_t n, int flags, OsNetDatagram *info) {
    sockaddr_in a;
    memset(&a, 0, sizeof a);
    union {
        char buf[CMSG_SPACE(sizeof(struct in_pktinfo)) + 64];
        struct cmsghdr align;
    } control;
    iovec iov;
    iov.iov_base = buf;
    iov.iov_len = n;
    msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = &a;
    msg.msg_namelen = sizeof a;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.buf;
    msg.msg_controllen = sizeof control.buf;
    ssize_t got = recvmsg(s, &msg, net_flags(flags));
    if (got < 0)
        return -1;
    if (info) {
        memset(info, 0, sizeof *info);
        net_unpack(a, &info->from_ip, &info->from_port);
        info->truncated = (msg.msg_flags & MSG_TRUNC) ? 1 : 0;
        for (cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
            if (cm->cmsg_level != IPPROTO_IP || cm->cmsg_type != IP_PKTINFO)
                continue;
            struct in_pktinfo pi;
            memcpy(&pi, CMSG_DATA(cm), sizeof pi);
            info->to_ip = ntohl(pi.ipi_addr.s_addr);
            info->ifindex = (unsigned)pi.ipi_ifindex;
        }
    }
    return (int64_t)got;
}
int os_net_readable(int s, uint32_t *bytes) {
    int n = 0;
    if (ioctl(s, FIONREAD, &n) != 0)
        return -1;
    *bytes = n > 0 ? (uint32_t)n : 0u;
    return 0;
}
int os_net_pending_error(int s) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        return errno;
    return err;
}
int os_net_set_option(int s, int option, int64_t value) {
    if (option == OS_NET_OPT_LINGER) {
        struct linger l;
        l.l_onoff = value >= 0;
        l.l_linger = value >= 0 ? (int)value : 0;
        return setsockopt(s, SOL_SOCKET, SO_LINGER, &l, sizeof l) == 0 ? 0 : -1;
    }
    if (option == OS_NET_OPT_MULTICAST_IF) {
        in_addr a;
        a.s_addr = htonl((uint32_t)value);
        return setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof a) == 0 ? 0 : -1;
    }
    int level = 0, name = 0;
    if (!net_option(option, &level, &name) || option == OS_NET_OPT_TYPE ||
        option == OS_NET_OPT_ACCEPTCONN) {
        errno = ENOPROTOOPT;
        return -1;
    }
    if (option == OS_NET_OPT_MULTICAST_TTL || option == OS_NET_OPT_MULTICAST_LOOP) {
        unsigned char v = (unsigned char)value; // u_char options on BSD
        if (setsockopt(s, level, name, &v, sizeof v) == 0)
            return 0;
    }
    int v = (int)value;
    return setsockopt(s, level, name, &v, sizeof v) == 0 ? 0 : -1;
}
int os_net_get_option(int s, int option, int64_t *value) {
    if (option == OS_NET_OPT_LINGER) {
        struct linger l;
        socklen_t len = sizeof l;
        if (getsockopt(s, SOL_SOCKET, SO_LINGER, &l, &len) != 0)
            return -1;
        *value = l.l_onoff ? l.l_linger : -1;
        return 0;
    }
    if (option == OS_NET_OPT_MULTICAST_IF) {
        in_addr a;
        socklen_t len = sizeof a;
        if (getsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &a, &len) != 0)
            return -1;
        *value = ntohl(a.s_addr);
        return 0;
    }
    int level = 0, name = 0;
    if (!net_option(option, &level, &name)) {
        errno = ENOPROTOOPT;
        return -1;
    }
    unsigned char raw[sizeof(int)] = {0};
    socklen_t len = sizeof raw;
    if (getsockopt(s, level, name, raw, &len) != 0)
        return -1;
    if (len == 1) {
        *value = raw[0];
    } else {
        int v;
        memcpy(&v, raw, sizeof v);
        *value = v;
    }
    if (option == OS_NET_OPT_TYPE)
        *value = *value == SOCK_STREAM ? OS_NET_STREAM : OS_NET_DGRAM;
    return 0;
}
int os_net_membership(int s, int join, uint32_t group, uint32_t iface) {
    ip_mreq m;
    m.imr_multiaddr.s_addr = htonl(group);
    m.imr_interface.s_addr = htonl(iface);
    return setsockopt(s, IPPROTO_IP, join ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP, &m, sizeof m) ==
                   0
               ? 0
               : -1;
}
int os_net_poll(OsNetPoll *items, size_t n, int timeout_ms) {
    std::string storage(n * sizeof(pollfd), '\0');
    pollfd *p = (pollfd *)&storage[0];
    for (size_t i = 0; i < n; ++i) {
        p[i].fd = items[i].fd;
        p[i].events = (short)(((items[i].want & OS_NET_POLL_IN) ? POLLIN : 0) |
                              ((items[i].want & OS_NET_POLL_OUT) ? POLLOUT : 0) |
                              ((items[i].want & OS_NET_POLL_PRI) ? POLLPRI : 0));
        p[i].revents = 0;
        items[i].got = 0;
    }
    int r;
    do
        r = poll(p, (nfds_t)n, timeout_ms);
    while (r < 0 && errno == EINTR);
    if (r <= 0)
        return r;
    for (size_t i = 0; i < n; ++i) {
        short e = p[i].revents;
        items[i].got = ((e & POLLIN) ? OS_NET_POLL_IN : 0) | ((e & POLLOUT) ? OS_NET_POLL_OUT : 0) |
                       ((e & POLLPRI) ? OS_NET_POLL_PRI : 0) |
                       ((e & (POLLERR | POLLNVAL)) ? OS_NET_POLL_ERR : 0) |
                       ((e & POLLHUP) ? OS_NET_POLL_HUP : 0);
    }
    return r;
}
int os_net_wakeup_pair(int fds[2]) {
    if (pipe(fds) != 0)
        return -1;
    for (int i = 0; i < 2; ++i) {
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
        fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    }
    return 0;
}
void os_net_wakeup_signal(int write_fd) {
    char b = 1;
    ssize_t r = write(write_fd, &b, 1); // a full pipe is already a wake-up
    (void)r;
}
void os_net_wakeup_drain(int read_fd) {
    char b[64];
    while (read(read_fd, b, sizeof b) > 0) {
    }
}

int os_net_interfaces(OsNetInterface *out, int cap) {
    ifaddrs *all = nullptr;
    if (getifaddrs(&all) != 0)
        return -1;
    // Hardware addresses come from the link-layer entries, by name.
    struct Link {
        char name[32];
        uint8_t mac[8];
        int mac_len;
    };
    std::string links_storage;
    int nlinks = 0;
    for (ifaddrs *a = all; a; a = a->ifa_next) {
        if (!a->ifa_addr || !a->ifa_name)
            continue;
        Link l;
        memset(&l, 0, sizeof l);
#ifdef __APPLE__
        if (a->ifa_addr->sa_family != AF_LINK)
            continue;
        const sockaddr_dl *dl = (const sockaddr_dl *)a->ifa_addr;
        if (dl->sdl_alen > 0 && dl->sdl_alen <= 8) {
            memcpy(l.mac, LLADDR(dl), dl->sdl_alen);
            l.mac_len = dl->sdl_alen;
        }
#elif defined(__linux__)
        if (a->ifa_addr->sa_family != AF_PACKET)
            continue;
        const sockaddr_ll *ll = (const sockaddr_ll *)a->ifa_addr;
        if (ll->sll_halen > 0 && ll->sll_halen <= 8) {
            memcpy(l.mac, ll->sll_addr, ll->sll_halen);
            l.mac_len = ll->sll_halen;
        }
#else
        continue;
#endif
        snprintf(l.name, sizeof l.name, "%s", a->ifa_name);
        links_storage.append((const char *)&l, sizeof l);
        ++nlinks;
    }
    const Link *links = (const Link *)links_storage.data();
    int probe = socket(AF_INET, SOCK_DGRAM, 0);
    int count = 0;
    for (ifaddrs *a = all; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_name)
            continue;
        if (count < cap) {
            OsNetInterface &e = out[count];
            memset(&e, 0, sizeof e);
            snprintf(e.name, sizeof e.name, "%s", a->ifa_name);
            e.index = if_nametoindex(a->ifa_name);
            e.ip = net_ip_of(a->ifa_addr);
            e.netmask = net_ip_of(a->ifa_netmask);
            unsigned flags = a->ifa_flags;
            e.up = (flags & IFF_UP) != 0;
            e.running = (flags & IFF_RUNNING) != 0;
            e.loopback = (flags & IFF_LOOPBACK) != 0;
            e.point_to_point = (flags & IFF_POINTOPOINT) != 0;
            e.can_broadcast = (flags & IFF_BROADCAST) != 0;
            if (e.can_broadcast)
                e.broadcast = net_ip_of(a->ifa_broadaddr);
            else if (e.point_to_point)
                e.peer = net_ip_of(a->ifa_dstaddr);
            if (e.can_broadcast && !e.broadcast && e.netmask)
                e.broadcast = e.ip | ~e.netmask;
            for (int i = 0; i < nlinks; ++i)
                if (strcmp(links[i].name, e.name) == 0) {
                    memcpy(e.mac, links[i].mac, sizeof e.mac);
                    e.mac_len = links[i].mac_len;
                }
            if (probe >= 0) {
                ifreq req;
                memset(&req, 0, sizeof req);
                snprintf(req.ifr_name, sizeof req.ifr_name, "%s", a->ifa_name);
                if (ioctl(probe, SIOCGIFMTU, &req) == 0)
                    e.mtu = req.ifr_mtu;
            }
        }
        ++count;
    }
    if (probe >= 0)
        close(probe);
    freeifaddrs(all);
    return count;
}

int os_net_default_route(uint32_t *gateway, unsigned *ifindex) {
#ifdef __APPLE__
    // The routing table as the kernel dumps it: an rt_msghdr per route, each
    // followed by the sockaddrs its rtm_addrs bits name, 4-byte aligned. A
    // scoped default route (one per interface) is a fallback for the unscoped
    // one, which is the system's choice.
    int mib[6] = {CTL_NET, PF_ROUTE, 0, AF_INET, NET_RT_FLAGS, RTF_GATEWAY};
    size_t len = 0;
    if (sysctl(mib, 6, nullptr, &len, nullptr, 0) != 0 || !len)
        return -1;
    std::string buf(len + 1024, '\0');
    len = buf.size();
    if (sysctl(mib, 6, &buf[0], &len, nullptr, 0) != 0)
        return -1;
    bool found = false;
    for (size_t off = 0; off + sizeof(rt_msghdr) <= len;) {
        const rt_msghdr *rtm = (const rt_msghdr *)(buf.data() + off);
        if (!rtm->rtm_msglen)
            break;
        const char *p = (const char *)(rtm + 1);
        const char *end = buf.data() + off + rtm->rtm_msglen;
        const sockaddr *addrs[RTAX_MAX] = {};
        for (int i = 0; i < RTAX_MAX && p < end; ++i) {
            if (!(rtm->rtm_addrs & (1 << i)))
                continue;
            const sockaddr *sa = (const sockaddr *)p;
            addrs[i] = sa;
            size_t sl = sa->sa_len ? (size_t)sa->sa_len : sizeof(uint32_t);
            p += (sl + sizeof(uint32_t) - 1) & ~(sizeof(uint32_t) - 1);
        }
        off += rtm->rtm_msglen;
        const sockaddr *dst = addrs[RTAX_DST], *gw = addrs[RTAX_GATEWAY];
        const sockaddr *mask = addrs[RTAX_NETMASK];
        if (!dst || dst->sa_family != AF_INET || net_ip_of(dst) != 0)
            continue;
        if (mask && mask->sa_len >= offsetof(sockaddr_in, sin_addr) + 4 && net_ip_of(mask) != 0)
            continue;
        if (!gw || gw->sa_family != AF_INET)
            continue;
#ifdef RTF_IFSCOPE
        const bool scoped = (rtm->rtm_flags & RTF_IFSCOPE) != 0;
#else
        const bool scoped = false;
#endif
        if (found && scoped)
            continue;
        *gateway = net_ip_of(gw);
        *ifindex = rtm->rtm_index;
        found = true;
        if (!scoped)
            break;
    }
    return found ? 0 : -1;
#elif defined(__linux__)
    FILE *f = fopen("/proc/net/route", "r");
    if (!f)
        return -1;
    char line[256], name[64];
    bool found = false;
    unsigned long dst = 0, gw = 0, flags = 0, mask = 0;
    while (fgets(line, sizeof line, f)) {
        int refcnt, use, metric;
        if (sscanf(line, "%63s %lx %lx %lx %d %d %d %lx", name, &dst, &gw, &flags, &refcnt, &use,
                   &metric, &mask) != 8)
            continue;
        if (dst == 0 && mask == 0 && (flags & 0x2)) { // RTF_GATEWAY
            *gateway = ntohl((uint32_t)gw);
            *ifindex = if_nametoindex(name);
            found = true;
            break;
        }
    }
    fclose(f);
    return found ? 0 : -1;
#else
    (void)gateway;
    (void)ifindex;
    errno = ENOSYS;
    return -1;
#endif
}

int os_net_route_source(uint32_t dest, uint32_t *source_ip) {
    // Connecting a datagram socket asks the routing table and sends nothing.
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;
    sockaddr_in a = net_sockaddr(dest, 9);
    int rc = -1;
    if (connect(s, (sockaddr *)&a, sizeof a) == 0) {
        sockaddr_in me;
        socklen_t len = sizeof me;
        if (getsockname(s, (sockaddr *)&me, &len) == 0) {
            *source_ip = ntohl(me.sin_addr.s_addr);
            rc = 0;
        }
    }
    close(s);
    return rc;
}

int os_net_hostname(char *buf, size_t cap) {
    if (!cap)
        return -1;
    if (gethostname(buf, cap) != 0)
        return -1;
    buf[cap - 1] = 0;
    return 0;
}

int os_net_resolve(const char *name, uint32_t *ips, int cap, char *canonical,
                   size_t canonical_cap) {
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM; // one entry per address, not one per socket type
    hints.ai_flags = AI_CANONNAME;
    addrinfo *res = nullptr;
    int rc = getaddrinfo(name, nullptr, &hints, &res);
    if (rc != 0) {
        switch (rc) {
        case EAI_NONAME:
            return OS_NET_RESOLVE_NOT_FOUND;
        case EAI_AGAIN:
            return OS_NET_RESOLVE_AGAIN;
#if defined(EAI_NODATA) && EAI_NODATA != EAI_NONAME
        case EAI_NODATA:
            return OS_NET_RESOLVE_NO_DATA;
#endif
#ifdef EAI_ADDRFAMILY
        case EAI_ADDRFAMILY:
            return OS_NET_RESOLVE_NO_DATA;
#endif
        default:
            return OS_NET_RESOLVE_FAIL;
        }
    }
    int n = 0;
    if (canonical && canonical_cap) {
        snprintf(canonical, canonical_cap, "%s",
                 res && res->ai_canonname ? res->ai_canonname : name);
    }
    for (addrinfo *a = res; a; a = a->ai_next) {
        if (a->ai_family != AF_INET || !a->ai_addr)
            continue;
        uint32_t ip = net_ip_of(a->ai_addr);
        bool dup = false;
        for (int i = 0; i < n && i < cap; ++i)
            dup |= ips[i] == ip;
        if (dup)
            continue;
        if (n < cap)
            ips[n] = ip;
        ++n;
    }
    freeaddrinfo(res);
    return n ? n : OS_NET_RESOLVE_NO_DATA;
}

} // extern "C"
#endif // __EMSCRIPTEN__
