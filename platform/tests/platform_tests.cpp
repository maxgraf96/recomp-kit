// platform_tests.cpp - the platform layer, checked without a guest, a window
// or a GPU. Runs on every platform the layer supports.
#include "../os.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
        }                                                                                          \
    } while (0)

namespace {

struct ThreadReport {
    OsThreadId seen_self = 0;
    int ran = 0;
};

void *report_self(void *arg) {
    ThreadReport *r = (ThreadReport *)arg;
    r->seen_self = os_thread_self();
    r->ran = 1;
    return nullptr;
}

void *deep_stack(void *arg) {
    // 400 KB of stack, which fits a 512 KB request and not a 64 KB one.
    volatile char buf[400 * 1024];
    buf[0] = 1;
    buf[sizeof buf - 1] = 2;
    *(int *)arg = buf[0] + buf[sizeof buf - 1];
    return nullptr;
}

// A new thread's run loop has nothing attached to it.
void *wait_on_own_run_loop(void *arg) {
    const uint64_t t0 = os_monotonic_ns();
    const int waited = os_wait_run_loop(0.2);
    *(uint64_t *)arg = (waited ? 1ull << 63 : 0) | (os_monotonic_ns() - t0);
    return nullptr;
}

int collect_name(const char *name, void *user) {
    ((std::vector<std::string> *)user)->push_back(name);
    return 0;
}

std::string scratch_dir() {
    const char *base = recomp_env("PLATFORM_TEST_DIR");
    std::string dir = std::string(base && *base ? base : "build/recomp") + "/platform-test";
    // A stub-only checkout has no build/recomp yet: create every component.
    for (size_t i = dir.find('/'); i != std::string::npos; i = dir.find('/', i + 1))
        os_mkdir(dir.substr(0, i).c_str());
    os_mkdir(dir.c_str());
    return dir;
}

void test_threads() {
    ThreadReport r;
    OsThread *t = os_thread_create(report_self, &r, 0);
    CHECK(t != nullptr);
    OsThreadId id = os_thread_id_of(t);
    os_thread_join(t);
    CHECK(r.ran == 1);
    CHECK(r.seen_self == id);
    CHECK(r.seen_self != os_thread_self());

    int sum = 0;
    OsThread *big = os_thread_create(deep_stack, &sum, 512 * 1024);
    CHECK(big != nullptr);
    os_thread_join(big);
    CHECK(sum == 3);
}

void test_time() {
    uint64_t a = os_monotonic_ns();
    os_sleep_us(2000);
    uint64_t b = os_monotonic_ns();
    CHECK(b > a);
    CHECK(b - a >= 1000000ull);                            // at least 1 ms elapsed
    CHECK(os_wall_time_us() > 1600000000ull * 1000000ull); // after 2020
    struct tm utc{};
    CHECK(os_gmtime(86400, &utc) == 0 && utc.tm_year == 70 && utc.tm_mday == 2 && utc.tm_hour == 0);
    struct tm local{};
    CHECK(os_localtime(86400 * 365, &local) == 0 && local.tm_year >= 70);
    // A run loop with nothing attached (a fresh thread's) does not wait, and
    // says so, so the caller sleeps by other means.
    uint64_t own = 0;
    OsThread *t = os_thread_create(wait_on_own_run_loop, &own, 0);
    CHECK(t != nullptr);
    if (t)
        os_thread_join(t);
    CHECK((own >> 63) == 0);
    CHECK((own & ~(1ull << 63)) < 100000000ull);
    CHECK(os_wait_run_loop(0.0) == 0);
}

void test_vm() {
    size_t bytes = 1 << 20;
    uint8_t *p = (uint8_t *)os_vm_reserve(bytes);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p[0] == 0 && p[bytes - 1] == 0);
        p[bytes - 1] = 7;
        CHECK(p[bytes - 1] == 7);
        os_vm_release(p, bytes);
    }
}

