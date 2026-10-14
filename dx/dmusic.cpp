// dmusic.cpp - DirectMusic (DirectX 6.1/7) over GothicKit/dmusic.
//
// A game that plays DirectMusic creates three objects with CoCreateInstance -
// a loader, a composer and a performance - gets an IDirectMusic and a
// synthesizer port from the performance, and from then on talks to these
// interfaces only:
//
//   IDirectMusicLoader        search directories, a cache, GetObject for segments
//   IDirectMusicPerformance   Init, ports, PlaySegment/Stop, the time, global params
//   IDirectMusic, IDirectMusicPort, IKsControl
//                             the software synthesizer: caps, channel groups,
//                             and its volume, effects and reverb as KS properties
//   IDirectMusicSegment, IDirectMusicSegmentState, IDirectMusicStyle
//                             a loaded segment, one play of it, and its style
//   IDirectMusicComposer      AutoTransition, the groove change at a boundary
//
// Everything musical - reading the segment, style, band and DLS files, choosing
// patterns by groove level, transitions, and the synthesizer - is dmusic's
// (third_party/dmusic/README.md). This file is the COM surface over it, the
// file access through the runtime's resolver, and the output stage DirectMusic
// puts behind its port: the port volume and the performance's master volume,
// the Waves reverb (dmusic_reverb.h), and a resampler for the rare host whose
// rate changes after the performance was made.
//
// THREADS
//
// Guest threads call the interfaces, one at a time under the scheduler baton,
// so the object tables here need no lock of their own. The host's audio thread
// calls render() for every block it mixes (host_synth_attach): it holds the
// mixer's lock, then takes g_engines_mutex, then each performance's own lock
// inside dmusic. A guest thread takes g_engines_mutex only to add or remove a
// performance or to change what the output stage reads, never while calling
// the host, so the order is always mixer, engines, dmusic.
//
// WHAT IS APPROXIMATED
//
// - Time. GetTime's reference time is the host's monotonic clock in 100 ns
//   units; its music time runs at 120 beats a minute from Init. A segment's
//   own tempo governs its notes, but the music time a game reads does not
//   follow it. Start times passed to PlaySegment and Stop are not honoured:
//   both act at the boundary their flags name.
// - Notifications. A handle and types are accepted and nothing is ever
//   signalled; GetNotificationPMsg has nothing to give.
// - Secondary segments, motifs, chord maps, tracks and tools are not played.
//   Each is logged once when a game asks for it.
// - The synthesizer is dmusic's TinySoundFont, not Microsoft's DLS synth: it
//   has no 32-voice limit, its envelopes are TinySoundFont's, and it renders at
//   the host's rate. Its output is trimmed by kOutputTrimDb to the level a
//   Windows reference recording measured.
#include "com.h"
#include "dx.h"
#include "dmusic.h"
#include "host_api.h"
#include "dmusic_reverb.h"
#include "../runtime/layout.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../platform/os.h"
#include "game_config.h"

#include <dmusic.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
#include <math.h>
#include <memory>
#include <mutex>
#include <set>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

// ===========================================================================
// GUIDs and constants, from the DirectX 7 SDK headers (dmusicc.h, dmusici.h,
// dmksctrl.h, dmerror.h).
// ===========================================================================
struct Guid {
    uint8_t b[16];
};
constexpr Guid make_guid(uint32_t d1, uint16_t d2, uint16_t d3, uint8_t b0, uint8_t b1, uint8_t b2,
                         uint8_t b3, uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7) {
    return Guid{{(uint8_t)d1, (uint8_t)(d1 >> 8), (uint8_t)(d1 >> 16), (uint8_t)(d1 >> 24),
                 (uint8_t)d2, (uint8_t)(d2 >> 8), (uint8_t)d3, (uint8_t)(d3 >> 8), b0, b1, b2, b3,
                 b4, b5, b6, b7}};
}
// Most DirectMusic GUIDs are {D2AC28xx-B39B-11D1-8704-00600893B1BD}.
constexpr Guid dm_guid(uint8_t low) {
    return make_guid(0xd2ac2800u | low, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1,
                     0xbd);
}

const Guid CLSID_DirectMusicLoader = dm_guid(0x92);
const Guid CLSID_DirectMusicComposer = dm_guid(0x90);
const Guid CLSID_DirectMusicPerformance = dm_guid(0x81);
const Guid CLSID_DirectMusicSegment = dm_guid(0x82);
const Guid CLSID_DirectMusic =
    make_guid(0x636b9f10, 0x0c7d, 0x11d1, 0x95, 0xb2, 0x00, 0x20, 0xaf, 0xdc, 0x74, 0x21);
// The software synthesizer's port class (CLSID_DirectMusicSynth), which is
// the default port GetDefaultPort names.
const Guid CLSID_DirectMusicSynth =
    make_guid(0x58c2b4d0, 0x46e7, 0x11d1, 0x89, 0xac, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29);

const Guid IID_IDirectMusicLoader =
    make_guid(0x2ffaaca2, 0x5dca, 0x11d2, 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6);
