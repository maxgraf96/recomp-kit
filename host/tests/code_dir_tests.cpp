// code_dir_tests.cpp - the player host's code directory (host/code_dir.h):
// build.json's load order, the bundles dlopen()ed in it, and every way a
// directory can be wrong, each with the message a player would see; then
// where the runtime finds symbols.json (RECOMP_SYMBOLS, RECOMP_CODE_DIR).
// The bundles are code_dir_fixture.c, built by CMake: one registers a
// translated module, the other registers nothing. Needs no game.
//
//   code_dir_tests <fixture.bundle> <empty-fixture.bundle>
#include "../code_dir.h"
#include "../../platform/os.h"
#include "../../runtime/layout.h"
#include "../../runtime/x86.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
// What this test wrote, removed at the end in reverse order.
std::vector<std::string> g_written;

bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    va_list ap;
    va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!ok)
        ++g_failures;
    printf("%s %s\n", ok ? "ok  " : "FAIL", msg);
    return ok;
}

bool contains(const std::string &s, const char *part) {
    return s.find(part) != std::string::npos;
}

void write_file(const std::string &path, const std::string &text) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path.c_str());
        exit(2);
    }
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    g_written.push_back(path);
}

bool copy_file(const std::string &from, const std::string &to) {
    FILE *in = fopen(from.c_str(), "rb");
    if (!in)
        return false;
    FILE *out = fopen(to.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    g_written.push_back(to);
    return fclose(out) == 0;
}

// A fresh code directory under the temporary root.
std::string make_dir(const std::string &root, const char *name) {
    std::string dir = root + "/" + name;
    os_mkdir(dir.c_str());
    g_written.push_back(dir);
    return dir;
}

std::string order(const std::vector<std::string> &names) {
    std::string s = "{\"key\": \"0123456789abcdef\", \"load_order\": [";
    for (size_t i = 0; i < names.size(); ++i)
        s += (i ? ", \"" : "\"") + names[i] + "\"";
    return s + "], \"bundles\": []}\n";
}

void refusals(const std::string &root, const std::string &empty_fixture) {
    std::string error;
    std::string dir = make_dir(root, "no-manifest");
    check(!code_dir_load(dir, &error) && contains(error, "build.json") &&
              contains(error, "Build it again"),
          "no build.json: %s", error.c_str());

    dir = make_dir(root, "bad-json");
    write_file(dir + "/build.json", "{\"load_order\": [\"main.bundle\"");
    error.clear();
    check(!code_dir_load(dir, &error) && contains(error, "not valid JSON"), "malformed: %s",
          error.c_str());

    dir = make_dir(root, "no-order");
    write_file(dir + "/build.json", "{\"key\": \"x\", \"load_order\": []}");
    error.clear();
    check(!code_dir_load(dir, &error) && contains(error, "names no bundles"), "empty order: %s",
          error.c_str());

    dir = make_dir(root, "escape");
    for (const char *name : {"../main.bundle", "sub/main.bundle", ".hidden.bundle"}) {
        write_file(dir + "/build.json", order({name}));
        error.clear();
        check(!code_dir_load(dir, &error) && contains(error, "file names"), "%s refused: %s", name,
              error.c_str());
    }

    dir = make_dir(root, "missing");
    write_file(dir + "/build.json", order({"a.bundle", "main.bundle"}));
    error.clear();
    check(!code_dir_load(dir, &error) && contains(error, "incomplete") &&
              contains(error, "a.bundle is missing"),
          "missing bundle: %s", error.c_str());

    dir = make_dir(root, "not-a-bundle");
    write_file(dir + "/build.json", order({"main.bundle"}));
    write_file(dir + "/main.bundle", "not a Mach-O file\n");
    error.clear();
    check(!code_dir_load(dir, &error) && contains(error, "Cannot load"), "not a bundle: %s",
          error.c_str());

    dir = make_dir(root, "registers-nothing");
    write_file(dir + "/build.json", order({"empty.bundle"}));
    check(copy_file(empty_fixture, dir + "/empty.bundle"), "copied the empty fixture");
    const uint32_t before = recomp_module_count();
    error.clear();
    check(!code_dir_load(dir, &error) && contains(error, "registered no translated code"),
          "registers nothing: %s", error.c_str());
    check(recomp_module_count() == before, "...and no module was registered");
}

void loads(const std::string &root, const std::string &fixture) {
    std::string dir = make_dir(root, "good");
    write_file(dir + "/build.json", order({"fixture.bundle"}));
    check(copy_file(fixture, dir + "/fixture.bundle"), "copied the fixture");

    std::vector<std::string> names;
    std::string error;
    check(code_dir_read_order(dir, &names, &error) && names.size() == 1 &&
              names[0] == "fixture.bundle",
          "read the load order");

    // Unset, the environment's loader does nothing: RECOMP_EXTRA_CODE may
    // still name code, and boot decides whether the entry point is covered.
    unsetenv("RECOMP_CODE_DIR");
    check(code_dir_load_env(&error), "RECOMP_CODE_DIR unset: nothing to load");
    check(recomp_module_lookup(0x00401000u) < 0, "...and nothing registered");

    setenv("RECOMP_CODE_DIR", dir.c_str(), 1);
    error.clear();
    const uint32_t before = recomp_module_count();
    check(code_dir_load_env(&error), "RECOMP_CODE_DIR loads its bundles %s", error.c_str());
    check(recomp_module_count() == before + 1, "one module registered");
    check(recomp_module_lookup(0x00401000u) >= 0, "the fixture's function is translated");
    const RecompModule *m = recomp_module_named("code-dir-fixture.dll");
    if (check(m != nullptr, "registered by name")) {
        X86 c;
        memset(&c, 0, sizeof c);
        m->base_ptrs[0](&c);
        check(c.r[R_EAX] == 0x600dc0deu, "the loaded function runs");
    }

    // The translation index lives with the bundles, not in the app.
    check(host_resource("symbols.json") == dir + "/symbols.json",
          "symbols.json is RECOMP_CODE_DIR's: %s", host_resource("symbols.json").c_str());
    setenv("RECOMP_SYMBOLS", "/elsewhere/symbols.json", 1);
    check(host_resource("symbols.json") == "/elsewhere/symbols.json",
          "RECOMP_SYMBOLS wins: %s", host_resource("symbols.json").c_str());
    unsetenv("RECOMP_SYMBOLS");
    unsetenv("RECOMP_CODE_DIR");
    check(host_resource("symbols.json") != dir + "/symbols.json",
          "neither set: the usual lookup (%s)", host_resource("symbols.json").c_str());
}

} // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 3) {
        fprintf(stderr, "usage: %s <fixture.bundle> <empty-fixture.bundle>\n", argv[0]);
        return 2;
    }
    unsetenv("RECOMP_CODE_DIR");
    unsetenv("RECOMP_SYMBOLS");
    std::string pattern = std::string(os_temp_dir()) + "/code-dir-tests-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (os_mkdtemp(buf.data()) != 0) {
        fprintf(stderr, "no temporary directory\n");
        return 2;
    }
    const std::string root = buf.data();
    refusals(root, argv[2]);
    loads(root, argv[1]);
    // Only what this test wrote, newest first, then the root.
    for (auto it = g_written.rbegin(); it != g_written.rend(); ++it)
        if (os_unlink(it->c_str()) != 0)
            os_rmdir(it->c_str());
    os_rmdir(root.c_str());
    printf("code_dir_tests: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