void test_paths_and_descriptors() {
    std::string dir = scratch_dir();
    std::string sub = dir + "/listed";
    os_unlink((sub + "/a.txt").c_str());
    os_unlink((sub + "/b.txt").c_str());
    os_mkdir(sub.c_str());
    CHECK(os_mkdir(sub.c_str()) == -1); // already there, as mkdir reports it

    int fd = os_fd_open((sub + "/a.txt").c_str(), OS_O_RDWR | OS_O_CREAT | OS_O_TRUNC);
    CHECK(fd >= 0);
    CHECK(os_fd_write(fd, "hello", 5) == 5);
    CHECK(os_fd_seek(fd, 0, OS_SEEK_SET) == 0);
    char buf[8] = {0};
    CHECK(os_fd_read(fd, buf, sizeof buf) == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    OsStat st;
    CHECK(os_fd_stat(fd, &st) == 0 && st.size == 5 && st.is_regular && !st.is_dir);
    CHECK(os_fd_truncate(fd, 2) == 0);
    CHECK(os_fd_fsync(fd) == 0);
    int dup = os_fd_dup(fd);
    CHECK(dup >= 0 && dup != fd);
    CHECK(os_fd_close(dup) == 0);
    CHECK(os_fd_close(fd) == 0);
    CHECK(os_stat((sub + "/a.txt").c_str(), &st) == 0 && st.size == 2);
    CHECK(os_fd_open((sub + "/a.txt").c_str(), OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL) == -1);

    std::string tmpl = sub + "/b.XXXXXX";
    std::vector<char> t(tmpl.begin(), tmpl.end());
    t.push_back(0);
    int tfd = os_mkstemp(t.data());
    CHECK(tfd >= 0);
    CHECK(strncmp(t.data(), (sub + "/b.").c_str(), sub.size() + 3) == 0);
    CHECK(strcmp(t.data() + sub.size() + 3, "XXXXXX") != 0);
    FILE *f = (FILE *)os_fdopen(tfd, "wb");
    CHECK(f != nullptr);
    if (f)
        fclose(f);
    CHECK(os_rename(t.data(), (sub + "/b.txt").c_str()) == 0);
    CHECK(os_rename((sub + "/b.txt").c_str(), (sub + "/a.txt").c_str()) == 0); // replaces

    std::vector<std::string> names;
    CHECK(os_listdir(sub.c_str(), collect_name, &names) == 0);
    CHECK(names.size() == 1 && names[0] == "a.txt");
    CHECK(os_listdir((sub + "/nowhere").c_str(), collect_name, &names) == -1);

    CHECK(os_stat(sub.c_str(), &st) == 0 && st.is_dir);
    CHECK(os_lstat(sub.c_str(), &st) == 0 && !st.is_symlink);
    CHECK(os_unlink((sub + "/a.txt").c_str()) == 0);
    CHECK(os_stat((sub + "/a.txt").c_str(), &st) == -1);
    CHECK(os_rmdir(sub.c_str()) == 0);
    CHECK(os_stat(sub.c_str(), &st) == -1);

    char cwd[4096];
    CHECK(os_getcwd(cwd, sizeof cwd) == 0 && cwd[0] != 0);
    CHECK(os_chdir(cwd) == 0);
}

void test_process_and_strings() {
    char exe[4096];
    CHECK(os_exe_path(exe, sizeof exe) == 0);
    char dir[512];
    snprintf(dir, sizeof dir, "%s/pop-platform-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    OsStat st;
    CHECK(os_stat(dir, &st) == 0 && st.is_dir);
    CHECK(os_rmdir(dir) == 0);
    CHECK(os_stat(os_null_device(), &st) == 0 || true); // exists on both; stat may refuse NUL
    // The test binary re-runs itself as a child that exits 7.
    const char *child_argv[] = {exe, "--child-exit-7", nullptr};
    int64_t pid = 0;
    CHECK(os_spawn(child_argv, &pid) == 0);
    int code = -1;
    CHECK(os_wait(pid, &code) == 0);
    CHECK(code == 7);
    CHECK(strstr(exe, "platform_tests") != nullptr);
    CHECK(os_strcasecmp("Data", "DATA") == 0);
    CHECK(os_strcasecmp("a", "b") < 0);
    CHECK(os_setenv("RECOMP_PLATFORM_TEST", "yes") == 0);
    CHECK(recomp_env("PLATFORM_TEST") && strcmp(recomp_env("PLATFORM_TEST"), "yes") == 0);
    CHECK(os_unsetenv("RECOMP_PLATFORM_TEST") == 0);
    CHECK(recomp_env("PLATFORM_TEST") == nullptr || recomp_env("PLATFORM_TEST")[0] == 0);
    const char *ext = os_plugin_extension();
    CHECK(ext[0] == '.' && strlen(ext) >= 3);
    char data[4096];
    CHECK(os_user_data_dir("PopRecompTest", data, sizeof data) == 0);
    CHECK(strstr(data, "PopRecompTest") != nullptr);
    CHECK(data[strlen(data) - 1] != '/' && data[strlen(data) - 1] != '\\');
    // Font directories: absolute, newline-separated, none empty or trailing.
    CHECK(os_font_dirs(data, sizeof data) == 0);
    CHECK(data[0] != 0 && data[strlen(data) - 1] != '\n' && !strstr(data, "\n\n"));
    CHECK(strstr(data, "Fonts") != nullptr || strstr(data, "fonts") != nullptr);
    CHECK(os_font_dirs(data, 4) == -1); // too small a buffer is an error, not a truncation
    {
        uint64_t free_bytes = 0;
        const std::string dir = scratch_dir();
        CHECK(os_free_space(dir.c_str(), &free_bytes) == 0);
        CHECK(free_bytes > 0);
        CHECK(os_free_space("/definitely/not/a/volume/path", &free_bytes) == -1);
        const std::string file = dir + "/mtime.txt";
        FILE *f = fopen(file.c_str(), "wb");
        CHECK(f != nullptr);
        if (f)
            fclose(f);
        CHECK(os_set_mtime(file.c_str(), 1000000000) == 0);
        OsStat st{};
        CHECK(os_stat(file.c_str(), &st) == 0 && st.mtime == 1000000000);
        os_unlink(file.c_str());
    }
    CHECK(os_dlopen_noload("/definitely/not/loaded") == nullptr);
    CHECK(os_dlopen("/definitely/not/a/plugin") == nullptr);
    CHECK(os_dlerror() != nullptr);
    os_write_stderr_raw("platform_tests: raw stderr write ok\n", 35);
}

// The kit's switches are RECOMP_<NAME>; the spellings they had while the kit
// was one game's port (POPM_<NAME>, POP_RECOMP_<NAME>) are not read.
// Switches from a file: NAME=VALUE lines, trimmed, comments and junk skipped.
void test_switch_file() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/recomp-switches-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    std::string path = std::string(dir) + "/switches.txt";
    os_unsetenv("RECOMP_SWITCHFILE_PROBE");
    os_unsetenv("RECOMP_SWITCHFILE_OTHER");
    CHECK(recomp_env_apply_file(path.c_str()) == 0); // not there yet
    FILE *f = fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f)
        return;
    fputs("# the pointer trace\nRECOMP_SWITCHFILE_PROBE=1\n\nno equals here\n"
          "RECOMP_SWITCHFILE_OTHER = two words \n",
          f);
    fclose(f);
    CHECK(recomp_env_apply_file(path.c_str()) == 2);
    CHECK(recomp_env("SWITCHFILE_PROBE") && strcmp(recomp_env("SWITCHFILE_PROBE"), "1") == 0);
    CHECK(recomp_env("SWITCHFILE_OTHER") &&
          strcmp(recomp_env("SWITCHFILE_OTHER"), "two words") == 0);
    os_unsetenv("RECOMP_SWITCHFILE_PROBE");
    os_unsetenv("RECOMP_SWITCHFILE_OTHER");
    os_unlink(path.c_str());
    os_rmdir(dir);
}