const Guid IID_IDirectMusicComposer = dm_guid(0xbf);
const Guid IID_IDirectMusicPerformance =
    make_guid(0x07d43d03, 0x6523, 0x11d2, 0x87, 0x1d, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
const Guid IID_IDirectMusic =
    make_guid(0x6536115a, 0x7b2d, 0x11d2, 0xba, 0x18, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12);
const Guid IID_IDirectMusicPort =
    make_guid(0x08f2d8c9, 0x37c2, 0x11d2, 0xb9, 0xf9, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12);
const Guid IID_IKsControl =
    make_guid(0x28f54685, 0x06fd, 0x11d2, 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
const Guid IID_IDirectMusicSegment =
    make_guid(0xf96029a2, 0x4282, 0x11d2, 0x87, 0x17, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
const Guid IID_IDirectMusicSegmentState =
    make_guid(0xa3afdcc7, 0xd3ee, 0x11d1, 0xbc, 0x8d, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
const Guid IID_IDirectMusicStyle = dm_guid(0xbd);

const Guid GUID_DirectMusicAllTypes = dm_guid(0x93);
const Guid GUID_IDirectMusicStyle = dm_guid(0xa1);
const Guid GUID_Download = dm_guid(0xa7);
const Guid GUID_Unload = dm_guid(0xa8);
const Guid GUID_PerfMasterTempo = dm_guid(0xb0);
const Guid GUID_PerfMasterVolume = dm_guid(0xb1);
const Guid GUID_PerfMasterGrooveLevel = dm_guid(0xb2);
const Guid GUID_PerfAutoDownload =
    make_guid(0xfb09565b, 0x3631, 0x11d2, 0xbc, 0xb8, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
// DirectX 8's names for the same downloads, through an audio path.
const Guid GUID_DownloadToAudioPath =
    make_guid(0x9f2c0341, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);
const Guid GUID_UnloadFromAudioPath =
    make_guid(0x9f2c0342, 0xc5c4, 0x11d3, 0x9b, 0xd1, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00);

const Guid GUID_DMUS_PROP_Volume =
    make_guid(0xfedfae25, 0xe46e, 0x11d1, 0xaa, 0xce, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12);
const Guid GUID_DMUS_PROP_Effects =
    make_guid(0xcda8d611, 0x684a, 0x11d2, 0x87, 0x1e, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
const Guid GUID_DMUS_PROP_WavesReverb =
    make_guid(0x04cb5622, 0x32e5, 0x11d2, 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6);

// HRESULTs: MAKE_HRESULT over FACILITY_DIRECTMUSIC (0x878) with 0x1000 + code.
const uint32_t DMUS_S_PARTIALLOAD = 0x08781091u;
const uint32_t DMUS_E_UNKNOWN_PROPERTY = 0x88781122u;
const uint32_t DMUS_E_SET_UNSUPPORTED = 0x88781123u;
const uint32_t DMUS_E_GET_UNSUPPORTED = 0x88781124u;
const uint32_t DMUS_E_NOT_FOUND = 0x88781193u;
const uint32_t DMUS_E_NOT_INIT = 0x88781194u;
const uint32_t DMUS_E_TYPE_UNSUPPORTED = 0x88781196u;
const uint32_t DMUS_E_LOADER_FAILEDOPEN = 0x887811a3u;
const uint32_t DMUS_E_LOADER_FORMATNOTSUPPORTED = 0x887811a4u;
const uint32_t DMUS_E_LOADER_OBJECTNOTFOUND = 0x887811a6u;
const uint32_t DMUS_E_LOADER_NOFILENAME = 0x887811a7u;

// DMUS_SEGF_* (PlaySegment, Stop) and DMUS_COMPOSEF_* (AutoTransition).
const uint32_t SEGF_SECONDARY = 0x80, SEGF_QUEUE = 0x100, SEGF_GRID = 0x800, SEGF_BEAT = 0x1000,
               SEGF_MEASURE = 0x2000, SEGF_DEFAULT = 0x4000;
const uint32_t COMPOSEF_IMMEDIATE = 0x4, COMPOSEF_GRID = 0x8, COMPOSEF_BEAT = 0x10,
               COMPOSEF_MEASURE = 0x20, COMPOSEF_MODULATE = 0x1000, COMPOSEF_LONG = 0x2000;

// DMUS_OBJECTDESC (dwSize 0x34c in DirectX 7, 0x350 with DirectX 8's pStream).
enum : uint32_t {
    OD_dwSize = 0x00,
    OD_dwValidData = 0x04,
    OD_guidObject = 0x08,
    OD_guidClass = 0x18,
    OD_wszName = 0x38,
    OD_wszFileName = 0x138,
    OD_llMemLength = 0x340,
    OD_pbMemData = 0x348,
    OD_MIN_SIZE = 0x34c,
    OBJ_OBJECT = 1u << 0,
    OBJ_CLASS = 1u << 1,
    OBJ_NAME = 1u << 2,
    OBJ_FILENAME = 1u << 4,
    OBJ_FULLPATH = 1u << 5,
    OBJ_MEMORY = 1u << 10,
};

// DMUS_PORTCAPS and DMUS_PORTPARAMS.
enum : uint32_t {
    PORTCAPS_SIZE = 0x134,
    PC_DLS = 0x1,
    PC_SOFTWARESYNTH = 0x4,
    PC_DIRECTSOUND = 0x80,
    PC_SHAREABLE = 0x100,
    PC_OUTPUTCLASS = 1,
    PORT_USER_MODE_SYNTH = 1,
    PC_SYSTEMMEMORY = 0x7fffffff,
    EFFECT_REVERB = 0x1,
    PORTPARAMS7_SIZE = 0x20,
    PORTPARAMS_VOICES = 0x1,
    PORTPARAMS_CHANNELGROUPS = 0x2,
    PORTPARAMS_AUDIOCHANNELS = 0x4,
    PORTPARAMS_SAMPLERATE = 0x8,
    PORTPARAMS_EFFECTS = 0x20,
    PORTPARAMS_SHARE = 0x40,
    MAX_CHANNEL_GROUPS = 1000,
};

// KSPROPERTY: GUID Set, ULONG Id, ULONG Flags.
const uint32_t KSPROPERTY_TYPE_GET = 0x1, KSPROPERTY_TYPE_SET = 0x2,
               KSPROPERTY_TYPE_BASICSUPPORT = 0x200;

// Music time: DMUS_PPQ ticks per quarter note, and the 120 beats a minute
// GetTime counts in (see the header).
const double kTicksPerSecond = 768.0 * 2.0;

// The synthesizer's output against Windows': a reference recording of one
// game's music at the same volume setting measured dmusic 2.8 dB louder
// (range 1.4-4.7 dB), so its output is taken down by this much.
const float kOutputTrimDb = -3.0f;

bool guid_is(uint32_t addr, const Guid &g) {
    return addr && gm_valid(addr, 16) && memcmp(gm_ptr(addr), g.b, 16) == 0;
}

std::string guid_text(uint32_t addr) {
    if (!addr || !gm_valid(addr, 16))
        return "(null)";
    const uint8_t *g = gm_ptr(addr);
    char s[48];
    snprintf(s, sizeof s, "{%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13],
             g[14], g[15]);
    return s;
}

void wr64(uint32_t addr, int64_t v) {
    wr32(addr, (uint32_t)(uint64_t)v);
    wr32(addr + 4, (uint32_t)((uint64_t)v >> 32));
}

int64_t arg64(X86 *c, int i) {
    return (int64_t)((uint64_t)arg(c, i) | ((uint64_t)arg(c, i + 1) << 32));
}

float mb_to_gain(int32_t mb) {
    if (mb <= -9600)
        return 0.0f;
    return powf(10.0f, (float)mb / 2000.0f);
}

std::string lower(std::string s) {
    for (char &ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
    return s;
}

// The last component of a Windows or POSIX path.
std::string base_name(const std::string &path) {
    size_t i = path.find_last_of("\\/");
    return i == std::string::npos ? path : path.substr(i + 1);
}

bool is_absolute(const std::string &path) {
    return (path.size() > 1 && path[1] == ':') ||
           (!path.empty() && (path[0] == '\\' || path[0] == '/'));
}

std::string join_guest(const std::string &dir, const std::string &name) {
    if (dir.empty())
        return name;
    char last = dir.back();
    return (last == '\\' || last == '/') ? dir + name : dir + "\\" + name;
}

// A whole host file, in memory dmusic will own and free (it frees with free()).
void *read_host_file(const std::string &path, size_t *len) {
    if (path.empty())
        return nullptr;
    int fd = os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0)
        return nullptr;
    OsStat st{};
    if (os_fd_stat(fd, &st) != 0 || !st.is_regular || st.size == 0 || st.size > 0x40000000u) {
        os_fd_close(fd);
        return nullptr;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)st.size);
    size_t got = 0;
    while (buf && got < st.size) {
        int64_t n = os_fd_read(fd, buf + got, (size_t)st.size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    os_fd_close(fd);
    if (!buf || got != st.size) {
        free(buf);
        return nullptr;
    }
    *len = got;
    return buf;
}

bool host_file_exists(const std::string &path) {
    OsStat st{};
    return !path.empty() && os_stat(path.c_str(), &st) == 0 && st.is_regular;
}

// ===========================================================================
// dmusic's log. Warnings about content it only partly understands come once
// per pattern, so by default only errors are let through, each text once;
// RECOMP_DMUSIC_LOG=warn|info|debug|trace lets more through.
// ===========================================================================
void on_dmusic_log(void *, DmLogLevel level, const char *msg) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> held(mutex);
    if (seen.size() < 4096 && !seen.insert(msg).second)
        return;
    log_msg(level <= DmLogLevel_ERROR ? 1 : 2, "dmusic: %s", msg);
}

DmLogLevel dmusic_log_level() {
    const char *e = recomp_env("DMUSIC_LOG");
    if (!e || !*e)
        return DmLogLevel_ERROR;
    const std::string v = lower(e);
    if (v == "warn" || v == "warning")
        return DmLogLevel_WARN;
    if (v == "info")
        return DmLogLevel_INFO;
    if (v == "debug")
        return DmLogLevel_DEBUG;
    if (v == "trace")
        return DmLogLevel_TRACE;
    return DmLogLevel_ERROR;
}

} // namespace

// ===========================================================================
// The General MIDI bank. A band names its GM and GS instruments by collection,
// gm.dls or gs.dls, and Windows answered with its own Roland GM/GS set, which
// cannot be shipped. A game folder that has one is used first; then the bank
// RECOMP_DMUSIC_GM names; then macOS's own Roland GS collection, which is on
// every Mac and never copied; then the kit's GeneralUser GS SoundFont, which
// dmusic plays in place of a DLS collection.
// ===========================================================================
std::string dmusic_gm_bank_path() {
    // An explicit choice stands even when it names nothing: that is how a
    // run asks for no General MIDI bank at all.
    if (const char *e = recomp_env("DMUSIC_GM"))
        if (*e)
            return host_file_exists(e) ? std::string(e) : std::string();
    static const char *const system_banks[] = {
        "/System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls",
    };
    for (const char *path : system_banks)
        if (host_file_exists(path))
            return path;
    const std::string bundled = host_resource("general-midi.sf2");
    if (host_file_exists(bundled))
        return bundled;
    return std::string();
}

namespace {

// ===========================================================================
// The object tables. Keyed by ComObj id; guest threads only.
// ===========================================================================

// What a loader resolves names with. Shared with the segments it loads, which
// download their bands and styles through it later.
struct LoaderData {
    DmLoader *dm = nullptr;
    std::string search_all;                       // GUID_DirectMusicAllTypes
    std::map<std::string, std::string> search_by; // class GUID text -> guest directory
    // A GetObject from memory: the name dmusic is asked for, and the bytes.
    std::string memory_name;
    std::vector<uint8_t> memory_bytes;
    // dmusic_gm_bank_path(), looked up when a band first asks for it.
    std::string gm_bank;
    bool gm_looked = false;
    ~LoaderData() {
        DmLoader_release(dm);
    }
};

struct Loader {
    std::shared_ptr<LoaderData> data;
    bool cache = true;                      // DirectMusic caches every type by default
    std::map<std::string, uint32_t> cached; // name -> segment object id, one reference held
};

// The segh chunk, read from the file itself: what dmusic does not expose.
struct SegmentHeader {
    uint32_t repeats = 0, length = 0, play_start = 0, loop_start = 0, loop_end = 0, resolution = 0;
    bool has_style = false;
};

struct Segment {
    DmSegment *dm = nullptr;
    std::shared_ptr<LoaderData> loader;
    SegmentHeader header;
    std::string name;
    uint32_t style = 0; // the style object GetParam handed out, one reference held
    bool downloaded = false;
};

// One play of a segment.
struct PlayState {
    uint32_t segment = 0; // object id, one reference held
    uint32_t repeats = 0;
    int32_t start = 0; // music time
    uint64_t gen = 0;  // the engine's play generation it belongs to
};

// What the port's KS properties set, read by the output stage under
// g_engines_mutex.
struct PortOut {
    int32_t volume_mb = 0;
    uint32_t effects = EFFECT_REVERB;
    WavesReverbParams reverb;
    uint64_t version = 1;
};

struct Port {
    std::shared_ptr<PortOut> out;
    uint32_t groups = 1;
    uint32_t voices = 32, audio_channels = 2, sample_rate = 22050;
    bool active = false;
};

// The synthesizer for one performance, and the output stage behind it. The
// fields marked "render" belong to the audio thread; the rest are written by
// guest threads under g_engines_mutex.
struct Engine {
    DmPerformance *dm = nullptr;
    double dm_rate = 44100.0;
    std::shared_ptr<PortOut> out;
    int32_t master_volume_mb = 0;
    // Whether what was last played is still playing, from the render side:
    // play_gen counts plays and stops (guest), seen_gen is the last generation
    // during which dmusic was playing and ended_gen the last one in which it
    // then stopped (render).
    std::atomic<uint64_t> play_gen{0}, seen_gen{0}, ended_gen{0};
    std::atomic<uint64_t> frames_rendered{0};
    // render
    float gain = 0.0f;
    bool gain_set = false;
    WavesReverb reverb;
    uint64_t reverb_version = 0;
    double reverb_rate = 0.0;
    double phase = 0.0;
    float last[2] = {0.0f, 0.0f};
    std::vector<float> scratch, left, right;

    ~Engine() {
        DmPerformance_release(dm);
    }
};

struct Performance {
    std::shared_ptr<Engine> engine;
    uint32_t directmusic = 0; // IDirectMusic object, one reference held
    uint32_t directsound =
        0; // the IDirectSound it was given (or the shared one), one reference held
    std::vector<uint32_t> ports;
    bool initialised = false, closed = false;
    bool auto_download = false;
    int8_t master_groove = 0;
    float master_tempo = 1.0f;
    std::map<std::string, std::vector<uint8_t>> globals; // every other global parameter
    uint32_t primary = 0; // segment state object of the primary segment, one reference held
    uint32_t prepare_ms = 1000, bumper_ms = 50;
    uint64_t start_ns = 0;
};

struct DirectMusicObj {
    bool active = false;
    uint32_t directsound = 0; // SetDirectSound's, one reference held
};

std::map<uint32_t, Loader> g_loaders;
std::map<uint32_t, Segment> g_segments;
std::map<uint32_t, PlayState> g_states;
std::map<uint32_t, Performance> g_perfs;
std::map<uint32_t, Port> g_ports;
std::map<uint32_t, DirectMusicObj> g_dms;
std::map<uint32_t, uint32_t> g_styles; // style object -> its segment object

std::mutex g_engines_mutex;
std::vector<std::shared_ptr<Engine>> g_engines; // g_engines_mutex
bool g_attached = false;
int g_enabled_override = -1;

template <class T> T *find(std::map<uint32_t, T> &m, ComObj *o) {
    if (!o)
        return nullptr;
    auto it = m.find(o->id);
    return it == m.end() ? nullptr : &it->second;
}

// The object behind a guest interface pointer of a given interface, or null.
ComObj *object_at(uint32_t addr, ComIface iface) {
    return com_this(addr, iface);
}

// A view of `o` handed to the guest through an out pointer, with a reference.
bool hand_out(uint32_t out, ComObj *o, ComIface iface) {
    uint32_t view = com_view(o, iface);
    if (!view || !com_out_ptr(out, view))
        return false;
    com_addref(o);
    return true;
}

void release_id(uint32_t id) {
    if (ComObj *o = com_get(id))
        com_release(o);
}

// The DirectSound a performance or IDirectMusic is handed, held as DirectMusic
// holds it: one reference. Null means the one the audio libraries share
// (dsound_shared_device), which is what DirectMusic would otherwise open for
// itself. The synthesizer does not play through it: the host mixes its output.
uint32_t hold_directsound(uint32_t guest_pointer) {
    ComObj *ds = com_this(guest_pointer ? guest_pointer : dsound_shared_device(), IF_NONE);
    if (!ds)
        return 0;
    com_addref(ds);
    return ds->id;
}

// ===========================================================================
// The loader's resolver: what dmusic calls for a segment, a style, a band's
// DLS collection. Every name goes through the runtime's file resolver (its
// overlays, drive mapping and case folding), so "Data\Music\Foo.sgt" finds
// data/music/foo.sgt.
// ===========================================================================
void *resolve(void *context, const char *name, size_t *len) {
    LoaderData *data = (LoaderData *)context;
    const std::string want = name ? name : "";
    if (want.empty())
        return nullptr;
    if (!data->memory_name.empty() && want == data->memory_name) {
        void *buf = malloc(data->memory_bytes.size());
        if (!buf)
            return nullptr;
        memcpy(buf, data->memory_bytes.data(), data->memory_bytes.size());
        *len = data->memory_bytes.size();
        return buf;
    }
    std::vector<std::string> candidates;
    if (is_absolute(want)) {
        candidates.push_back(want);
    } else {
        if (!data->search_all.empty())
            candidates.push_back(join_guest(data->search_all, want));
        for (const auto &dir : data->search_by)
            candidates.push_back(join_guest(dir.second, want));
        candidates.push_back(want); // the current directory
    }
    for (const std::string &guest : candidates) {
        std::string host = win32_host_path_op(guest, WIN32_FILE_READ);
        if (void *buf = read_host_file(host, len))
            return buf;
    }
    // A collection Windows itself supplied.
    const std::string file = lower(base_name(want));
    if (file == "gm.dls" || file == "gs.dls" || file == "xg.dls") {
        std::string &bank = data->gm_bank;
        if (!data->gm_looked) {
            data->gm_looked = true;
            bank = dmusic_gm_bank_path();
            if (bank.empty())
                log_once("dmusic.gm", "dmusic: no General MIDI bank was found (no gm.dls, no "
                                      "system GS collection, no general-midi.sf2): instruments "
                                      "a band takes from it are silent");
            else
                log_once("dmusic.gm", "dmusic: General MIDI instruments come from %s",
                         bank.c_str());
        }
        if (void *buf = read_host_file(bank, len))
            return buf;
    }
    return nullptr;
}

// Reads the segment header and whether there is a style track, from the
// segment file's top level and its track list.
SegmentHeader parse_segment_header(const uint8_t *p, size_t n) {
    SegmentHeader h;
    auto u32 = [&](size_t at) -> uint32_t {
        uint32_t v = 0;
        if (at + 4 <= n)
            memcpy(&v, p + at, 4);
        return v;
    };
    if (n < 12 || memcmp(p, "RIFF", 4) != 0)
        return h;
    for (size_t at = 12; at + 8 <= n;) {
        uint32_t size = u32(at + 4);
        if (memcmp(p + at, "segh", 4) == 0 && size >= 24) {
            h.repeats = u32(at + 8);
            h.length = u32(at + 12);
            h.play_start = u32(at + 16);
            h.loop_start = u32(at + 20);
            h.loop_end = u32(at + 24);
            h.resolution = size >= 24 ? u32(at + 28) : 0;
        }
        if (size > n)
            break;
        at += 8 + size + (size & 1);
    }
    // A style track is a LIST 'sttr' somewhere in a track: a byte search is
    // enough, for a four-character code that only names that.
    for (size_t i = 0; i + 12 <= n; ++i)
        if (memcmp(p + i, "LIST", 4) == 0 && memcmp(p + i + 8, "sttr", 4) == 0) {
            h.has_style = true;
            break;
        }
    return h;
}

// ===========================================================================
// The output stage, on the audio thread.
// ===========================================================================

// Renders `n` frames of dmusic into e.scratch, interleaved. Render side.
void render_dmusic(Engine &e, uint32_t n) {
    e.scratch.assign((size_t)n * 2, 0.0f);
    if (n)
        DmPerformance_renderPcm(e.dm, e.scratch.data(), (size_t)n * 2,
                                (DmRenderOptions)(DmRender_FLOAT | DmRender_STEREO));
    e.frames_rendered.fetch_add(n, std::memory_order_relaxed);
}

// One engine's block, added into the mix: dmusic at its own rate, resampled
// to the host's if they differ, then gain, then the reverb. g_engines_mutex held.
void render_engine(Engine &e, float *left, float *right, uint32_t frames, double rate) {
    e.left.assign(frames, 0.0f);
    e.right.assign(frames, 0.0f);
    if (fabs(e.dm_rate - rate) < 0.5) {
        render_dmusic(e, frames);
        for (uint32_t i = 0; i < frames; ++i) {
            e.left[i] = e.scratch[2 * i];
            e.right[i] = e.scratch[2 * i + 1];
        }
    } else {
        // Linear interpolation between the previous block's last frame and
        // this one's: only reached when the host's rate changed after the
        // performance was made.
        const double step = e.dm_rate / rate;
        const double end = e.phase + frames * step;
        const uint32_t n = (uint32_t)end;
        render_dmusic(e, n);
        auto src = [&](uint32_t j, int ch) -> float {
            return j == 0 ? e.last[ch] : e.scratch[2 * (j - 1) + ch];
        };
        for (uint32_t i = 0; i < frames; ++i) {
            const double x = e.phase + i * step;
            const uint32_t j = (uint32_t)x;
            const float f = (float)(x - j);
            e.left[i] = src(j, 0) * (1.0f - f) + src(j + 1, 0) * f;
            e.right[i] = src(j, 1) * (1.0f - f) + src(j + 1, 1) * f;
        }
        if (n) {
            e.last[0] = e.scratch[2 * (n - 1)];
            e.last[1] = e.scratch[2 * (n - 1) + 1];
        }
        e.phase = end - n;
    }

    // Whether the segment the guest last played is still playing.
    const uint64_t gen = e.play_gen.load(std::memory_order_acquire);
    if (DmPerformance_isPlaying(e.dm))
        e.seen_gen.store(gen, std::memory_order_release);
    else if (gen && e.seen_gen.load(std::memory_order_acquire) == gen)
        e.ended_gen.store(gen, std::memory_order_release);

    // The port's volume and the performance's master volume, ramped over
    // about 5 ms so a game's fade in steps is a fade and not a staircase.
    const PortOut &out = *e.out;
    const float target = mb_to_gain(out.volume_mb) * mb_to_gain(e.master_volume_mb) *
                         powf(10.0f, kOutputTrimDb / 20.0f);
    if (!e.gain_set) {
        e.gain = target;
        e.gain_set = true;
    }
    const float k = 1.0f - expf(-1.0f / (0.005f * (float)rate));
    for (uint32_t i = 0; i < frames; ++i) {
        e.gain += (target - e.gain) * k;
        e.left[i] *= e.gain;
        e.right[i] *= e.gain;
    }

    if (out.effects & EFFECT_REVERB) {
        if (e.reverb_version != out.version || e.reverb_rate != rate) {
            e.reverb.configure(rate, out.reverb);
            e.reverb_version = out.version;
            e.reverb_rate = rate;
        }
        e.reverb.process(e.left.data(), e.right.data(), frames);
    }

    for (uint32_t i = 0; i < frames; ++i) {
        left[i] += e.left[i];
        right[i] += e.right[i];
    }
}

void render(void *, float *left, float *right, uint32_t frames, double rate) {
    if (rate <= 0.0)
        return;
    std::lock_guard<std::mutex> held(g_engines_mutex);
    for (const std::shared_ptr<Engine> &e : g_engines)
        render_engine(*e, left, right, frames, rate);
}

void attach_once() {
    if (g_attached)
        return;
    g_attached = true;
    host_synth_attach(render, nullptr);
}

void engine_add(const std::shared_ptr<Engine> &e) {
    {
        std::lock_guard<std::mutex> held(g_engines_mutex);
        g_engines.push_back(e);
    }
    attach_once();
}

void engine_remove(const std::shared_ptr<Engine> &e) {
    std::lock_guard<std::mutex> held(g_engines_mutex);
    g_engines.erase(std::remove(g_engines.begin(), g_engines.end(), e), g_engines.end());
}

} // namespace

namespace {

// ===========================================================================
// Shared pieces of the methods.
// ===========================================================================

// A fresh object's creation reference goes to the caller with its view.
bool give(uint32_t out, ComObj *o, ComIface iface) {
    uint32_t view = com_view(o, iface);
    if (!view || !com_out_ptr(out, view)) {
        com_release(o);
        return false;
    }
    return true;
}

bool out_ok(uint32_t out, uint32_t bytes = 4) {
    return out && gm_valid(out, bytes);
}

// The music time now, counted from the performance's Init.
int32_t music_time(const Performance &p) {
    const uint64_t ns = os_monotonic_ns() - p.start_ns;
    return (int32_t)(uint32_t)(int64_t)((double)ns * 1e-9 * kTicksPerSecond);
}

int64_t reference_time() {
    return (int64_t)(os_monotonic_ns() / 100);
}

// The boundary a play or stop waits for: the flags' own, or with
// DMUS_SEGF_DEFAULT the segment's default resolution.
DmTiming timing_for(uint32_t flags, uint32_t resolution) {
    if (flags & SEGF_DEFAULT)
        flags = resolution;
    if (flags & SEGF_MEASURE)
        return DmTiming_MEASURE;
    if (flags & SEGF_BEAT)
        return DmTiming_BEAT;
    if (flags & SEGF_GRID)
        return DmTiming_GRID;
    return DmTiming_INSTANT;
}

// Downloads a segment's bands and style through the loader that made it.
// A partial download (a collection that cannot be found) is not an error to
// the game: DirectMusic plays what it has.
void download(Segment &s) {
    if (s.downloaded)
        return;
    DmResult r = DmSegment_download(s.dm, s.loader->dm);
    if (r != DmResult_SUCCESS)
        log_once(("dmusic.download." + s.name).c_str(),
                 "dmusic: %s downloaded only in part (dmusic result %d)", s.name.c_str(), (int)r);
    s.downloaded = true;
}

// The primary segment's state, if what it plays has not ended.
PlayState *primary_playing(Performance &p) {
    if (!p.primary)
        return nullptr;
    auto it = g_states.find(p.primary);
    if (it == g_states.end())
        return nullptr;
    const Engine &e = *p.engine;
    const uint64_t gen = it->second.gen;
    if (e.play_gen.load(std::memory_order_acquire) != gen ||
        e.ended_gen.load(std::memory_order_acquire) == gen) {
        release_id(p.primary);
        p.primary = 0;
        return nullptr;
    }
    return &it->second;
}

// A new segment state for `seg`, which becomes the primary segment of `p`.
// Returns the state object (its creation reference is the performance's).
ComObj *start_primary(Performance &p, ComObj *seg_obj, Segment &seg) {
    ComObj *st = com_new(K_DMSEGMENTSTATE);
    PlayState ps;
    ps.segment = seg_obj->id;
    com_addref(seg_obj);
    ps.repeats = DmSegment_getRepeats(seg.dm);
    ps.start = music_time(p);
    ps.gen = p.engine->play_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_states[st->id] = ps;
    if (p.primary)
        release_id(p.primary);
    p.primary = st->id;
    return st;
}

void stop_primary(Performance &p, DmTiming timing) {
    DmPerformance_playSegment(p.engine->dm, nullptr, timing);
    p.engine->play_gen.fetch_add(1, std::memory_order_acq_rel);
    if (p.primary) {
        release_id(p.primary);
        p.primary = 0;
    }
}

// ===========================================================================
// IDirectMusicLoader
// ===========================================================================
void clear_cache(Loader &l) {
    std::map<std::string, uint32_t> cached;
    cached.swap(l.cached);
    for (const auto &entry : cached)
        release_id(entry.second);
}

// GetObject(pDesc, riid, ppv): a segment, by file name relative to the search
// directory, by full path, or from memory.
void Loader_GetObject(X86 *c) {
    Loader *l = find(g_loaders, com_this_arg(c, IF_DMLOADER));
    const uint32_t desc = arg(c, 1), riid = arg(c, 2), out = arg(c, 3);
    if (!out_ok(out) || !l) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (!desc || !gm_valid(desc, OD_MIN_SIZE) || rd32(desc + OD_dwSize) < OD_MIN_SIZE) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    const uint32_t valid = rd32(desc + OD_dwValidData);
    const bool segment = (valid & OBJ_CLASS)
                             ? guid_is(desc + OD_guidClass, CLSID_DirectMusicSegment)
                             : guid_is(riid, IID_IDirectMusicSegment);
    if (!segment) {
        log_once(("dmusic.class." + guid_text(desc + OD_guidClass)).c_str(),
                 "dmusic: the loader makes segments only; class %s is not loaded",
                 guid_text(desc + OD_guidClass).c_str());
        com_ret(c, DMUS_E_LOADER_FORMATNOTSUPPORTED);
        return;
    }
    static const uint8_t iid_unknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46};
    if (!guid_is(riid, IID_IDirectMusicSegment) &&
        !(riid && gm_valid(riid, 16) && memcmp(gm_ptr(riid), iid_unknown, 16) == 0)) {
        com_ret(c, E_NOINTERFACE);
        return;
    }

    // What to read.
    std::string name;
    std::vector<uint8_t> bytes;
    const bool memory = (valid & OBJ_MEMORY) != 0;
    if (memory) {
        const uint32_t mem = rd32(desc + OD_pbMemData);
        const uint32_t n = rd32(desc + OD_llMemLength);
        if (!mem || !n || rd32(desc + OD_llMemLength + 4) || !gm_valid(mem, n)) {
            com_ret(c, E_INVALIDARG);
            return;
        }
        bytes.assign(gm_ptr(mem), gm_ptr(mem) + n);
        char tag[32];
        snprintf(tag, sizeof tag, "<memory %08x>", mem);
        name = tag;
    } else if (valid & (OBJ_FILENAME | OBJ_FULLPATH)) {
        name = gm_wstr(desc + OD_wszFileName, 260);
    } else if (valid & (OBJ_NAME | OBJ_OBJECT)) {
        log_once("dmusic.byname", "dmusic: GetObject by name or GUID alone is not supported");
        com_ret(c, DMUS_E_LOADER_OBJECTNOTFOUND);
        return;
    }
    if (name.empty()) {
        com_ret(c, DMUS_E_LOADER_NOFILENAME);
        return;
    }

    const std::string key = lower(name);
    if (l->cache && !memory) {
        auto it = l->cached.find(key);
        if (it != l->cached.end()) {
            if (ComObj *o = com_get(it->second)) {
                com_ret(c, hand_out(out, o, IF_DMSEGMENT) ? S_OK : E_OUTOFMEMORY);
                return;
            }
            l->cached.erase(it);
        }
    }

    if (!memory) {
        size_t len = 0;
        void *buf = resolve(l->data.get(), name.c_str(), &len);
        if (!buf) {
            LOGV("dmusic: GetObject(%s): not found", name.c_str());
            com_ret(c, DMUS_E_LOADER_FAILEDOPEN);
            return;
        }
        bytes.assign((uint8_t *)buf, (uint8_t *)buf + len);
        free(buf);
    }

    // dmusic reads it through the resolver, which answers this name with these bytes.
    Segment seg;
    seg.header = parse_segment_header(bytes.data(), bytes.size());
    seg.loader = l->data;
    seg.name = base_name(name);
    l->data->memory_name = name;
    l->data->memory_bytes.swap(bytes);
    DmResult r = DmLoader_getSegment(l->data->dm, name.c_str(), &seg.dm);
    l->data->memory_name.clear();
    l->data->memory_bytes.clear();
    if (r != DmResult_SUCCESS || !seg.dm) {
        log_once(("dmusic.load." + key).c_str(), "dmusic: %s is not a segment dmusic can read (%d)",
                 name.c_str(), (int)r);
        com_ret(c,
                r == DmResult_MEMORY_EXHAUSTED ? E_OUTOFMEMORY : DMUS_E_LOADER_FORMATNOTSUPPORTED);
        return;
    }
    ComObj *o = com_new(K_DMSEGMENT);
    g_segments[o->id] = seg;
    LOGV("dmusic: GetObject(%s) -> segment %u (length %u, repeats %u, resolution %#x)",
         name.c_str(), o->id, seg.header.length, seg.header.repeats, seg.header.resolution);
    if (l->cache && !memory) {
        l->cached[key] = o->id;
        com_addref(o);
    }
    com_ret(c, give(out, o, IF_DMSEGMENT) ? S_OK : E_OUTOFMEMORY);
}

void Loader_SetObject(X86 *c) {
    log_once("dmusic.setobject", "dmusic: IDirectMusicLoader::SetObject is accepted and ignored");
    com_ret(c, S_OK);
}

// SetSearchDirectory(rguidClass, pwzPath, fClear)
void Loader_SetSearchDirectory(X86 *c) {
    Loader *l = find(g_loaders, com_this_arg(c, IF_DMLOADER));
    const uint32_t cls = arg(c, 1), path = arg(c, 2), clear = arg(c, 3);
    if (!l || !path || !gm_valid(path, 2)) {
        com_ret(c, E_POINTER);
        return;
    }
    const std::string dir = gm_wstr(path, 260);
    // DirectMusic refuses a directory that does not exist, and a game may
    // fall back to another on that refusal.
    const std::string host = win32_host_path_op(dir, WIN32_FILE_READ);
    OsStat st{};
    if (host.empty() || os_stat(host.c_str(), &st) != 0 || !st.is_dir) {
        LOGV("dmusic: SetSearchDirectory(%s): no such directory", dir.c_str());
        com_ret(c, 0x887811a2u); // DMUS_E_LOADER_BADPATH
        return;
    }
    std::string &slot = guid_is(cls, GUID_DirectMusicAllTypes) || !cls
                            ? l->data->search_all
                            : l->data->search_by[guid_text(cls)];
    const bool same = slot == dir;
    slot = dir;
    if (clear)
        clear_cache(*l);
    LOGV("dmusic: SetSearchDirectory(%s, %s)", guid_text(cls).c_str(), dir.c_str());
    com_ret(c, same ? S_FALSE : S_OK);
}

void Loader_ScanDirectory(X86 *c) {
    log_once("dmusic.scan", "dmusic: IDirectMusicLoader::ScanDirectory finds nothing here");
    com_ret(c, S_FALSE);
}

// CacheObject(pObject): keeps a segment this loader made.
void Loader_CacheObject(X86 *c) {
    Loader *l = find(g_loaders, com_this_arg(c, IF_DMLOADER));
    ComObj *o = object_at(arg(c, 1), IF_DMSEGMENT);
    Segment *s = find(g_segments, o);
    if (!l || !s) {
        com_ret(c, E_POINTER);
        return;
    }
    const std::string key = lower(s->name);
    if (l->cached.count(key)) {
        com_ret(c, S_FALSE);
        return;
    }
    l->cached[key] = o->id;
    com_addref(o);
    com_ret(c, S_OK);
}

// ReleaseObject(pObject): forgets one cached object.
void Loader_ReleaseObject(X86 *c) {
    Loader *l = find(g_loaders, com_this_arg(c, IF_DMLOADER));
    ComObj *o = com_this(arg(c, 1));
    if (!l || !o) {
        com_ret(c, E_POINTER);
        return;
    }
    for (auto it = l->cached.begin(); it != l->cached.end(); ++it) {
        if (it->second == o->id) {
            l->cached.erase(it);
            com_release(o);
            com_ret(c, S_OK);
            return;
        }
    }
    com_ret(c, S_FALSE);
}

void Loader_ClearCache(X86 *c) {
    Loader *l = find(g_loaders, com_this_arg(c, IF_DMLOADER));
    if (l)
        clear_cache(*l);
    com_ret(c, l ? S_OK : E_POINTER);
}

// EnableCache(rguidClass, fEnable): only segments are cached, whatever the class.
void Loader_EnableCache(X86 *c) {
    Loader *l = find(g_loaders, com_this_arg(c, IF_DMLOADER));
    if (!l) {
        com_ret(c, E_POINTER);
        return;
    }
    const bool enable = arg(c, 2) != 0;
    const bool same = l->cache == enable;
    l->cache = enable;
    if (!enable)
        clear_cache(*l);
    com_ret(c, same ? S_FALSE : S_OK);
}

void Loader_EnumObject(X86 *c) {
    com_ret(c, S_FALSE);
}

ComObj *loader_create() {
    ComObj *o = com_new(K_DMLOADER);
    Loader &l = g_loaders[o->id];
    l.data = std::make_shared<LoaderData>();
    if (DmLoader_create(&l.data->dm, DmLoader_DEFAULT) != DmResult_SUCCESS) {
        g_loaders.erase(o->id);
        com_destroy(o);
        return nullptr;
    }
    DmLoader_addResolver(l.data->dm, resolve, l.data.get());
    return o;
}

void loader_destroy(ComObj *o) {
    auto it = g_loaders.find(o->id);
    if (it == g_loaders.end())
        return;
    clear_cache(it->second);
    g_loaders.erase(o->id);
}

// ===========================================================================
// IDirectMusicSegment
// ===========================================================================
Segment *this_segment(X86 *c) {
    return find(g_segments, com_this_arg(c, IF_DMSEGMENT));
}

void Segment_GetLength(X86 *c) {
    Segment *s = this_segment(c);
    if (!s || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), s->header.length);
    com_ret(c, S_OK);
}

void Segment_SetLength(X86 *c) {
    Segment *s = this_segment(c);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    log_once("dmusic.setlength", "dmusic: IDirectMusicSegment::SetLength is recorded, not played");
    s->header.length = arg(c, 1);
    com_ret(c, S_OK);
}

void Segment_GetRepeats(X86 *c) {
    Segment *s = this_segment(c);
    if (!s || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), DmSegment_getRepeats(s->dm));
    com_ret(c, S_OK);
}

void Segment_SetRepeats(X86 *c) {
    Segment *s = this_segment(c);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    DmSegment_setRepeats(s->dm, arg(c, 1));
    com_ret(c, S_OK);
}

void Segment_GetDefaultResolution(X86 *c) {
    Segment *s = this_segment(c);
    if (!s || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), s->header.resolution);
    com_ret(c, S_OK);
}

void Segment_SetDefaultResolution(X86 *c) {
    Segment *s = this_segment(c);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    s->header.resolution = arg(c, 1);
    com_ret(c, S_OK);
}

// GetTrack(rguidType, dwGroupBits, dwIndex, ppTrack) and the other track
// calls: dmusic does not expose its tracks.
void Segment_GetTrack(X86 *c) {
    if (out_ok(arg(c, 4)))
        wr32(arg(c, 4), 0);
    log_once("dmusic.tracks", "dmusic: a segment's tracks are not available as objects");
    com_ret(c, DMUS_E_NOT_FOUND);
}
void Segment_GetTrackGroup(X86 *c) {
    com_ret(c, DMUS_E_NOT_FOUND);
}
void Segment_TrackNotImpl(X86 *c) {
    log_once("dmusic.tracks.edit", "dmusic: a segment's tracks cannot be changed");
    com_ret(c, E_NOTIMPL);
}

void Segment_InitPlay(X86 *c) {
    log_once("dmusic.initplay", "dmusic: IDirectMusicSegment::InitPlay is not implemented");
    com_ret(c, E_NOTIMPL);
}

void Segment_GetGraph(X86 *c) {
    if (out_ok(arg(c, 1)))
        wr32(arg(c, 1), 0);
    com_ret(c, DMUS_E_NOT_FOUND);
}

void Segment_OkNoop(X86 *c) {
    com_ret(c, S_OK);
}

// GetParam(rguidType, dwGroupBits, dwIndex, mtTime, pmtNext, pParam): the
// segment's style, which is all a style-based segment is asked for.
void Segment_GetParam(X86 *c) {
    ComObj *so = com_this_arg(c, IF_DMSEGMENT);
    Segment *s = find(g_segments, so);
    const uint32_t type = arg(c, 1), next = arg(c, 5), param = arg(c, 6);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    if (out_ok(next))
        wr32(next, 0);
    if (guid_is(type, GUID_IDirectMusicStyle)) {
        if (!out_ok(param)) {
            com_ret(c, E_POINTER);
            return;
        }
        wr32(param, 0);
        if (!s->header.has_style) {
            com_ret(c, DMUS_E_NOT_FOUND);
            return;
        }
        ComObj *style = s->style ? com_get(s->style) : nullptr;
        if (!style) {
            style = com_new(K_DMSTYLE);
            g_styles[style->id] = so->id;
            s->style = style->id; // the segment's reference
        }
        com_ret(c, hand_out(param, style, IF_DMSTYLE) ? S_OK : E_OUTOFMEMORY);
        return;
    }
    log_once(("dmusic.getparam." + guid_text(type)).c_str(),
             "dmusic: IDirectMusicSegment::GetParam %s is not available", guid_text(type).c_str());
    com_ret(c, DMUS_E_GET_UNSUPPORTED);
}

// SetParam(rguidType, dwGroupBits, dwIndex, mtTime, pParam): the downloads.
// Unloading leaves dmusic's instruments where they are: they are the
// synthesizer's, keyed by collection, and cost nothing until played.
void Segment_SetParam(X86 *c) {
    Segment *s = this_segment(c);
    const uint32_t type = arg(c, 1);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    if (guid_is(type, GUID_Download) || guid_is(type, GUID_DownloadToAudioPath)) {
        download(*s);
        com_ret(c, S_OK);
        return;
    }
    if (guid_is(type, GUID_Unload) || guid_is(type, GUID_UnloadFromAudioPath)) {
        com_ret(c, S_OK);
        return;
    }
    log_once(("dmusic.setparam." + guid_text(type)).c_str(),
             "dmusic: IDirectMusicSegment::SetParam %s is accepted and ignored",
             guid_text(type).c_str());
    com_ret(c, S_OK);
}

void Segment_Clone(X86 *c) {
    if (out_ok(arg(c, 3)))
        wr32(arg(c, 3), 0);
    log_once("dmusic.clone", "dmusic: IDirectMusicSegment::Clone is not implemented");
    com_ret(c, E_NOTIMPL);
}

void Segment_SetStartPoint(X86 *c) {
    Segment *s = this_segment(c);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    log_once("dmusic.startpoint", "dmusic: a segment's start point is recorded, not played");
    s->header.play_start = arg(c, 1);
    com_ret(c, S_OK);
}

void Segment_GetStartPoint(X86 *c) {
    Segment *s = this_segment(c);
    if (!s || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), s->header.play_start);
    com_ret(c, S_OK);
}

void Segment_SetLoopPoints(X86 *c) {
    Segment *s = this_segment(c);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    log_once("dmusic.looppoints", "dmusic: a segment's loop points are recorded, not played");
    s->header.loop_start = arg(c, 1);
    s->header.loop_end = arg(c, 2);
    com_ret(c, S_OK);
}

void Segment_GetLoopPoints(X86 *c) {
    Segment *s = this_segment(c);
    if (!s || !out_ok(arg(c, 1)) || !out_ok(arg(c, 2))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), s->header.loop_start);
    wr32(arg(c, 2), s->header.loop_end);
    com_ret(c, S_OK);
}

