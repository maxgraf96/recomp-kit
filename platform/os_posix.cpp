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
