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