void segment_destroy(ComObj *o) {
    auto it = g_segments.find(o->id);
    if (it == g_segments.end())
        return;
    const uint32_t style = it->second.style;
    DmSegment_release(it->second.dm);
    g_segments.erase(it);
    if (style)
        release_id(style);
}

// ===========================================================================
// IDirectMusicSegmentState
// ===========================================================================
PlayState *this_state(X86 *c) {
    return find(g_states, com_this_arg(c, IF_DMSEGMENTSTATE));
}

void State_GetRepeats(X86 *c) {
    PlayState *st = this_state(c);
    if (!st || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), st->repeats);
    com_ret(c, S_OK);
}

// GetSegment(ppSegment): the same interface pointer the loader handed out.
void State_GetSegment(X86 *c) {
    PlayState *st = this_state(c);
    const uint32_t out = arg(c, 1);
    if (!st || !out_ok(out)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    ComObj *seg = com_get(st->segment);
    com_ret(c, seg && hand_out(out, seg, IF_DMSEGMENT) ? S_OK : DMUS_E_NOT_FOUND);
}

void State_GetStartTime(X86 *c) {
    PlayState *st = this_state(c);
    if (!st || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), (uint32_t)st->start);
    com_ret(c, S_OK);
}

void State_GetZero(X86 *c) {
    if (!this_state(c) || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), 0);
    com_ret(c, S_OK);
}

