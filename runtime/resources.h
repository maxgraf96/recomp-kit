// Shared reader for the loaded PE image's resource directory. Returned handles
// and payloads are guest addresses. No module or host pointer is substituted.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ResourceName {
    bool is_string = false;
    uint32_t id = 0;
    std::string name; // UTF-8, decoded from the directory's length-prefixed UTF-16.
};

// Every function takes the module whose resource directory it reads, as a
// module handle: 0 (or the image base) is the executable, otherwise the base of
// a mapped DLL. The variants without `module` read the executable's.
//
// Type and name accept integer IDs, UTF-16 "#123", or UTF-16 names. Return the
// IMAGE_RESOURCE_DATA_ENTRY address, or zero. `language` is a LANGID as
// FindResourceEx takes it: that language if present, else a neutral one, else
// the first. RESOURCE_LOADER_LANGUAGE (the default) is the loader's own
// search, FindResource's and LoadString's: a neutral entry, else the user's
// language (RESOURCE_USER_LANGID), else its primary language, else the first.
// RESOURCE_ANY_LANGUAGE takes the first.
static const uint32_t RESOURCE_ANY_LANGUAGE = 0xffffffffu;
static const uint32_t RESOURCE_LOADER_LANGUAGE = 0xfffffffeu;
// The user's UI language, which the runtime reports as US English
// (GetUserDefaultUILanguage, GetUserDefaultLangID).
static const uint32_t RESOURCE_USER_LANGID = 0x0409u;
uint32_t resource_find_in(uint32_t module, uint32_t type_id_or_name, uint32_t name_id_or_name,
                          uint32_t language = RESOURCE_LOADER_LANGUAGE, std::string *why = nullptr);
uint32_t resource_find(uint32_t type_id_or_name, uint32_t name_id_or_name,
                       std::string *why = nullptr);
uint32_t resource_data_in(uint32_t module, uint32_t entry, uint32_t *size,
                          std::string *why = nullptr);
bool resource_names_in(uint32_t module, uint32_t type_id_or_name, std::vector<ResourceName> *names,
                       std::string *why = nullptr);
// The LANGIDs a resource exists in, in directory order.
bool resource_languages_in(uint32_t module, uint32_t type_id_or_name, uint32_t name_id_or_name,
                           std::vector<uint32_t> *languages, std::string *why = nullptr);
// String `id` of a module's RT_STRING table, as UTF-16 (LoadString's lookup:
// block id/16 + 1, entry id%16, in the loader's language order).
// False when the block or the entry is missing.
bool resource_string_in(uint32_t module, uint32_t id, std::u16string *out,
                        uint32_t language = RESOURCE_LOADER_LANGUAGE);
// Validate a data-entry handle and return its image-backed payload and size.
uint32_t resource_data(uint32_t entry, uint32_t *size, std::string *why = nullptr);
// Snapshot the names before guest callbacks run; no references into the directory
// or temporary guest buffers escape this reader. False explains failure in why.
bool resource_names(uint32_t type_id_or_name, std::vector<ResourceName> *names,
                    std::string *why = nullptr);
