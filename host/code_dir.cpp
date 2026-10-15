// code_dir.cpp - see code_dir.h.
#include "code_dir.h"

#include "controls/json.h"
#include "../platform/os.h"
#include "../runtime/x86.h"

#include <stdio.h>

namespace {

bool read_file(const std::string &path, std::string *text) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text->append(buf, n);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

// A bundle is named by a plain file name inside the code directory: no
// separators, nothing hidden, never "." or "..".
bool plain_name(const std::string &name) {
    if (name.empty() || name[0] == '.')
        return false;
    for (char c : name)
        if (c == '/' || c == '\\' || c == '\0')
            return false;
    return true;
}

} // namespace

bool code_dir_read_order(const std::string &dir, std::vector<std::string> *order,
                         std::string *error) {
    const std::string manifest = dir + "/build.json";
    std::string text;
    if (!read_file(manifest, &text)) {
        *error = "The translated game is missing: there is no " + manifest +
                 ". Build it again (the game's first run).";
        return false;
    }
    controls::Json json;
    std::string why;
    if (!controls::json_parse(text, &json, &why)) {
        *error = manifest + " is not valid JSON (" + why + "). Build the game again.";
        return false;
    }
    const controls::Json *list = json.get("load_order");
    if (!list || list->type != controls::Json::Array || list->a.empty()) {
        *error = manifest + " names no bundles (\"load_order\" is missing or empty).";
        return false;
    }
    order->clear();
    for (const controls::Json &item : list->a) {
        if (item.type != controls::Json::String || !plain_name(item.s)) {
            *error = manifest + ": \"load_order\" must list file names inside " + dir + ".";
            return false;
        }
        order->push_back(item.s);
    }
    return true;
}

bool code_dir_load(const std::string &dir, std::string *error, CodeDirLoad *out) {
    std::vector<std::string> order;
    if (!code_dir_read_order(dir, &order, error))
        return false;
    CodeDirLoad result;
    for (const std::string &name : order) {
        const std::string path = dir + "/" + name;
        OsStat st;
        if (os_stat(path.c_str(), &st) != 0 || !st.is_regular) {
            *error = "The translated game is incomplete: " + path +
                     " is missing. Build it again (the game's first run).";
            return false;
        }
        const uint32_t before = recomp_module_count();
        if (!os_dlopen(path.c_str())) {
            const char *why = os_dlerror();
            *error = "Cannot load " + path + ": " + (why && *why ? why : "unknown error");
            return false;
        }
        const uint32_t after = recomp_module_count();
        if (after == before) {
            *error = path + " registered no translated code: it is not a bundle of this game's "
                            "translation.";
            return false;
        }
        for (uint32_t i = before; i < after; ++i)
            if (const RecompModule *m = recomp_module_at(i))
                result.functions += m->func_count;
        result.modules += after - before;
        result.loaded.push_back(path);
    }
    if (out)
        *out = result;
    return true;
}

bool code_dir_load_env(std::string *error) {
    const char *dir = recomp_env("CODE_DIR");
    if (!dir || !*dir)
        return true;
    CodeDirLoad load;
    if (!code_dir_load(dir, error, &load))
        return false;
    fprintf(stderr, "[host] %s: %zu bundle%s, %u module%s, %u translated functions\n", dir,
            load.loaded.size(), load.loaded.size() == 1 ? "" : "s", load.modules,
            load.modules == 1 ? "" : "s", load.functions);
    return true;
}