void state_destroy(ComObj *o) {
    auto it = g_states.find(o->id);
    if (it == g_states.end())
        return;
    const uint32_t seg = it->second.segment;
    g_states.erase(it);
    release_id(seg);
}

// ===========================================================================
// IDirectMusicStyle: a style-based segment's style, asked for its motifs.
// dmusic plays neither motifs nor a style on its own, so it has none.
// ===========================================================================
void Style_EnumNone(X86 *c) {
    com_ret(c, S_FALSE);
}

void Style_GetNone(X86 *c) {
    // GetBand(wszName, ppBand), GetMotif(wszName, ppSegment), GetChordMap(wszName, ppChordMap)
    if (out_ok(arg(c, 2)))
        wr32(arg(c, 2), 0);
    log_once("dmusic.style.get",
             "dmusic: a style's bands, motifs and chord maps are not available");
    com_ret(c, DMUS_E_NOT_FOUND);
}

void Style_GetDefaultNone(X86 *c) {
    if (out_ok(arg(c, 1)))
        wr32(arg(c, 1), 0);
    com_ret(c, DMUS_E_NOT_FOUND);
}

void Style_NotImpl(X86 *c) {
    log_once("dmusic.style.info", "dmusic: a style's time signature, tempo and embellishment "
                                  "lengths are not available");
    com_ret(c, E_NOTIMPL);
}

