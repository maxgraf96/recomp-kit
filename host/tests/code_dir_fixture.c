/* code_dir_fixture.c - a stand-in for a translated module's bundle
 * (tools/recomp/bundle.py) for code_dir_tests: one translated function that
 * registers itself from a constructor, as a generated table.c does. Built
 * twice: CODE_DIR_FIXTURE_EMPTY makes the bundle that registers nothing. */
#include "runtime/x86.h"

#define FIXTURE_BASE 0x00401000u

static void fixture_fn(X86 *c) {
    c->r[R_EAX] = 0x600dc0deu;
}

static const uint32_t fixture_addrs[1] = {FIXTURE_BASE};
static void (*const fixture_ptrs[1])(X86 *) = {fixture_fn};
static RecompHookFn fixture_hooks[1];
static uint8_t fixture_hooked[1];
static const RecompModule fixture_module = {
    "code-dir-fixture.dll", FIXTURE_BASE, FIXTURE_BASE + 0x1000u, fixture_addrs, 1,
    fixture_ptrs, fixture_hooks, fixture_hooked, 0, 0, 0};

#ifndef CODE_DIR_FIXTURE_EMPTY
__attribute__((constructor)) static void fixture_register(void) {
    recomp_module_register(&fixture_module);
}
#else
/* Referenced so the table is not an unused-variable warning. */
const RecompModule *code_dir_fixture_unregistered(void) {
    return &fixture_module;
}
#endif