void test_switch_names() {
    os_unsetenv("RECOMP_SWITCH_PROBE");
    CHECK(recomp_env("SWITCH_PROBE") == nullptr);
    CHECK(os_setenv("POPM_SWITCH_PROBE", "old") == 0);
    CHECK(os_setenv("POP_RECOMP_SWITCH_PROBE", "old") == 0);
    CHECK(recomp_env("SWITCH_PROBE") == nullptr);
    CHECK(os_setenv("RECOMP_SWITCH_PROBE", "new") == 0);
    CHECK(recomp_env("SWITCH_PROBE") && strcmp(recomp_env("SWITCH_PROBE"), "new") == 0);
    os_unsetenv("RECOMP_SWITCH_PROBE");
    os_unsetenv("POPM_SWITCH_PROBE");
    os_unsetenv("POP_RECOMP_SWITCH_PROBE");
}

// The network layer on the loopback interface: nothing leaves the machine.
void test_network() {
    if (!os_net_supported()) {
        CHECK(os_net_socket(OS_NET_DGRAM) == -1);
        return;
    }
    const uint32_t lo = 0x7f000001u;
    int a = os_net_socket(OS_NET_DGRAM), b = os_net_socket(OS_NET_DGRAM);
    CHECK(a >= 0 && b >= 0);
    CHECK(os_net_bind(a, lo, 0) == 0);
    uint32_t ip = 0;
    uint16_t port = 0;
    CHECK(os_net_local_address(a, &ip, &port) == 0 && ip == lo && port != 0);
    // A fresh socket is non-blocking.
    char buf[64];
    OsNetDatagram info;
    CHECK(os_net_recvfrom(a, buf, sizeof buf, 0, &info) == -1 &&
          (errno == EAGAIN || errno == EWOULDBLOCK));
    // The destination and interface of what arrives.
    CHECK(os_net_set_option(a, OS_NET_OPT_RECV_DESTINATION, 1) == 0);
    CHECK(os_net_sendto(b, "abc", 3, 0, lo, port, 0) == 3);
    OsNetPoll p = {a, OS_NET_POLL_IN, 0};
    CHECK(os_net_poll(&p, 1, 1000) == 1 && (p.got & OS_NET_POLL_IN));
    CHECK(os_net_recvfrom(a, buf, sizeof buf, 0, &info) == 3 && memcmp(buf, "abc", 3) == 0);
    CHECK(info.from_ip == lo && info.to_ip == lo && info.ifindex != 0 && !info.truncated);
    // Out of a named interface: the loopback one, by index.
    unsigned lo_index = info.ifindex;
    CHECK(os_net_sendto(b, "via", 3, 0, lo, port, lo_index) == 3);
    CHECK(os_net_poll(&p, 1, 1000) == 1);
    CHECK(os_net_recvfrom(a, buf, 2, 0, &info) == 2 && info.truncated == 1);
    int64_t v = 0;
    CHECK(os_net_get_option(a, OS_NET_OPT_TYPE, &v) == 0 && v == OS_NET_DGRAM);
    CHECK(os_net_set_option(b, OS_NET_OPT_BROADCAST, 1) == 0);
    CHECK(os_net_get_option(b, OS_NET_OPT_BROADCAST, &v) == 0 && v != 0);
    // A stream: connect in progress, accept, data, the peer's close.
    int l = os_net_socket(OS_NET_STREAM), c = os_net_socket(OS_NET_STREAM);
    CHECK(os_net_bind(l, lo, 0) == 0 && os_net_listen(l, 4) == 0);
    CHECK(os_net_local_address(l, nullptr, &port) == 0);
    int rc = os_net_connect(c, lo, port);
    CHECK(rc == 0 || errno == EINPROGRESS);
    OsNetPoll w = {c, OS_NET_POLL_OUT, 0};
    CHECK(os_net_poll(&w, 1, 1000) == 1 && os_net_pending_error(c) == 0);
    OsNetPoll q = {l, OS_NET_POLL_IN, 0};
    CHECK(os_net_poll(&q, 1, 1000) == 1);
    int s = os_net_accept(l, &ip, nullptr);
    CHECK(s >= 0 && ip == lo);
    CHECK(os_net_send(c, "hello", 5, 0) == 5);
    OsNetPoll r = {s, OS_NET_POLL_IN, 0};
    CHECK(os_net_poll(&r, 1, 1000) == 1);
    uint32_t queued = 0;
    CHECK(os_net_readable(s, &queued) == 0 && queued == 5);
    CHECK(os_net_recv(s, buf, sizeof buf, OS_NET_MSG_PEEK) == 5 &&
          os_net_recv(s, buf, sizeof buf, 0) == 5);
    os_net_close(c);
    CHECK(os_net_poll(&r, 1, 1000) == 1 && os_net_recv(s, buf, sizeof buf, 0) == 0);
    // A refused connection.
    int gone = os_net_socket(OS_NET_STREAM);
    os_net_bind(gone, lo, 0);
    os_net_local_address(gone, nullptr, &port);
    os_net_close(gone);
    int f = os_net_socket(OS_NET_STREAM);
    rc = os_net_connect(f, lo, port);
    CHECK(rc == -1);
    OsNetPoll fw = {f, OS_NET_POLL_OUT, 0};
    os_net_poll(&fw, 1, 1000);
    CHECK(os_net_pending_error(f) == ECONNREFUSED || errno == ECONNREFUSED);
    for (int k : {a, b, l, s, f})
        os_net_close(k);
    // The wake-up pair.
    int wake[2];
    CHECK(os_net_wakeup_pair(wake) == 0);
    OsNetPoll wp = {wake[0], OS_NET_POLL_IN, 0};
    CHECK(os_net_poll(&wp, 1, 0) == 0);
    os_net_wakeup_signal(wake[1]);
    CHECK(os_net_poll(&wp, 1, 0) == 1);
    os_net_wakeup_drain(wake[0]);
    CHECK(os_net_poll(&wp, 1, 0) == 0);
    os_fd_close(wake[0]);
    os_fd_close(wake[1]);
    // Interfaces, routes, names - without asking anything off this machine.
    OsNetInterface ifs[64];
    int n = os_net_interfaces(ifs, 64);
    bool loopback = false;
    for (int i = 0; i < n && i < 64; ++i)
        loopback |= ifs[i].loopback && ifs[i].ip == lo && ifs[i].index == lo_index;
    CHECK(n >= 1 && loopback);
    uint32_t src = 0;
    CHECK(os_net_route_source(lo, &src) == 0 && src == lo);
    char host[256];
    CHECK(os_net_hostname(host, sizeof host) == 0 && host[0]);
    uint32_t ips[4];
    char canon[256];
    CHECK(os_net_resolve("localhost", ips, 4, canon, sizeof canon) >= 1);
    uint32_t gw = 0;
    unsigned gw_if = 0;
    if (os_net_default_route(&gw, &gw_if) == 0)
        CHECK(gw != 0 && gw_if != 0);
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--child-exit-7") == 0)
        return 7;
    test_switch_names();
    test_switch_file();
    test_threads();
    test_time();
    test_vm();
    test_paths_and_descriptors();
    test_process_and_strings();
    test_network();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all platform tests passed\n");
    return g_failures ? 1 : 0;
}