void style_destroy(ComObj *o) {
    g_styles.erase(o->id);
}

// ===========================================================================
// IDirectMusicComposer
// ===========================================================================

// AutoTransition(pPerformance, pToSeg, wCommand, dwFlags, pChordMap,
// ppTransSeg, ppToSegState, ppTransSegState): a transition pattern from the
// playing style at the next boundary, then the new segment.
void Composer_AutoTransition(X86 *c) {
    Performance *p = find(g_perfs, object_at(arg(c, 1), IF_DMPERFORMANCE));
    ComObj *so = arg(c, 2) ? object_at(arg(c, 2), IF_DMSEGMENT) : nullptr;
    Segment *s = find(g_segments, so);
    const uint32_t command = arg(c, 3) & 0xffff, flags = arg(c, 4);
    const uint32_t trans_seg = arg(c, 6), to_state = arg(c, 7), trans_state = arg(c, 8);
    for (uint32_t out : {trans_seg, to_state, trans_state})
        if (out_ok(out))
            wr32(out, 0);
    if (!p || (arg(c, 2) && !s)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (!p->initialised || p->closed) {
        com_ret(c, DMUS_E_NOT_INIT);
        return;
    }
    static const DmEmbellishmentType kCommand[6] = {
        DmEmbellishment_GROOVE, DmEmbellishment_FILL, DmEmbellishment_INTRO,
        DmEmbellishment_BREAK,  DmEmbellishment_END,  DmEmbellishment_END_AND_INTRO};
    const DmEmbellishmentType embellishment =
        command < 6 ? kCommand[command] : DmEmbellishment_GROOVE;
    DmTiming timing = DmTiming_MEASURE;
    if (flags & COMPOSEF_IMMEDIATE)
        timing = DmTiming_INSTANT;
    else if (flags & COMPOSEF_GRID)
        timing = DmTiming_GRID;
    else if (flags & COMPOSEF_BEAT)
        timing = DmTiming_BEAT;
    if (flags & (COMPOSEF_MODULATE | COMPOSEF_LONG))
        log_once("dmusic.transition.flags", "dmusic: AutoTransition's modulate and long "
                                            "transitions play as one-measure transitions");
    if (trans_seg)
        log_once("dmusic.transition.seg", "dmusic: AutoTransition does not hand out the "
                                          "transition segment it composes");
    if (!s) {
        stop_primary(*p, timing);
        com_ret(c, S_OK);
        return;
    }
    download(*s);
    if (DmPerformance_playTransition(p->engine->dm, s->dm, embellishment, timing) !=
        DmResult_SUCCESS) {
        com_ret(c, E_FAIL);
        return;
    }
    ComObj *st = start_primary(*p, so, *s);
    if (out_ok(to_state))
        hand_out(to_state, st, IF_DMSEGMENTSTATE);
    com_ret(c, S_OK);
}

void Composer_NotImpl(X86 *c) {
    log_once("dmusic.composer", "dmusic: IDirectMusicComposer composes transitions only");
    com_ret(c, E_NOTIMPL);
}

ComObj *composer_create() {
    return com_new(K_DMCOMPOSER);
}

} // namespace

namespace {

// ===========================================================================
// IDirectMusicPort and IKsControl: the software synthesizer.
// ===========================================================================
Port *this_port(X86 *c, ComIface iface = IF_DMPORT) {
    return find(g_ports, com_this_arg(c, iface));
}

// DMUS_PORTCAPS for the one port there is.
void put_port_caps(uint32_t caps) {
    gm_zero(caps + 4, PORTCAPS_SIZE - 4);
    wr32(caps + 0x04, PC_DLS | PC_SOFTWARESYNTH | PC_DIRECTSOUND | PC_SHAREABLE);
    memcpy(gm_ptr(caps + 0x08), CLSID_DirectMusicSynth.b, 16); // guidPort
    wr32(caps + 0x18, PC_OUTPUTCLASS);                         // dwClass
    wr32(caps + 0x1c, PORT_USER_MODE_SYNTH);                   // dwType
    wr32(caps + 0x20, PC_SYSTEMMEMORY);                        // dwMemorySize
    wr32(caps + 0x24, MAX_CHANNEL_GROUPS);                     // dwMaxChannelGroups
    wr32(caps + 0x28, 1000);                                   // dwMaxVoices
    wr32(caps + 0x2c, 2);                                      // dwMaxAudioChannels
    wr32(caps + 0x30, EFFECT_REVERB);                          // dwEffectFlags
    gm_put_wstr(caps + 0x34, "Software Synthesizer", 128);
}

void Port_GetCaps(X86 *c) {
    const uint32_t caps = arg(c, 1);
    if (!this_port(c) || !caps || !gm_valid(caps, PORTCAPS_SIZE)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (rd32(caps) != PORTCAPS_SIZE) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    put_port_caps(caps);
    com_ret(c, S_OK);
}

void Port_SetNumChannelGroups(X86 *c) {
    Port *p = this_port(c);
    const uint32_t n = arg(c, 1);
    if (!p) {
        com_ret(c, E_POINTER);
        return;
    }
    if (n < 1 || n > MAX_CHANNEL_GROUPS) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    p->groups = n;
    com_ret(c, S_OK);
}

void Port_GetNumChannelGroups(X86 *c) {
    Port *p = this_port(c);
    if (!p || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), p->groups);
    com_ret(c, S_OK);
}

void Port_Activate(X86 *c) {
    Port *p = this_port(c);
    if (!p) {
        com_ret(c, E_POINTER);
        return;
    }
    const bool on = arg(c, 1) != 0;
    const bool same = p->active == on;
    p->active = on;
    com_ret(c, same ? S_FALSE : S_OK);
}

// GetFormat(pWaveFormatEx, pdwWaveFormatExSize, pdwBufferSize)
void Port_GetFormat(X86 *c) {
    Port *p = this_port(c);
    const uint32_t wfx = arg(c, 1), size = arg(c, 2), buffer = arg(c, 3);
    if (!p || !out_ok(size)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (wfx) {
        if (rd32(size) < 18 || !gm_valid(wfx, 18)) {
            com_ret(c, E_INVALIDARG);
            return;
        }
        wr16(wfx + 0, 1); // WAVE_FORMAT_PCM
        wr16(wfx + 2, (uint16_t)p->audio_channels);
        wr32(wfx + 4, p->sample_rate);
        wr32(wfx + 8, p->sample_rate * 2 * p->audio_channels);
        wr16(wfx + 12, (uint16_t)(2 * p->audio_channels));
        wr16(wfx + 14, 16);
        wr16(wfx + 16, 0);
    }
    wr32(size, 18);
    if (out_ok(buffer))
        wr32(buffer, p->sample_rate / 10 * 2 * p->audio_channels); // about 100 ms
    com_ret(c, S_OK);
}

void Port_SetChannelPriority(X86 *c) {
    com_ret(c, this_port(c) ? S_OK : E_POINTER);
}

void Port_GetChannelPriority(X86 *c) {
    if (!this_port(c) || !out_ok(arg(c, 3))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 3), 0x80000000u); // DAUD_STANDARD_VOICE_PRIORITY
    com_ret(c, S_OK);
}

void Port_OkNoop(X86 *c) {
    com_ret(c, S_OK);
}

void Port_NotImpl(X86 *c) {
    log_once("dmusic.port", "dmusic: the port's buffers, instruments, clocks and statistics are "
                            "the synthesizer's own and not available");
    com_ret(c, E_NOTIMPL);
}

void port_destroy(ComObj *o) {
    g_ports.erase(o->id);
}

ComObj *port_create() {
    ComObj *o = com_new(K_DMPORT);
    Port &p = g_ports[o->id];
    p.out = std::make_shared<PortOut>();
    return o;
}

// KsProperty(pProperty, ulPropertyLength, pPropertyData, ulDataLength,
// pBytesReturned): the synthesizer's volume (LONG, hundredths of a dB), its
// effects (DWORD, bit 0 the reverb) and the reverb's parameters
// (DMUS_WAVES_REVERB_PARAMS, four floats).
void Ks_Property(X86 *c) {
    Port *p = this_port(c, IF_KSCONTROL);
    const uint32_t prop = arg(c, 1), prop_len = arg(c, 2), data = arg(c, 3), data_len = arg(c, 4),
                   returned = arg(c, 5);
    if (!p || !prop || prop_len < 0x18 || !gm_valid(prop, 0x18)) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    if (out_ok(returned))
        wr32(returned, 0);
    const uint32_t id = rd32(prop + 16), flags = rd32(prop + 20);
    uint32_t size = 0;
    if (guid_is(prop, GUID_DMUS_PROP_Volume) || guid_is(prop, GUID_DMUS_PROP_Effects))
        size = 4;
    else if (guid_is(prop, GUID_DMUS_PROP_WavesReverb))
        size = 16;
    if (!size || id != 0) {
        log_once(("dmusic.ks." + guid_text(prop)).c_str(),
                 "dmusic: the synthesizer has no property %s (id %u)", guid_text(prop).c_str(), id);
        com_ret(c, DMUS_E_UNKNOWN_PROPERTY);
        return;
    }
    if (flags & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (data_len < 4 || !gm_valid(data, 4)) {
            com_ret(c, E_INVALIDARG);
            return;
        }
        wr32(data, 0x3); // KSPROPERTY_SUPPORT_GET | KSPROPERTY_SUPPORT_SET
        if (out_ok(returned))
            wr32(returned, 4);
        com_ret(c, S_OK);
        return;
    }
    if (!(flags & (KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET)) || data_len < size ||
        !gm_valid(data, size)) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    std::lock_guard<std::mutex> held(g_engines_mutex);
    PortOut &out = *p->out;
    const bool set = (flags & KSPROPERTY_TYPE_SET) != 0;
    if (guid_is(prop, GUID_DMUS_PROP_Volume)) {
        if (set)
            out.volume_mb = std::min(std::max((int32_t)rd32(data), -20000), 2000);
        else
            wr32(data, (uint32_t)out.volume_mb);
    } else if (guid_is(prop, GUID_DMUS_PROP_Effects)) {
        if (set)
            out.effects = rd32(data) & EFFECT_REVERB;
        else
            wr32(data, out.effects);
    } else {
        float v[4];
        if (set) {
            memcpy(v, gm_ptr(data), 16);
            out.reverb.in_gain_db = v[0];
            out.reverb.mix_db = v[1];
            out.reverb.time_ms = v[2];
            out.reverb.hf_ratio = v[3];
            ++out.version;
        } else {
            v[0] = out.reverb.in_gain_db;
            v[1] = out.reverb.mix_db;
            v[2] = out.reverb.time_ms;
            v[3] = out.reverb.hf_ratio;
            memcpy(gm_ptr(data), v, 16);
        }
    }
    if (out_ok(returned))
        wr32(returned, set ? 0 : size);
    com_ret(c, S_OK);
}

void Ks_NotImpl(X86 *c) {
    com_ret(c, DMUS_E_UNKNOWN_PROPERTY);
}

// ===========================================================================
// IDirectMusic: the port factory.
// ===========================================================================
void DM_EnumPort(X86 *c) {
    const uint32_t index = arg(c, 1), caps = arg(c, 2);
    if (!find(g_dms, com_this_arg(c, IF_DIRECTMUSIC)) || !caps || !gm_valid(caps, PORTCAPS_SIZE)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (index != 0) {
        com_ret(c, S_FALSE);
        return;
    }
    if (rd32(caps) != PORTCAPS_SIZE) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    put_port_caps(caps);
    com_ret(c, S_OK);
}

// CreatePort(rclsidPort, pPortParams, ppPort, pUnkOuter): the software
// synthesizer, whatever was asked for; the parameters it cannot take are
// written back with what it took, as DirectMusic does, with S_FALSE.
void DM_CreatePort(X86 *c) {
    const uint32_t cls = arg(c, 1), params = arg(c, 2), out = arg(c, 3), outer = arg(c, 4);
    if (!find(g_dms, com_this_arg(c, IF_DIRECTMUSIC)) || !out_ok(out) || !params ||
        !gm_valid(params, PORTPARAMS7_SIZE)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (outer) {
        com_ret(c, CLASS_E_NOAGGREGATION);
        return;
    }
    if (rd32(params) < PORTPARAMS7_SIZE) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    static const uint8_t guid_null[16] = {0};
    if (!guid_is(cls, CLSID_DirectMusicSynth) &&
        !(cls && gm_valid(cls, 16) && memcmp(gm_ptr(cls), guid_null, 16) == 0))
        log_once(("dmusic.port." + guid_text(cls)).c_str(),
                 "dmusic: port %s is served by the software synthesizer", guid_text(cls).c_str());
    ComObj *o = port_create();
    Port &p = g_ports[o->id];
    const uint32_t valid = rd32(params + 4);
    bool changed = false;
    if (valid & PORTPARAMS_VOICES)
        p.voices = std::max(1u, rd32(params + 8));
    if (valid & PORTPARAMS_CHANNELGROUPS) {
        const uint32_t want = rd32(params + 12);
        p.groups = std::min(std::max(want, 1u), (uint32_t)MAX_CHANNEL_GROUPS);
        if (p.groups != want) {
            wr32(params + 12, p.groups);
            changed = true;
        }
    }
    if (valid & PORTPARAMS_AUDIOCHANNELS) {
        const uint32_t want = rd32(params + 16);
        p.audio_channels = want == 1 ? 1 : 2;
        if (p.audio_channels != want) {
            wr32(params + 16, p.audio_channels);
            changed = true;
        }
    }
    if (valid & PORTPARAMS_SAMPLERATE)
        p.sample_rate = std::max(8000u, std::min(rd32(params + 20), 96000u));
    if (valid & PORTPARAMS_EFFECTS)
        p.out->effects = rd32(params + 24) & EFFECT_REVERB;
    LOGV("dmusic: CreatePort -> port %u (%u groups, %u voices, %u Hz requested)", o->id, p.groups,
         p.voices, p.sample_rate);
    com_ret(c, give(out, o, IF_DMPORT) ? (changed ? S_FALSE : S_OK) : E_OUTOFMEMORY);
}

void DM_GetDefaultPort(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (!out_ok(out, 16)) {
        com_ret(c, E_POINTER);
        return;
    }
    memcpy(gm_ptr(out), CLSID_DirectMusicSynth.b, 16);
    com_ret(c, S_OK);
}

void DM_Activate(X86 *c) {
    DirectMusicObj *d = find(g_dms, com_this_arg(c, IF_DIRECTMUSIC));
    if (!d) {
        com_ret(c, E_POINTER);
        return;
    }
    const bool on = arg(c, 1) != 0;
    const bool same = d->active == on;
    d->active = on;
    com_ret(c, same ? S_FALSE : S_OK);
}

void DM_EnumMasterClock(X86 *c) {
    com_ret(c, S_FALSE);
}

void DM_GetMasterClock(X86 *c) {
    if (out_ok(arg(c, 1), 16))
        gm_zero(arg(c, 1), 16);
    if (arg(c, 2)) {
        log_once("dmusic.clock", "dmusic: the master clock is not available as an object");
        if (out_ok(arg(c, 2)))
            wr32(arg(c, 2), 0);
        com_ret(c, E_NOTIMPL);
        return;
    }
    com_ret(c, S_OK);
}

void DM_OkNoop(X86 *c) {
    com_ret(c, S_OK);
}

void DM_CreateMusicBuffer(X86 *c) {
    if (out_ok(arg(c, 2)))
        wr32(arg(c, 2), 0);
    log_once("dmusic.buffer", "dmusic: music buffers are not available");
    com_ret(c, E_NOTIMPL);
}

ComObj *directmusic_create() {
    ComObj *o = com_new(K_DIRECTMUSIC);
    g_dms[o->id] = DirectMusicObj();
    return o;
}

void directmusic_destroy(ComObj *o) {
    auto it = g_dms.find(o->id);
    if (it == g_dms.end())
        return;
    const uint32_t ds = it->second.directsound;
    g_dms.erase(it);
    if (ds)
        release_id(ds);
}

// SetDirectSound(pDirectSound, hWnd)
void DM_SetDirectSound(X86 *c) {
    DirectMusicObj *d = find(g_dms, com_this_arg(c, IF_DIRECTMUSIC));
    if (!d) {
        com_ret(c, E_POINTER);
        return;
    }
    const uint32_t held = hold_directsound(arg(c, 1));
    if (d->directsound)
        release_id(d->directsound);
    d->directsound = held;
    com_ret(c, S_OK);
}

// ===========================================================================
// IDirectMusicPerformance (DirectX 6.1)
// ===========================================================================
Performance *this_perf(X86 *c) {
    return find(g_perfs, com_this_arg(c, IF_DMPERFORMANCE));
}

// The calls that need an initialised performance get it from here, or the
// error DirectMusic returns.
Performance *live_perf(X86 *c) {
    Performance *p = this_perf(c);
    if (!p) {
        com_ret(c, E_POINTER);
        return nullptr;
    }
    if (!p->initialised || p->closed) {
        com_ret(c, DMUS_E_NOT_INIT);
        return nullptr;
    }
    return p;
}

// Init(ppDirectMusic, pDirectSound, hWnd): the synthesizer, at the rate the
// host mixes at, the IDirectMusic the ports come from, and a reference to the
// DirectSound given (or, given none, the shared one).
void Perf_Init(X86 *c) {
    ComObj *po = com_this_arg(c, IF_DMPERFORMANCE);
    Performance *p = find(g_perfs, po);
    const uint32_t ppdm = arg(c, 1);
    if (!p) {
        com_ret(c, E_POINTER);
        return;
    }
    if (p->initialised) {
        com_ret(c, E_FAIL); // DMUS_E_ALREADY_INITED
        return;
    }
    double rate = host_synth_rate();
    if (rate <= 0.0)
        rate = 44100.0;
    auto engine = std::make_shared<Engine>();
    if (DmPerformance_create(&engine->dm, (uint32_t)lround(rate)) != DmResult_SUCCESS) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    engine->dm_rate = rate;
    engine->out = std::make_shared<PortOut>();
    DmPerformance_setMasterGroove(engine->dm, p->master_groove);

    // The IDirectMusic: the caller's, or a new one handed back.
    ComObj *dm = nullptr;
    if (ppdm && gm_valid(ppdm, 4) && rd32(ppdm)) {
        dm = object_at(rd32(ppdm), IF_DIRECTMUSIC);
        if (!dm) {
            com_ret(c, E_INVALIDARG);
            return;
        }
        com_addref(dm);
    } else {
        dm = directmusic_create();
        if (ppdm && gm_valid(ppdm, 4) && !hand_out(ppdm, dm, IF_DIRECTMUSIC)) {
            com_release(dm);
            com_ret(c, E_OUTOFMEMORY);
            return;
        }
    }
    p->directmusic = dm->id;
    p->directsound = hold_directsound(arg(c, 2));
    p->engine = engine;
    p->start_ns = os_monotonic_ns();
    p->initialised = true;
    engine_add(engine);
    LOGV("dmusic: performance %u: synthesizer at %.0f Hz", po->id, rate);
    com_ret(c, S_OK);
}

// PlaySegment(pSegment, dwFlags, i64 rtStartTime, ppSegmentState)
void Perf_PlaySegment(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    ComObj *so = object_at(arg(c, 1), IF_DMSEGMENT);
    Segment *s = find(g_segments, so);
    const uint32_t flags = arg(c, 2), out = arg(c, 5);
    if (out_ok(out))
        wr32(out, 0);
    if (!s) {
        com_ret(c, E_POINTER);
        return;
    }
    if (arg64(c, 3) != 0)
        log_once("dmusic.starttime", "dmusic: PlaySegment's start time is not honoured; "
                                     "the segment starts at its boundary");
    if (flags & SEGF_SECONDARY) {
        // A secondary segment would play over the primary; dmusic has one.
        log_once("dmusic.secondary", "dmusic: secondary segments are not played");
        ComObj *st = com_new(K_DMSEGMENTSTATE);
        PlayState ps;
        ps.segment = so->id;
        com_addref(so);
        ps.repeats = DmSegment_getRepeats(s->dm);
        ps.start = music_time(*p);
        g_states[st->id] = ps;
        if (out_ok(out))
            give(out, st, IF_DMSEGMENTSTATE);
        else
            com_release(st);
        com_ret(c, S_OK);
        return;
    }
    if (flags & SEGF_QUEUE)
        log_once("dmusic.queue", "dmusic: a queued segment starts at its boundary, not after "
                                 "the playing one ends");
    if (!s->downloaded && !p->auto_download)
        log_once("dmusic.notdownloaded", "dmusic: a segment played before its instruments were "
                                         "downloaded is downloaded now");
    download(*s);
    const DmTiming timing = timing_for(flags, s->header.resolution);
    if (DmPerformance_playSegment(p->engine->dm, s->dm, timing) != DmResult_SUCCESS) {
        com_ret(c, E_FAIL);
        return;
    }
    ComObj *st = start_primary(*p, so, *s);
    LOGV("dmusic: PlaySegment(%s, %#x) -> state %u", s->name.c_str(), flags, st->id);
    if (out_ok(out))
        hand_out(out, st, IF_DMSEGMENTSTATE);
    com_ret(c, S_OK);
}

// Stop(pSegment, pSegmentState, mtTime, dwFlags): everything when both are
// null, else the primary segment if it is the one named.
void Perf_Stop(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    const uint32_t seg = arg(c, 1), state = arg(c, 2), flags = arg(c, 4);
    bool match = !seg && !state;
    if (!match && p->primary) {
        auto it = g_states.find(p->primary);
        ComObj *st = com_get(p->primary);
        if (state && st && com_this(state) == st)
            match = true;
        if (seg && it != g_states.end() && com_this(seg) == com_get(it->second.segment))
            match = true;
    }
    if (match)
        stop_primary(*p, timing_for(flags, 0));
    com_ret(c, S_OK);
}

// GetSegmentState(ppSegmentState, mtTime): the primary segment, while it plays.
void Perf_GetSegmentState(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    const uint32_t out = arg(c, 1);
    if (!out_ok(out)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (!primary_playing(*p)) {
        com_ret(c, DMUS_E_NOT_FOUND);
        return;
    }
    ComObj *st = com_get(p->primary);
    com_ret(c, st && hand_out(out, st, IF_DMSEGMENTSTATE) ? S_OK : DMUS_E_NOT_FOUND);
}

void Perf_SetPrepareTime(X86 *c) {
    if (Performance *p = this_perf(c))
        p->prepare_ms = arg(c, 1);
    com_ret(c, S_OK);
}

void Perf_GetPrepareTime(X86 *c) {
    Performance *p = this_perf(c);
    if (!p || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), p->prepare_ms);
    com_ret(c, S_OK);
}

void Perf_SetBumperLength(X86 *c) {
    if (Performance *p = this_perf(c))
        p->bumper_ms = arg(c, 1);
    com_ret(c, S_OK);
}

void Perf_GetBumperLength(X86 *c) {
    Performance *p = this_perf(c);
    if (!p || !out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), p->bumper_ms);
    com_ret(c, S_OK);
}

// SendPMsg(pPMSG): the performance owns the message from here and frees it.
void Perf_SendPMsg(X86 *c) {
    log_once("dmusic.pmsg", "dmusic: messages sent to the performance are freed, not played");
    if (arg(c, 1))
        heap_free(arg(c, 1));
    com_ret(c, S_OK);
}

// MusicToReferenceTime(mtTime, prtTime) and ReferenceToMusicTime(rtTime,
// prtTime), on GetTime's clock.
void Perf_MusicToReferenceTime(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    if (!out_ok(arg(c, 2), 8)) {
        com_ret(c, E_POINTER);
        return;
    }
    const int32_t mt = (int32_t)arg(c, 1);
    const int64_t origin = (int64_t)(p->start_ns / 100);
    wr64(arg(c, 2), origin + (int64_t)((double)mt / kTicksPerSecond * 1e7));
    com_ret(c, S_OK);
}

void Perf_ReferenceToMusicTime(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    if (!out_ok(arg(c, 3))) {
        com_ret(c, E_POINTER);
        return;
    }
    const int64_t rt = arg64(c, 1) - (int64_t)(p->start_ns / 100);
    wr32(arg(c, 3), (uint32_t)(int32_t)((double)rt * 1e-7 * kTicksPerSecond));
    com_ret(c, S_OK);
}

// IsPlaying(pSegment, pSegState)
void Perf_IsPlaying(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    const uint32_t seg = arg(c, 1), state = arg(c, 2);
    if (!seg && !state) {
        com_ret(c, E_POINTER);
        return;
    }
    PlayState *ps = primary_playing(*p);
    bool playing = false;
    if (ps) {
        if (state)
            playing = com_this(state) == com_get(p->primary);
        else
            playing = com_this(seg) == com_get(ps->segment);
    }
    com_ret(c, playing ? S_OK : S_FALSE);
}

// GetTime(prtNow, pmtNow)
void Perf_GetTime(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    if (out_ok(arg(c, 1), 8))
        wr64(arg(c, 1), reference_time());
    if (out_ok(arg(c, 2)))
        wr32(arg(c, 2), (uint32_t)music_time(*p));
    com_ret(c, S_OK);
}

// AllocPMsg(cb, ppPMSG) and FreePMsg(pPMSG): DMUS_PMSG is 0x34 bytes, the
// first of which is its size.
void Perf_AllocPMsg(X86 *c) {
    const uint32_t cb = arg(c, 1), out = arg(c, 2);
    if (!out_ok(out)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(out, 0);
    if (cb < 0x34) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    const uint32_t msg = heap_alloc(cb, true);
    if (!msg) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(msg, cb);
    wr32(out, msg);
    com_ret(c, S_OK);
}

void Perf_FreePMsg(X86 *c) {
    if (!arg(c, 1)) {
        com_ret(c, E_POINTER);
        return;
    }
    heap_free(arg(c, 1));
    com_ret(c, S_OK);
}

void Perf_GetGraph(X86 *c) {
    if (out_ok(arg(c, 1)))
        wr32(arg(c, 1), 0);
    com_ret(c, DMUS_E_NOT_FOUND);
}

void Perf_OkNoop(X86 *c) {
    com_ret(c, S_OK);
}

// GetNotificationPMsg(ppNotificationMsg): no notification is ever queued.
void Perf_GetNotificationPMsg(X86 *c) {
    if (!out_ok(arg(c, 1))) {
        com_ret(c, E_POINTER);
        return;
    }
    wr32(arg(c, 1), 0);
    com_ret(c, S_FALSE);
}

void Perf_SetNotificationHandle(X86 *c) {
    if (arg(c, 1))
        log_once("dmusic.notify", "dmusic: a notification handle is accepted; nothing is "
                                  "ever signalled on it");
    com_ret(c, S_OK);
}

// AddPort(pPort): the synthesizer whose output stage the performance plays
// through. Null adds a default port.
void Perf_AddPort(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    ComObj *po = arg(c, 1) ? object_at(arg(c, 1), IF_DMPORT) : port_create();
    Port *port = find(g_ports, po);
    if (!port) {
        com_ret(c, E_POINTER);
        return;
    }
    if (std::find(p->ports.begin(), p->ports.end(), po->id) != p->ports.end()) {
        com_ret(c, S_FALSE);
        return;
    }
    if (arg(c, 1))
        com_addref(po); // a default port's creation reference is already the performance's
    p->ports.push_back(po->id);
    if (p->ports.size() == 1) {
        std::lock_guard<std::mutex> held(g_engines_mutex);
        p->engine->out = port->out;
    }
    com_ret(c, S_OK);
}

void Perf_RemovePort(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    ComObj *po = object_at(arg(c, 1), IF_DMPORT);
    auto it = po ? std::find(p->ports.begin(), p->ports.end(), po->id) : p->ports.end();
    if (it == p->ports.end()) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    p->ports.erase(it);
    {
        std::lock_guard<std::mutex> held(g_engines_mutex);
        Port *first = p->ports.empty() ? nullptr : find(g_ports, com_get(p->ports.front()));
        p->engine->out = first ? first->out : std::make_shared<PortOut>();
    }
    com_release(po);
    com_ret(c, S_OK);
}

// AssignPChannelBlock(dwBlockNum, pPort, dwGroup) and AssignPChannel: one
// synthesizer plays every channel.
void Perf_AssignPChannelBlock(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    com_ret(c, find(g_ports, object_at(arg(c, 2), IF_DMPORT)) ? S_OK : E_INVALIDARG);
}

void Perf_AssignPChannel(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    com_ret(c, find(g_ports, object_at(arg(c, 2), IF_DMPORT)) ? S_OK : E_INVALIDARG);
}

// PChannelInfo(dwPChannel, ppPort, pdwGroup, pdwMChannel)
void Perf_PChannelInfo(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    const uint32_t pchannel = arg(c, 1);
    if (p->ports.empty()) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    if (out_ok(arg(c, 2)))
        hand_out(arg(c, 2), com_get(p->ports.front()), IF_DMPORT);
    if (out_ok(arg(c, 3)))
        wr32(arg(c, 3), pchannel / 16 + 1);
    if (out_ok(arg(c, 4)))
        wr32(arg(c, 4), pchannel % 16);
    com_ret(c, S_OK);
}

void Perf_DownloadInstrument(X86 *c) {
    for (int i : {3, 6, 7, 8})
        if (out_ok(arg(c, i)))
            wr32(arg(c, i), 0);
    log_once("dmusic.instrument", "dmusic: instruments are downloaded with their segments only");
    com_ret(c, E_NOTIMPL);
}

void Perf_GetParam(X86 *c) {
    if (out_ok(arg(c, 5)))
        wr32(arg(c, 5), 0);
    log_once(("dmusic.perf.getparam." + guid_text(arg(c, 1))).c_str(),
             "dmusic: IDirectMusicPerformance::GetParam %s is not available",
             guid_text(arg(c, 1)).c_str());
    com_ret(c, DMUS_E_NOT_FOUND);
}

void Perf_SetParam(X86 *c) {
    log_once(("dmusic.perf.setparam." + guid_text(arg(c, 1))).c_str(),
             "dmusic: IDirectMusicPerformance::SetParam %s is accepted and ignored",
             guid_text(arg(c, 1)).c_str());
    com_ret(c, S_OK);
}

// SetGlobalParam(rguidType, pParam, dwSize): auto-download (BOOL), the master
// groove level (char), the master volume (LONG, hundredths of a dB) and the
// master tempo (float) mean something; every parameter is kept for
// GetGlobalParam, as DirectMusic keeps them.
void Perf_SetGlobalParam(X86 *c) {
    Performance *p = this_perf(c);
    const uint32_t type = arg(c, 1), param = arg(c, 2), size = arg(c, 3);
    if (!p || !type || !gm_valid(type, 16) || !param || !size || !gm_valid(param, size)) {
        com_ret(c, E_POINTER);
        return;
    }
    if (size > 0x10000) {
        com_ret(c, E_INVALIDARG);
        return;
    }
    p->globals[guid_text(type)].assign(gm_ptr(param), gm_ptr(param) + size);
    if (guid_is(type, GUID_PerfAutoDownload) && size >= 4) {
        p->auto_download = rd32(param) != 0;
    } else if (guid_is(type, GUID_PerfMasterGrooveLevel)) {
        p->master_groove = (int8_t)rd8(param);
        if (p->engine)
            DmPerformance_setMasterGroove(p->engine->dm, p->master_groove);
        LOGV("dmusic: master groove level %d", p->master_groove);
    } else if (guid_is(type, GUID_PerfMasterVolume) && size >= 4) {
        if (p->engine) {
            std::lock_guard<std::mutex> held(g_engines_mutex);
            p->engine->master_volume_mb = std::min(std::max((int32_t)rd32(param), -20000), 2000);
        }
    } else if (guid_is(type, GUID_PerfMasterTempo) && size >= 4) {
        float tempo;
        memcpy(&tempo, gm_ptr(param), 4);
        p->master_tempo = tempo;
        if (tempo != 1.0f)
            log_once("dmusic.tempo", "dmusic: the master tempo is recorded; the music keeps "
                                     "its own tempo");
    }
    com_ret(c, S_OK);
}

void Perf_GetGlobalParam(X86 *c) {
    Performance *p = this_perf(c);
    const uint32_t type = arg(c, 1), param = arg(c, 2), size = arg(c, 3);
    if (!p || !type || !gm_valid(type, 16) || !param || !size || !gm_valid(param, size)) {
        com_ret(c, E_POINTER);
        return;
    }
    auto it = p->globals.find(guid_text(type));
    if (it != p->globals.end()) {
        memcpy(gm_ptr(param), it->second.data(), std::min<size_t>(size, it->second.size()));
        com_ret(c, S_OK);
        return;
    }
    // The defaults of the ones that have them.
    if (guid_is(type, GUID_PerfAutoDownload) && size >= 4) {
        wr32(param, 0);
    } else if (guid_is(type, GUID_PerfMasterGrooveLevel)) {
        wr8(param, 0);
    } else if (guid_is(type, GUID_PerfMasterVolume) && size >= 4) {
        wr32(param, 0);
    } else if (guid_is(type, GUID_PerfMasterTempo) && size >= 4) {
        const float one = 1.0f;
        memcpy(gm_ptr(param), &one, 4);
    } else {
        com_ret(c, E_INVALIDARG);
        return;
    }
    com_ret(c, S_OK);
}

// GetLatencyTime(prtTime) and GetQueueTime(prtTime): now, plus the host's
// block and, for the queue, the bumper.
void Perf_GetLatencyTime(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    if (!out_ok(arg(c, 1), 8)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr64(arg(c, 1), reference_time() + 20 * 10000);
    com_ret(c, S_OK);
}

void Perf_GetQueueTime(X86 *c) {
    Performance *p = live_perf(c);
    if (!p)
        return;
    if (!out_ok(arg(c, 1), 8)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr64(arg(c, 1), reference_time() + (20 + (int64_t)p->bumper_ms) * 10000);
    com_ret(c, S_OK);
}

// GetResolvedTime(i64 rtTime, prtResolved, dwTimeResolveFlags)
void Perf_GetResolvedTime(X86 *c) {
    if (!out_ok(arg(c, 3), 8)) {
        com_ret(c, E_POINTER);
        return;
    }
    wr64(arg(c, 3), arg64(c, 1));
    com_ret(c, S_OK);
}

void Perf_NotImpl(X86 *c) {
    log_once("dmusic.perf.music", "dmusic: the performance's MIDI and rhythm conversions are "
                                  "not implemented");
    com_ret(c, E_NOTIMPL);
}

// Everything a performance holds, let go: by CloseDown or its last Release.
void close_down(Performance &p) {
    if (p.closed)
        return;
    p.closed = true;
    if (p.engine) {
        DmPerformance_playSegment(p.engine->dm, nullptr, DmTiming_INSTANT);
        engine_remove(p.engine);
    }
    if (p.primary)
        release_id(p.primary);
    p.primary = 0;
    std::vector<uint32_t> ports;
    ports.swap(p.ports);
    for (uint32_t id : ports)
        release_id(id);
    if (p.directmusic)
        release_id(p.directmusic);
    p.directmusic = 0;
    if (p.directsound)
        release_id(p.directsound);
    p.directsound = 0;
}

void Perf_CloseDown(X86 *c) {
    Performance *p = this_perf(c);
    if (!p) {
        com_ret(c, E_POINTER);
        return;
    }
    close_down(*p);
    com_ret(c, S_OK);
}

ComObj *performance_create() {
    ComObj *o = com_new(K_DMPERFORMANCE);
    g_perfs[o->id] = Performance();
    return o;
}

void performance_destroy(ComObj *o) {
    auto it = g_perfs.find(o->id);
    if (it == g_perfs.end())
        return;
    close_down(it->second);
    g_perfs.erase(o->id);
}

// ===========================================================================
// The vtables. Slot order is the DirectX 6.1/7 headers'.
// ===========================================================================
#define IUNKNOWN_SLOTS                                                                             \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef}, {                        \
        "Release", 1, com_Release                                                                  \
    }

const ComMethod kLoader[] = {
    IUNKNOWN_SLOTS,
    {"GetObject", 4, Loader_GetObject},
    {"SetObject", 2, Loader_SetObject},
    {"SetSearchDirectory", 4, Loader_SetSearchDirectory},
    {"ScanDirectory", 4, Loader_ScanDirectory},
    {"CacheObject", 2, Loader_CacheObject},
    {"ReleaseObject", 2, Loader_ReleaseObject},
    {"ClearCache", 2, Loader_ClearCache},
    {"EnableCache", 3, Loader_EnableCache},
    {"EnumObject", 4, Loader_EnumObject},
};
static_assert(sizeof kLoader / sizeof *kLoader == 12, "IDirectMusicLoader has 12 slots");

const ComMethod kComposer[] = {
    IUNKNOWN_SLOTS,
    {"ComposeSegmentFromTemplate", 6, Composer_NotImpl},
    {"ComposeSegmentFromShape", 9, Composer_NotImpl},
    {"ComposeTransition", 8, Composer_NotImpl},
    {"AutoTransition", 9, Composer_AutoTransition},
    {"ComposeTemplateFromShape", 7, Composer_NotImpl},
    {"ChangeChordMap", 4, Composer_NotImpl},
};
static_assert(sizeof kComposer / sizeof *kComposer == 9, "IDirectMusicComposer has 9 slots");

const ComMethod kPerformance[] = {
    IUNKNOWN_SLOTS,
    {"Init", 4, Perf_Init},
    {"PlaySegment", 6, Perf_PlaySegment},
    {"Stop", 5, Perf_Stop},
    {"GetSegmentState", 3, Perf_GetSegmentState},
    {"SetPrepareTime", 2, Perf_SetPrepareTime},
    {"GetPrepareTime", 2, Perf_GetPrepareTime},
    {"SetBumperLength", 2, Perf_SetBumperLength},
    {"GetBumperLength", 2, Perf_GetBumperLength},
    {"SendPMsg", 2, Perf_SendPMsg},
    {"MusicToReferenceTime", 3, Perf_MusicToReferenceTime},
    {"ReferenceToMusicTime", 4, Perf_ReferenceToMusicTime},
    {"IsPlaying", 3, Perf_IsPlaying},
    {"GetTime", 3, Perf_GetTime},
    {"AllocPMsg", 3, Perf_AllocPMsg},
    {"FreePMsg", 2, Perf_FreePMsg},
    {"GetGraph", 2, Perf_GetGraph},
    {"SetGraph", 2, Perf_OkNoop},
    {"SetNotificationHandle", 4, Perf_SetNotificationHandle},
    {"GetNotificationPMsg", 2, Perf_GetNotificationPMsg},
    {"AddNotificationType", 2, Perf_OkNoop},
    {"RemoveNotificationType", 2, Perf_OkNoop},
    {"AddPort", 2, Perf_AddPort},
    {"RemovePort", 2, Perf_RemovePort},
    {"AssignPChannelBlock", 4, Perf_AssignPChannelBlock},
    {"AssignPChannel", 5, Perf_AssignPChannel},
    {"PChannelInfo", 5, Perf_PChannelInfo},
    {"DownloadInstrument", 9, Perf_DownloadInstrument},
    {"Invalidate", 3, Perf_OkNoop},
    {"GetParam", 7, Perf_GetParam},
    {"SetParam", 6, Perf_SetParam},
    {"GetGlobalParam", 4, Perf_GetGlobalParam},
    {"SetGlobalParam", 4, Perf_SetGlobalParam},
    {"GetLatencyTime", 2, Perf_GetLatencyTime},
    {"GetQueueTime", 2, Perf_GetQueueTime},
    {"AdjustTime", 3, Perf_OkNoop},
    {"CloseDown", 1, Perf_CloseDown},
    {"GetResolvedTime", 5, Perf_GetResolvedTime},
    {"MIDIToMusic", 6, Perf_NotImpl},
    {"MusicToMIDI", 6, Perf_NotImpl},
    {"TimeToRhythm", 7, Perf_NotImpl},
    {"RhythmToTime", 7, Perf_NotImpl},
};
static_assert(sizeof kPerformance / sizeof *kPerformance == 44,
              "IDirectMusicPerformance has 44 slots");

const ComMethod kDirectMusic[] = {
    IUNKNOWN_SLOTS,
    {"EnumPort", 3, DM_EnumPort},
    {"CreateMusicBuffer", 4, DM_CreateMusicBuffer},
    {"CreatePort", 5, DM_CreatePort},
    {"EnumMasterClock", 3, DM_EnumMasterClock},
    {"GetMasterClock", 3, DM_GetMasterClock},
    {"SetMasterClock", 2, DM_OkNoop},
    {"Activate", 2, DM_Activate},
    {"GetDefaultPort", 2, DM_GetDefaultPort},
    {"SetDirectSound", 3, DM_SetDirectSound},
};
static_assert(sizeof kDirectMusic / sizeof *kDirectMusic == 12, "IDirectMusic has 12 slots");

const ComMethod kPort[] = {
    IUNKNOWN_SLOTS,
    {"PlayBuffer", 2, Port_NotImpl},
    {"SetReadNotificationHandle", 2, Port_NotImpl},
    {"Read", 2, Port_NotImpl},
    {"DownloadInstrument", 5, Port_NotImpl},
    {"UnloadInstrument", 2, Port_NotImpl},
    {"GetLatencyClock", 2, Port_NotImpl},
    {"GetRunningStats", 2, Port_NotImpl},
    {"Compact", 1, Port_OkNoop},
    {"GetCaps", 2, Port_GetCaps},
    {"DeviceIoControl", 8, Port_NotImpl},
    {"SetNumChannelGroups", 2, Port_SetNumChannelGroups},
    {"GetNumChannelGroups", 2, Port_GetNumChannelGroups},
    {"Activate", 2, Port_Activate},
    {"SetChannelPriority", 4, Port_SetChannelPriority},
    {"GetChannelPriority", 4, Port_GetChannelPriority},
    {"SetDirectSound", 3, Port_OkNoop},
    {"GetFormat", 4, Port_GetFormat},
};
static_assert(sizeof kPort / sizeof *kPort == 20, "IDirectMusicPort has 20 slots");

const ComMethod kKsControl[] = {
    IUNKNOWN_SLOTS,
    {"KsProperty", 6, Ks_Property},
    {"KsMethod", 6, Ks_NotImpl},
    {"KsEvent", 6, Ks_NotImpl},
};

const ComMethod kSegment[] = {
    IUNKNOWN_SLOTS,
    {"GetLength", 2, Segment_GetLength},
    {"SetLength", 2, Segment_SetLength},
    {"GetRepeats", 2, Segment_GetRepeats},
    {"SetRepeats", 2, Segment_SetRepeats},
    {"GetDefaultResolution", 2, Segment_GetDefaultResolution},
    {"SetDefaultResolution", 2, Segment_SetDefaultResolution},
    {"GetTrack", 5, Segment_GetTrack},
    {"GetTrackGroup", 3, Segment_GetTrackGroup},
    {"InsertTrack", 3, Segment_TrackNotImpl},
    {"RemoveTrack", 2, Segment_TrackNotImpl},
    {"InitPlay", 4, Segment_InitPlay},
    {"GetGraph", 2, Segment_GetGraph},
    {"SetGraph", 2, Segment_OkNoop},
    {"AddNotificationType", 2, Segment_OkNoop},
    {"RemoveNotificationType", 2, Segment_OkNoop},
    {"GetParam", 7, Segment_GetParam},
    {"SetParam", 6, Segment_SetParam},
    {"Clone", 4, Segment_Clone},
    {"SetStartPoint", 2, Segment_SetStartPoint},
    {"GetStartPoint", 2, Segment_GetStartPoint},
    {"SetLoopPoints", 3, Segment_SetLoopPoints},
    {"GetLoopPoints", 3, Segment_GetLoopPoints},
    {"SetPChannelsUsed", 3, Segment_OkNoop},
};
static_assert(sizeof kSegment / sizeof *kSegment == 26, "IDirectMusicSegment has 26 slots");

const ComMethod kSegmentState[] = {
    IUNKNOWN_SLOTS,
    {"GetRepeats", 2, State_GetRepeats},
    {"GetSegment", 2, State_GetSegment},
    {"GetStartTime", 2, State_GetStartTime},
    {"GetSeek", 2, State_GetZero},
    {"GetStartPoint", 2, State_GetZero},
};

const ComMethod kStyle[] = {
    IUNKNOWN_SLOTS,
    {"GetBand", 3, Style_GetNone},
    {"EnumBand", 3, Style_EnumNone},
    {"GetDefaultBand", 2, Style_GetDefaultNone},
    {"EnumMotif", 3, Style_EnumNone},
    {"GetMotif", 3, Style_GetNone},
    {"GetDefaultChordMap", 2, Style_GetDefaultNone},
    {"EnumChordMap", 3, Style_EnumNone},
    {"GetChordMap", 3, Style_GetNone},
    {"GetTimeSignature", 2, Style_NotImpl},
    {"GetEmbellishmentLength", 5, Style_NotImpl},
    {"GetTempo", 2, Style_NotImpl},
};
static_assert(sizeof kStyle / sizeof *kStyle == 14, "IDirectMusicStyle has 14 slots");

} // namespace

// ===========================================================================
// Registration
// ===========================================================================
bool dmusic_enabled() {
    if (g_enabled_override >= 0)
        return g_enabled_override != 0;
    const char *e = recomp_env("DMUSIC");
    if (e && *e)
        return strcmp(e, "0") != 0;
    return RECOMP_DMUSIC_DEFAULT != 0;
}

// Test seam: -1 goes back to the game's setting. Takes effect at the next
// dmusic_register.
void dmusic_enable_for_test(int on) {
    g_enabled_override = on;
}

void dmusic_register() {
    static bool logger = false;
    if (!logger) {
        logger = true;
        Dm_setLogger(dmusic_log_level(), on_dmusic_log, nullptr);
    }
    const char *dll = "DMUSIC.dll";
    com_define(IF_DMLOADER, dll, "IDirectMusicLoader", kLoader, std::size(kLoader));
    com_define(IF_DMCOMPOSER, dll, "IDirectMusicComposer", kComposer, std::size(kComposer));
    com_define(IF_DMPERFORMANCE, dll, "IDirectMusicPerformance", kPerformance,
               std::size(kPerformance));
    com_define(IF_DIRECTMUSIC, dll, "IDirectMusic", kDirectMusic, std::size(kDirectMusic));
    com_define(IF_DMPORT, dll, "IDirectMusicPort", kPort, std::size(kPort));
    com_define(IF_KSCONTROL, dll, "IKsControl", kKsControl, std::size(kKsControl));
    com_define(IF_DMSEGMENT, dll, "IDirectMusicSegment", kSegment, std::size(kSegment));
    com_define(IF_DMSEGMENTSTATE, dll, "IDirectMusicSegmentState", kSegmentState,
               std::size(kSegmentState));
    com_define(IF_DMSTYLE, dll, "IDirectMusicStyle", kStyle, std::size(kStyle));

    com_bind(IF_DMLOADER, K_DMLOADER);
    com_bind(IF_DMCOMPOSER, K_DMCOMPOSER);
    com_bind(IF_DMPERFORMANCE, K_DMPERFORMANCE);
    com_bind(IF_DIRECTMUSIC, K_DIRECTMUSIC);
    com_bind(IF_DMPORT, K_DMPORT);
    com_bind(IF_KSCONTROL, K_DMPORT);
    com_bind(IF_DMSEGMENT, K_DMSEGMENT);
    com_bind(IF_DMSEGMENTSTATE, K_DMSEGMENTSTATE);
    com_bind(IF_DMSTYLE, K_DMSTYLE);

    com_register_iid(IF_DMLOADER, IID_IDirectMusicLoader.b);
    com_register_iid(IF_DMCOMPOSER, IID_IDirectMusicComposer.b);
    com_register_iid(IF_DMPERFORMANCE, IID_IDirectMusicPerformance.b);
    com_register_iid(IF_DIRECTMUSIC, IID_IDirectMusic.b);
    com_register_iid(IF_DMPORT, IID_IDirectMusicPort.b);
    com_register_iid(IF_KSCONTROL, IID_IKsControl.b);
    com_register_iid(IF_DMSEGMENT, IID_IDirectMusicSegment.b);
    com_register_iid(IF_DMSEGMENTSTATE, IID_IDirectMusicSegmentState.b);
    com_register_iid(IF_DMSTYLE, IID_IDirectMusicStyle.b);

    com_set_destructor(K_DMLOADER, loader_destroy);
    com_set_destructor(K_DMPERFORMANCE, performance_destroy);
    com_set_destructor(K_DIRECTMUSIC, directmusic_destroy);
    com_set_destructor(K_DMPORT, port_destroy);
    com_set_destructor(K_DMSEGMENT, segment_destroy);
    com_set_destructor(K_DMSEGMENTSTATE, state_destroy);
    com_set_destructor(K_DMSTYLE, style_destroy);

    if (!dmusic_enabled())
        return;
    com_register_class(CLSID_DirectMusicLoader.b, "DirectMusicLoader", IF_DMLOADER, loader_create);
    com_register_class(CLSID_DirectMusicComposer.b, "DirectMusicComposer", IF_DMCOMPOSER,
                       composer_create);
    com_register_class(CLSID_DirectMusicPerformance.b, "DirectMusicPerformance", IF_DMPERFORMANCE,
                       performance_create);
    com_register_class(CLSID_DirectMusic.b, "DirectMusic", IF_DIRECTMUSIC, directmusic_create);
    (void)CLSID_DirectMusicSegment; // a loader's class name for segments, not creatable here
}

void dmusic_reset() {
    {
        std::lock_guard<std::mutex> held(g_engines_mutex);
        g_engines.clear();
    }
    // The guest heap these objects lived in is gone: let go of dmusic's side
    // without touching guest memory.
    for (auto &entry : g_segments)
        DmSegment_release(entry.second.dm);
    g_segments.clear();
    g_loaders.clear();
    g_states.clear();
    g_perfs.clear();
    g_ports.clear();
    g_dms.clear();
    g_styles.clear();
}

// How many performances are rendering, for the tests.
size_t dmusic_engine_count() {
    std::lock_guard<std::mutex> held(g_engines_mutex);
    return g_engines.size();
}
