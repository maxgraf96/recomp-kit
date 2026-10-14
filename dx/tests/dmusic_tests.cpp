// dmusic_tests.cpp - DirectMusic (dx/dmusic.cpp) through the guest ABI, on
// synthetic files (make_dmusic_fixtures.py): no game's music.
//
// Every call goes through a COM vtable slot exactly as a game's would, and
// the synthesizer's output is rendered offline through the render function
// the shim hands the host, which this binary captures: no audio device is
// opened. The fixtures' instruments are sines, so what plays is told apart by
// pitch: groove 1 sounds at 441 Hz, groove 2 at 660.7 Hz, grooves 3 and up at
// 882 Hz, and the stand-in General MIDI instrument at 1102.5 Hz; a bar lasts
// one second.
#include "../com.h"
#include "../dx.h"
#include "../dmusic.h"
#include "../dmusic_reverb.h"
#include "../host_api.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"
#include "../../platform/os.h"
#include "fixtures/dmusic_fixtures.h"
#include "guest_abi.h"

#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
        }                                                                                          \
    } while (0)
#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        ++g_checks;                                                                                \
        auto va_ = (a);                                                                            \
        auto vb_ = (b);                                                                            \
        if (!(va_ == vb_)) {                                                                       \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s == %s (%#llx vs %#llx)\n", __FILE__, __LINE__, #a, #b, \
                    (unsigned long long)va_, (unsigned long long)vb_);                             \
        }                                                                                          \
    } while (0)

// ---------------------------------------------------------------------------
// The host: the render function the shim attaches, and the rate it is told.
// ---------------------------------------------------------------------------
static HostSynthRender g_render = nullptr;
static void *g_render_context = nullptr;
static double g_rate = 48000.0;
static std::atomic<int> g_attaches{0};

extern "C" void host_synth_attach(HostSynthRender render, void *context) {
    g_render = render;
    g_render_context = context;
    ++g_attaches;
}
extern "C" double host_synth_rate(void) {
    return g_rate;
}

struct Audio {
    std::vector<float> left, right;
    double rate = 48000.0;
    size_t frames() const {
        return left.size();
    }
};

// Renders `seconds` more of the mix at `rate`, in blocks a host would use.
static void render(Audio &a, double seconds, double rate = 48000.0, uint32_t block = 512) {
    a.rate = rate;
    const size_t frames = (size_t)lround(seconds * rate);
    const size_t base = a.left.size();
    a.left.resize(base + frames, 0.0f);
    a.right.resize(base + frames, 0.0f);
    for (size_t at = 0; at < frames;) {
        const uint32_t n = (uint32_t)std::min<size_t>(block, frames - at);
        if (g_render)
            g_render(g_render_context, a.left.data() + base + at, a.right.data() + base + at, n,
                     rate);
        at += n;
    }
}

// The level of [t0, t1) in dB of full scale, both channels.
static double rms_db(const Audio &a, double t0, double t1) {
    size_t i0 = (size_t)(t0 * a.rate), i1 = std::min((size_t)(t1 * a.rate), a.frames());
    double sum = 0.0;
    for (size_t i = i0; i < i1; ++i)
        sum += 0.5 * ((double)a.left[i] * a.left[i] + (double)a.right[i] * a.right[i]);
    const double rms = i1 > i0 ? sqrt(sum / (double)(i1 - i0)) : 0.0;
    return 20.0 * log10(rms + 1e-12);
}

// The magnitude of one frequency over [t0, t1) (Goertzel, left + right).
static double tone(const Audio &a, double t0, double t1, double hz) {
    size_t i0 = (size_t)(t0 * a.rate), i1 = std::min((size_t)(t1 * a.rate), a.frames());
    const double w = 2.0 * M_PI * hz / a.rate, k = 2.0 * cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (size_t i = i0; i < i1; ++i) {
        const double s = 0.5 * (a.left[i] + a.right[i]) + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double n = (double)(i1 > i0 ? i1 - i0 : 1);
    return sqrt(s1 * s1 + s2 * s2 - k * s1 * s2) / n;
}

// Which of the fixtures' pitches sounds over [t0, t1): 441, 660, 882 or
// 1102 Hz, or 0 when the window is quiet.
static int pitch(const Audio &a, double t0, double t1) {
    static const double kPitches[4] = {441.0, 441.0 * 1.4983070768766815, 882.0, 1102.5};
    if (rms_db(a, t0, t1) < -60.0)
        return 0;
    int best = 0;
    double level = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double m = tone(a, t0, t1, kPitches[i]);
        if (m > level) {
            level = m;
            best = i;
        }
    }
    static const int kNames[4] = {441, 660, 882, 1102};
    return kNames[best];
}

// The first time from `from` at which `hz` sounds, in 10 ms windows, or -1.
static double first_time(const Audio &a, double from, int hz) {
    for (double t = from; t + 0.01 <= (double)a.frames() / a.rate; t += 0.005)
        if (pitch(a, t, t + 0.01) == hz)
            return t;
    return -1.0;
}

// ---------------------------------------------------------------------------
// GUIDs and guest scratch.
// ---------------------------------------------------------------------------
struct G {
    uint8_t b[16];
};
static G guid(uint32_t d1, uint16_t d2, uint16_t d3, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
              uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7) {
    return G{{(uint8_t)d1, (uint8_t)(d1 >> 8), (uint8_t)(d1 >> 16), (uint8_t)(d1 >> 24),
              (uint8_t)d2, (uint8_t)(d2 >> 8), (uint8_t)d3, (uint8_t)(d3 >> 8), b0, b1, b2, b3, b4,
              b5, b6, b7}};
}
static G dm(uint8_t low) {
    return guid(0xd2ac2800u | low, 0xb39b, 0x11d1, 0x87, 0x04, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
}
static const G CLSID_Loader = dm(0x92), CLSID_Composer = dm(0x90), CLSID_Performance = dm(0x81),
               CLSID_Segment = dm(0x82), CLSID_Style = dm(0xa3);
static const G IID_Loader =
    guid(0x2ffaaca2, 0x5dca, 0x11d2, 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6);
static const G IID_Composer = dm(0xbf);
static const G IID_Performance =
    guid(0x07d43d03, 0x6523, 0x11d2, 0x87, 0x1d, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const G IID_Segment =
    guid(0xf96029a2, 0x4282, 0x11d2, 0x87, 0x17, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const G IID_KsControl =
    guid(0x28f54685, 0x06fd, 0x11d2, 0xb2, 0x7a, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96);
static const G IID_Unknown = guid(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
static const G GUID_AllTypes = dm(0x93), GUID_Style = dm(0xa1), GUID_Download = dm(0xa7),
               GUID_Unload = dm(0xa8), GUID_MasterVolume = dm(0xb1), GUID_MasterGroove = dm(0xb2),
               GUID_NotifySegment = dm(0x99);
static const G GUID_AutoDownload =
    guid(0xfb09565b, 0x3631, 0x11d2, 0xbc, 0xb8, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
static const G GUID_NotifyPerformance =
    guid(0x81f75bc5, 0x4e5d, 0x11d2, 0xbc, 0xc7, 0x00, 0xa0, 0xc9, 0x22, 0xe6, 0xeb);
static const G PROP_Volume =
    guid(0xfedfae25, 0xe46e, 0x11d1, 0xaa, 0xce, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12);
static const G PROP_Effects =
    guid(0xcda8d611, 0x684a, 0x11d2, 0x87, 0x1e, 0x00, 0x60, 0x08, 0x93, 0xb1, 0xbd);
static const G PROP_WavesReverb =
    guid(0x04cb5622, 0x32e5, 0x11d2, 0xaf, 0xa6, 0x00, 0xaa, 0x00, 0x24, 0xd8, 0xb6);
static const G PROP_Unknown =
    guid(0x178f2f24, 0xc364, 0x11d1, 0xa7, 0x60, 0x00, 0x00, 0xf8, 0x75, 0xac, 0x12);

static const uint32_t S_OK_ = 0, S_FALSE_ = 1, REGDB_E_CLASSNOTREG_ = 0x80040154u;
static const uint32_t DMUS_E_NOT_FOUND_ = 0x88781193u, DMUS_E_NOT_INIT_ = 0x88781194u,
                      DMUS_E_LOADER_FAILEDOPEN_ = 0x887811a3u, DMUS_E_LOADER_BADPATH_ = 0x887811a2u,
                      DMUS_E_LOADER_FORMATNOTSUPPORTED_ = 0x887811a4u,
                      DMUS_E_UNKNOWN_PROPERTY_ = 0x88781122u;

// Slots, from the DirectX 6.1/7 headers.
enum {
    QI = 0,
    ADDREF = 1,
    RELEASE = 2,
    LOADER_GETOBJECT = 3,
    LOADER_SETSEARCHDIRECTORY = 5,
    LOADER_CACHEOBJECT = 7,
    LOADER_RELEASEOBJECT = 8,
    LOADER_CLEARCACHE = 9,
    LOADER_ENABLECACHE = 10,
    COMPOSER_AUTOTRANSITION = 6,
    PERF_INIT = 3,
    PERF_PLAYSEGMENT = 4,
    PERF_STOP = 5,
    PERF_GETSEGMENTSTATE = 6,
    PERF_ISPLAYING = 14,
    PERF_GETTIME = 15,
    PERF_ALLOCPMSG = 16,
    PERF_FREEPMSG = 17,
    PERF_SETNOTIFICATIONHANDLE = 20,
    PERF_GETNOTIFICATIONPMSG = 21,
    PERF_ADDNOTIFICATIONTYPE = 22,
    PERF_ADDPORT = 24,
    PERF_ASSIGNPCHANNELBLOCK = 26,
    PERF_GETGLOBALPARAM = 33,
    PERF_SETGLOBALPARAM = 34,
    PERF_CLOSEDOWN = 38,
    DM_ENUMPORT = 3,
    DM_CREATEPORT = 5,
    DM_ACTIVATE = 9,
    DM_GETDEFAULTPORT = 10,
    PORT_GETCAPS = 11,
    PORT_SETNUMCHANNELGROUPS = 13,
    PORT_GETNUMCHANNELGROUPS = 14,
    PORT_ACTIVATE = 15,
    PORT_GETFORMAT = 19,
    KS_PROPERTY = 3,
    SEG_GETLENGTH = 3,
    SEG_GETREPEATS = 5,
    SEG_SETREPEATS = 6,
    SEG_GETPARAM = 18,
    SEG_SETPARAM = 19,
    STATE_GETREPEATS = 3,
    STATE_GETSEGMENT = 4,
    STYLE_ENUMMOTIF = 6,
};

static uint32_t put(uint32_t at, const G &g) {
    memcpy(g_mem + at, g.b, 16);
    return at;
}
// One GUID slot per use, so two GUID arguments of one call do not collide.
static uint32_t gp(const G &g, int slot = 0) {
    return put(sc(0x3000 + 16 * (uint32_t)slot), g);
}

static std::string g_dir;       // the fixtures on disk
static std::string g_guest_dir; // the same, as the guest names it

static void write_fixtures() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/recomp-dmusic-XXXXXX", os_temp_dir());
    if (os_mkdtemp(dir) != 0) {
        CHECK(!"mkdtemp");
        return;
    }
    g_dir = dir;
    for (const DmFixture &f : kDmFixtures) {
        FILE *out = fopen((g_dir + "/" + f.name).c_str(), "wb");
        CHECK(out != nullptr);
        if (!out)
            continue;
        fwrite(f.data, 1, f.size, out);
        fclose(out);
    }
    win32_init(g_dir);
    g_guest_dir = win32_guest_path(g_dir);
}

// ---------------------------------------------------------------------------
// A game's DirectMusic, set up the way a DirectX 6/7 game sets it up.
// ---------------------------------------------------------------------------
struct Music {
    uint32_t loader = 0, composer = 0, perf = 0, dm = 0, port = 0, ks = 0;
};

static uint32_t cocreate(const G &clsid, const G &iid) {
    wr32(sc(0x10), 0);
    uint32_t hr = call_shim(tramp("ole32.dll", "CoCreateInstance"),
                            {gp(clsid, 0), 0, 3, gp(iid, 1), sc(0x10)});
    CHECK_EQ(hr, S_OK_);
    return rd32(sc(0x10));
}

static uint32_t ks_property(uint32_t ks, const G &set, uint32_t flags, uint32_t data,
                            uint32_t size) {
    const uint32_t prop = sc(0x200);
    put(prop, set);
    wr32(prop + 16, 0);
    wr32(prop + 20, flags);
    return call_method(ks, KS_PROPERTY, {prop, 0x18, data, size, sc(0x220)});
}

static void set_volume_mb(const Music &m, int32_t mb) {
    wr32(sc(0x230), (uint32_t)mb);
    CHECK_EQ(ks_property(m.ks, PROP_Volume, 2, sc(0x230), 4), S_OK_);
}

static void set_reverb(const Music &m, bool on, float in_gain, float mix, float time_ms,
                       float hf_ratio) {
    wr32(sc(0x240), on ? 1 : 0);
    CHECK_EQ(ks_property(m.ks, PROP_Effects, 2, sc(0x240), 4), S_OK_);
    const float p[4] = {in_gain, mix, time_ms, hf_ratio};
    memcpy(g_mem + sc(0x250), p, 16);
    CHECK_EQ(ks_property(m.ks, PROP_WavesReverb, 2, sc(0x250), 16), S_OK_);
}

static void set_groove(const Music &m, int8_t level) {
    wr8(sc(0x260), (uint8_t)level);
    CHECK_EQ(call_method(m.perf, PERF_SETGLOBALPARAM, {gp(GUID_MasterGroove), sc(0x260), 1}),
             S_OK_);
}

static Music open_music() {
    Music m;
    m.loader = cocreate(CLSID_Loader, IID_Loader);
    m.composer = cocreate(CLSID_Composer, IID_Composer);
    m.perf = cocreate(CLSID_Performance, IID_Performance);
    CHECK(m.loader && m.composer && m.perf);
    if (!m.perf)
        return m;
    wr32(sc(0x20), 0);
    CHECK_EQ(call_method(m.perf, PERF_INIT, {sc(0x20), 0, 0}), S_OK_);
    m.dm = rd32(sc(0x20));
    CHECK(m.dm != 0);

    // The default port with one channel group (DMUS_PORTPARAMS8).
    CHECK_EQ(call_method(m.dm, DM_GETDEFAULTPORT, {sc(0x30)}), S_OK_);
    const uint32_t params = sc(0x40);
    gm_zero(params, 0x24);
    wr32(params, 0x24);
    wr32(params + 4, 0x2); // DMUS_PORTPARAMS_CHANNELGROUPS
    wr32(params + 12, 1);
    wr32(sc(0x70), 0);
    CHECK_EQ(call_method(m.dm, DM_CREATEPORT, {sc(0x30), params, sc(0x70), 0}), S_OK_);
    m.port = rd32(sc(0x70));
    CHECK(m.port != 0);
    // Its caps: an output-class DLS port with room for 8 channel groups.
    const uint32_t caps = sc(0x400);
    gm_zero(caps, 0x134);
    wr32(caps, 0x134);
    CHECK_EQ(call_method(m.port, PORT_GETCAPS, {caps}), S_OK_);
    CHECK_EQ(rd32(caps + 0x18), 1u);                            // DMUS_PC_OUTPUTCLASS
    CHECK_EQ(rd32(caps + 4) & 1, 1u);                           // DMUS_PC_DLS
    CHECK(rd32(caps + 0x24) >= 8);                              // dwMaxChannelGroups
    CHECK(memcmp(g_mem + caps + 8, g_mem + sc(0x30), 16) == 0); // the default port's GUID
    CHECK_EQ(call_method(m.port, PORT_SETNUMCHANNELGROUPS, {8}), S_OK_);
    CHECK_EQ(call_method(m.port, PORT_GETNUMCHANNELGROUPS, {sc(0x80)}), S_OK_);
    CHECK_EQ(rd32(sc(0x80)), 8u);
    CHECK_EQ(call_method(m.port, PORT_ACTIVATE, {1}), S_OK_);
    CHECK_EQ(call_method(m.perf, PERF_ADDPORT, {m.port}), S_OK_);
    for (uint32_t i = 0; i < 8; ++i)
        CHECK_EQ(call_method(m.perf, PERF_ASSIGNPCHANNELBLOCK, {i, m.port, i + 1}), S_OK_);
    CHECK_EQ(call_method(m.dm, DM_ACTIVATE, {1}), S_OK_);
    CHECK_EQ(call_method(m.perf, PERF_SETNOTIFICATIONHANDLE, {0x1234, 0, 0}), S_OK_);
    CHECK_EQ(call_method(m.perf, PERF_ADDNOTIFICATIONTYPE, {gp(GUID_NotifyPerformance)}), S_OK_);
    CHECK_EQ(call_method(m.perf, PERF_ADDNOTIFICATIONTYPE, {gp(GUID_NotifySegment)}), S_OK_);
    wr32(sc(0x90), 0);
    CHECK_EQ(call_method(m.perf, PERF_SETGLOBALPARAM, {gp(GUID_AutoDownload), sc(0x90), 4}), S_OK_);
    CHECK_EQ(call_method(m.loader, LOADER_ENABLECACHE, {gp(GUID_AllTypes), 1}), S_FALSE_);

    // IKsControl on the port, for the synthesizer's volume and reverb.
    wr32(sc(0xa0), 0);
    CHECK_EQ(call_method(m.port, QI, {gp(IID_KsControl), sc(0xa0)}), S_OK_);
    m.ks = rd32(sc(0xa0));
    CHECK(m.ks != 0);

    // The search directory, as a wide string.
    gm_put_wstr(sc(0x600), g_guest_dir, 260);
    CHECK_EQ(call_method(m.loader, LOADER_SETSEARCHDIRECTORY, {gp(GUID_AllTypes), sc(0x600), 0}),
             S_OK_);
    return m;
}

static void close_music(Music &m) {
    if (!m.perf)
        return;
    call_method(m.perf, PERF_STOP, {0, 0, 0, 0});
    CHECK_EQ(call_method(m.perf, PERF_CLOSEDOWN, {}), S_OK_);
    for (uint32_t *p : {&m.ks, &m.port, &m.dm, &m.loader, &m.perf, &m.composer})
        if (*p) {
            call_method(*p, RELEASE, {});
            *p = 0;
        }
}

// GetObject of a segment by file name.
static uint32_t load(const Music &m, const char *file, uint32_t *hr_out = nullptr) {
    const uint32_t desc = sc(0x1000);
    gm_zero(desc, 0x350);
    wr32(desc, 0x350);
    wr32(desc + 4, 0x2 | 0x10); // DMUS_OBJ_CLASS | DMUS_OBJ_FILENAME
    put(desc + 0x18, CLSID_Segment);
    gm_put_wstr(desc + 0x138, file, 260);
    wr32(sc(0xb0), 0);
    uint32_t hr = call_method(m.loader, LOADER_GETOBJECT, {desc, gp(IID_Segment), sc(0xb0)});
    if (hr_out)
        *hr_out = hr;
    return hr == S_OK_ ? rd32(sc(0xb0)) : 0;
}

// What a game does with a freshly loaded score: download, repeats, play.
static uint32_t play(const Music &m, uint32_t seg, uint32_t repeats, uint32_t flags = 0x4000) {
    CHECK_EQ(call_method(seg, SEG_SETPARAM, {gp(GUID_Download), 0xffffffffu, 0, 0, m.perf}), S_OK_);
    CHECK_EQ(call_method(seg, SEG_SETREPEATS, {repeats}), S_OK_);
    wr32(sc(0xc0), 0);
    CHECK_EQ(call_method(m.perf, PERF_PLAYSEGMENT, {seg, flags, 0, 0, sc(0xc0)}), S_OK_);
    return rd32(sc(0xc0));
}

static uint32_t segment_state(const Music &m, uint32_t *hr_out = nullptr) {
    CHECK_EQ(call_method(m.perf, PERF_GETTIME, {0, sc(0xd0)}), S_OK_);
    wr32(sc(0xd4), 0);
    uint32_t hr = call_method(m.perf, PERF_GETSEGMENTSTATE, {sc(0xd4), rd32(sc(0xd0))});
    if (hr_out)
        *hr_out = hr;
    return hr == S_OK_ ? rd32(sc(0xd4)) : 0;
}

// ===========================================================================
// The tests
// ===========================================================================

// The classes exist only when the game asks for them.
static void test_registration() {
    wr32(sc(0x10), 0xdeadbeef);
    uint32_t hr = call_shim(tramp("ole32.dll", "CoCreateInstance"),
                            {gp(CLSID_Performance, 0), 0, 3, gp(IID_Performance, 1), sc(0x10)});
    CHECK_EQ(hr, REGDB_E_CLASSNOTREG_);
    CHECK_EQ(rd32(sc(0x10)), 0u);
    dmusic_enable_for_test(1);
    dmusic_register();
    CHECK(dmusic_enabled());
    uint32_t perf = cocreate(CLSID_Performance, IID_Performance);
    CHECK(perf != 0);
    // A performance asked for the loader's interface is refused.
    wr32(sc(0x14), 0xdeadbeef);
    CHECK_EQ(call_method(perf, QI, {gp(IID_Loader), sc(0x14)}), 0x80004002u);
    CHECK_EQ(rd32(sc(0x14)), 0u);
    CHECK_EQ(call_method(perf, RELEASE, {}), 0u);
}

// Every slot is where the DirectX headers put it, by name.
static void test_slots() {
    Music m = open_music();
    struct Slot {
        uint32_t *iface;
        uint32_t slot;
        const char *name;
    } slots[] = {
        {&m.loader, LOADER_GETOBJECT, "IDirectMusicLoader::GetObject"},
        {&m.loader, 11, "IDirectMusicLoader::EnumObject"},
        {&m.composer, COMPOSER_AUTOTRANSITION, "IDirectMusicComposer::AutoTransition"},
        {&m.perf, PERF_GETTIME, "IDirectMusicPerformance::GetTime"},
        {&m.perf, PERF_SETGLOBALPARAM, "IDirectMusicPerformance::SetGlobalParam"},
        {&m.perf, PERF_CLOSEDOWN, "IDirectMusicPerformance::CloseDown"},
        {&m.perf, 43, "IDirectMusicPerformance::RhythmToTime"},
        {&m.dm, DM_GETDEFAULTPORT, "IDirectMusic::GetDefaultPort"},
        {&m.dm, 11, "IDirectMusic::SetDirectSound"},
        {&m.port, PORT_ACTIVATE, "IDirectMusicPort::Activate"},
        {&m.port, PORT_GETFORMAT, "IDirectMusicPort::GetFormat"},
        {&m.ks, KS_PROPERTY, "IKsControl::KsProperty"},
    };
    for (const Slot &s : slots) {
        const uint32_t vtbl = rd32(*s.iface);
        const char *d = imports_describe(rd32(vtbl + 4 * s.slot));
        CHECK(d && strstr(d, s.name) != nullptr);
        if (!d || !strstr(d, s.name))
            fprintf(stderr, "  slot %u is %s, expected %s\n", s.slot, d ? d : "(null)", s.name);
    }
    uint32_t seg = load(m, "Song.sgt");
    CHECK(seg != 0);
    if (seg) {
        const uint32_t vtbl = rd32(seg);
        const char *d = imports_describe(rd32(vtbl + 4 * SEG_SETPARAM));
        CHECK(d && strstr(d, "IDirectMusicSegment::SetParam"));
        call_method(seg, RELEASE, {});
    }
    // The port and its IKsControl are one object: one IUnknown.
    CHECK_EQ(call_method(m.port, QI, {gp(IID_Unknown), sc(0xe0)}), S_OK_);
    CHECK_EQ(call_method(m.ks, QI, {gp(IID_Unknown), sc(0xe4)}), S_OK_);
    CHECK_EQ(rd32(sc(0xe0)), rd32(sc(0xe4)));
    call_method(rd32(sc(0xe0)), RELEASE, {});
    call_method(rd32(sc(0xe4)), RELEASE, {});
    close_music(m);
}

// The loader: search directory, case-blind names, the cache, memory, errors.
static void test_loader() {
    Music m = open_music();
    // A directory that is not there is refused.
    gm_put_wstr(sc(0x600), "C:\\No Such Folder\\", 260);
    CHECK_EQ(call_method(m.loader, LOADER_SETSEARCHDIRECTORY, {gp(GUID_AllTypes), sc(0x600), 0}),
             DMUS_E_LOADER_BADPATH_);
    // The same directory again is S_FALSE.
    gm_put_wstr(sc(0x600), g_guest_dir, 260);
    CHECK_EQ(call_method(m.loader, LOADER_SETSEARCHDIRECTORY, {gp(GUID_AllTypes), sc(0x600), 0}),
             S_FALSE_);

    uint32_t a = load(m, "song.SGT");
    uint32_t b = load(m, "Song.sgt");
    CHECK(a != 0);
    CHECK_EQ(a, b); // cached: the same object
    uint32_t hr = 0;
    CHECK_EQ(load(m, "Missing.sgt", &hr), 0u);
    CHECK_EQ(hr, DMUS_E_LOADER_FAILEDOPEN_);
    // A style is not a segment.
    const uint32_t desc = sc(0x1000);
    put(desc + 0x18, CLSID_Style);
    CHECK_EQ(call_method(m.loader, LOADER_GETOBJECT, {desc, gp(IID_Segment), sc(0xb0)}),
             DMUS_E_LOADER_FORMATNOTSUPPORTED_);

    // The segment's header and its style, which has no motifs.
    CHECK_EQ(call_method(a, SEG_GETLENGTH, {sc(0xb4)}), S_OK_);
    CHECK_EQ(rd32(sc(0xb4)), 4u * 3072u);
    CHECK_EQ(call_method(a, SEG_GETREPEATS, {sc(0xb4)}), S_OK_);
    CHECK_EQ(rd32(sc(0xb4)), 0u);
    wr32(sc(0xb8), 0);
    CHECK_EQ(call_method(a, SEG_GETPARAM, {gp(GUID_Style), 0xffffffffu, 0, 0, 0, sc(0xb8)}), S_OK_);
    const uint32_t style = rd32(sc(0xb8));
    CHECK(style != 0);
    if (style) {
        CHECK_EQ(call_method(style, STYLE_ENUMMOTIF, {0, sc(0x800)}), S_FALSE_);
        call_method(style, RELEASE, {});
    }

    // Caching off: a new object each time, and the cached one let go.
    CHECK_EQ(call_method(m.loader, LOADER_ENABLECACHE, {gp(GUID_AllTypes), 0}), S_OK_);
    uint32_t c = load(m, "Song.sgt");
    CHECK(c != 0 && c != a);
    // CacheObject and ReleaseObject.
    CHECK_EQ(call_method(m.loader, LOADER_CACHEOBJECT, {c}), S_OK_);
    CHECK_EQ(call_method(m.loader, LOADER_CACHEOBJECT, {c}), S_FALSE_);
    CHECK_EQ(call_method(m.loader, LOADER_RELEASEOBJECT, {c}), S_OK_);
    CHECK_EQ(call_method(m.loader, LOADER_RELEASEOBJECT, {c}), S_FALSE_);
    CHECK_EQ(call_method(m.loader, LOADER_CLEARCACHE, {gp(GUID_AllTypes)}), S_OK_);

    // From memory: DMUS_OBJ_MEMORY with the bytes in guest memory.
    const DmFixture *song = nullptr;
    for (const DmFixture &f : kDmFixtures)
        if (!strcmp(f.name, "Song.sgt"))
            song = &f;
    const uint32_t bytes = heap_alloc((uint32_t)song->size, false);
    memcpy(g_mem + bytes, song->data, song->size);
    gm_zero(desc, 0x350);
    wr32(desc, 0x350);
    wr32(desc + 4, 0x2 | 0x400); // DMUS_OBJ_CLASS | DMUS_OBJ_MEMORY
    put(desc + 0x18, CLSID_Segment);
    wr32(desc + 0x340, (uint32_t)song->size);
    wr32(desc + 0x348, bytes);
    wr32(sc(0xb0), 0);
    CHECK_EQ(call_method(m.loader, LOADER_GETOBJECT, {desc, gp(IID_Segment), sc(0xb0)}), S_OK_);
    uint32_t d = rd32(sc(0xb0));
    CHECK(d != 0);
    if (d) {
        CHECK_EQ(call_method(d, SEG_GETLENGTH, {sc(0xb4)}), S_OK_);
        CHECK_EQ(rd32(sc(0xb4)), 4u * 3072u);
    }
    heap_free(bytes);
    for (uint32_t s : {a, b, c, d})
        if (s)
            call_method(s, RELEASE, {});
    close_music(m);
}

// Playing: the groove-1 pattern, the state, the segment it names.
static void test_play() {
    Music m = open_music();
    uint32_t seg = load(m, "Song.sgt");
    uint32_t state = play(m, seg, 100);
    CHECK(state != 0);
    Audio a;
    render(a, 1.5);
    CHECK(rms_db(a, 0.1, 0.9) > -30.0);
    CHECK_EQ(pitch(a, 0.1, 0.9), 441);
    // The playing state names the segment by the pointer the loader gave.
    uint32_t current = segment_state(m);
    CHECK(current != 0);
    if (current) {
        wr32(sc(0xe8), 0);
        CHECK_EQ(call_method(current, STATE_GETSEGMENT, {sc(0xe8)}), S_OK_);
        CHECK_EQ(rd32(sc(0xe8)), seg);
        call_method(seg, RELEASE, {}); // GetSegment's reference
        CHECK_EQ(call_method(current, STATE_GETREPEATS, {sc(0xec)}), S_OK_);
        CHECK_EQ(rd32(sc(0xec)), 100u);
        call_method(current, RELEASE, {});
    }
    CHECK_EQ(call_method(m.perf, PERF_ISPLAYING, {seg, 0}), S_OK_);
    // GetTime moves on.
    CHECK_EQ(call_method(m.perf, PERF_GETTIME, {sc(0xf0), sc(0xf8)}), S_OK_);
    const uint64_t rt0 = rd32(sc(0xf0)) | ((uint64_t)rd32(sc(0xf4)) << 32);
    os_sleep_us(20000);
    CHECK_EQ(call_method(m.perf, PERF_GETTIME, {sc(0xf0), sc(0xfc)}), S_OK_);
    const uint64_t rt1 = rd32(sc(0xf0)) | ((uint64_t)rd32(sc(0xf4)) << 32);
    CHECK(rt1 > rt0 + 100000); // 10 ms in 100 ns units
    CHECK((int32_t)rd32(sc(0xfc)) > (int32_t)rd32(sc(0xf8)));
    if (state)
        call_method(state, RELEASE, {});
    call_method(seg, RELEASE, {});
    close_music(m);
}

// Stop ends the primary segment at once for the state queries: a game's
// unload loop, which retries while the score still plays, gets through on
// its first try.
static void test_stop_and_unload() {
    Music m = open_music();
    set_reverb(m, false, 0, -10, 1000, 0.001f); // no tail after the stop
    uint32_t seg = load(m, "Song.sgt");
    uint32_t state = play(m, seg, 100000);
    Audio a;
    render(a, 0.5);
    uint32_t hr = 0;
    uint32_t current = segment_state(m, &hr);
    CHECK_EQ(hr, S_OK_);
    if (current)
        call_method(current, RELEASE, {});
    CHECK_EQ(call_method(m.perf, PERF_STOP, {0, 0, 0, 0}), S_OK_);
    int tries = 0;
    for (; tries < 100; ++tries) {
        segment_state(m, &hr);
        if (hr == DMUS_E_NOT_FOUND_)
            break;
    }
    CHECK_EQ(tries, 0);
    CHECK_EQ(call_method(m.perf, PERF_ISPLAYING, {seg, 0}), S_FALSE_);
    // Unloading and releasing the score afterwards.
    CHECK_EQ(call_method(seg, SEG_SETPARAM, {gp(GUID_Unload), 0xffffffffu, 0, 0, 0x12345678}),
             S_OK_);
    render(a, 0.5);
    CHECK(rms_db(a, 0.7, 1.0) < -80.0); // silent after the stop
    if (state)
        call_method(state, RELEASE, {});
    call_method(seg, RELEASE, {});
    close_music(m);
}

// Repeats: four bars and silence; with one repeat, eight.
static void test_repeats() {
    Music m = open_music();
    set_reverb(m, false, 0, -10, 1000, 0.001f); // the end is silence, not a tail
    uint32_t seg = load(m, "Song.sgt");
    uint32_t state = play(m, seg, 0);
    Audio a;
    render(a, 4.6);
    CHECK_EQ(pitch(a, 3.2, 3.8), 441);
    CHECK(rms_db(a, 4.2, 4.6) < -80.0);
    uint32_t hr = 0;
    segment_state(m, &hr);
    CHECK_EQ(hr, DMUS_E_NOT_FOUND_);
    CHECK_EQ(call_method(m.perf, PERF_ISPLAYING, {seg, 0}), S_FALSE_);
    if (state)
        call_method(state, RELEASE, {});

    state = play(m, seg, 1);
    Audio b;
    render(b, 6.5);
    CHECK_EQ(pitch(b, 5.2, 5.8), 441); // in the repeat
    uint32_t current = segment_state(m, &hr);
    CHECK_EQ(hr, S_OK_);
    if (current)
        call_method(current, RELEASE, {});
    render(b, 2.3);
    CHECK(rms_db(b, 8.3, 8.8) < -80.0);
    segment_state(m, &hr);
    CHECK_EQ(hr, DMUS_E_NOT_FOUND_);
    if (state)
        call_method(state, RELEASE, {});
    call_method(seg, RELEASE, {});
    close_music(m);
}

// The tempo: four bars at 240 beats a minute are four seconds to the sample,
// whatever size of block the host renders in.
static void test_tempo() {
    Music m = open_music();
    set_reverb(m, false, 0, -10, 1000, 0.001f);
    uint32_t seg = load(m, "Song.sgt");
    for (uint32_t block : {64u, 333u, 512u, 4800u}) {
        uint32_t state = play(m, seg, 100);
        if (state)
            call_method(state, RELEASE, {});
        Audio a;
        render(a, 4.2, 48000.0, block);
        // Each bar's note starts after the breath that ends the one before.
        int onsets = 0;
        for (int bar = 1; bar <= 4; ++bar) {
            const size_t from = (size_t)((bar - 0.01) * a.rate),
                         to = (size_t)((bar + 0.01) * a.rate);
            size_t at = 0;
            for (size_t i = from; i < to && i < a.frames(); ++i)
                if (fabsf(a.left[i]) > 1e-3f) {
                    at = i;
                    break;
                }
            if (!at)
                continue;
            ++onsets;
            const double error_ms = ((double)at / a.rate - bar) * 1000.0;
            if (fabs(error_ms) > 1.0)
                fprintf(stderr, "  %u-frame blocks: bar %d starts %.2f ms off\n", block, bar + 1,
                        error_ms);
            CHECK(fabs(error_ms) <= 1.0);
        }
        CHECK_EQ(onsets, 4);
        call_method(m.perf, PERF_STOP, {0, 0, 0, 0});
        Audio settle;
        render(settle, 0.1);
    }
    call_method(seg, RELEASE, {});
    close_music(m);
}

// The master groove level and AutoTransition: a groove change asked for in
// the middle of a bar sounds from the next bar line; a change of the master
// groove alone sounds from the next pattern the style chooses.
static void test_groove() {
    Music m = open_music();
    uint32_t seg = load(m, "Song.sgt");
    set_groove(m, 0);
    uint32_t state = play(m, seg, 100);
    if (state)
        call_method(state, RELEASE, {});
    Audio a;
    render(a, 1.3);
    CHECK_EQ(pitch(a, 0.1, 1.2), 441);
    // Intensity up: groove 1 + 1, at the next measure.
    set_groove(m, 1);
    wr32(sc(0x110), 0);
    CHECK_EQ(call_method(m.composer, COMPOSER_AUTOTRANSITION,
                         {m.perf, seg, 0 /* GROOVE */, 0x20 /* MEASURE */, 0, 0, sc(0x110), 0}),
             S_OK_);
    const uint32_t to_state = rd32(sc(0x110));
    CHECK(to_state != 0);
    if (to_state)
        call_method(to_state, RELEASE, {});
    render(a, 1.2); // to 2.5 s
    const double switched = first_time(a, 1.3, 660);
    printf("  (groove 2 from %.3f s; the bar line is at 2.000 s)\n", switched);
    CHECK(switched > 1.985 && switched < 2.01);
    CHECK_EQ(pitch(a, 1.35, 1.95), 441); // not before the bar line
    CHECK_EQ(pitch(a, 2.05, 2.45), 660);
    // The master groove alone: the segment restarts after the one-bar
    // transition, at 3 s, and chooses groove 1 + 2 there.
    set_groove(m, 2);
    render(a, 1.5); // to 4.0 s
    CHECK_EQ(pitch(a, 2.55, 2.95), 660);
    const double high = first_time(a, 2.5, 882);
    printf("  (groove 3 from %.3f s; the next pattern starts at 3.000 s)\n", high);
    CHECK(high > 2.985 && high < 3.01);
    // A negative level keeps groove 1, the lowest there is.
    set_groove(m, -5);
    wr8(sc(0x260), 0x55);
    CHECK_EQ(call_method(m.perf, PERF_GETGLOBALPARAM, {gp(GUID_MasterGroove), sc(0x260), 1}),
             S_OK_);
    CHECK_EQ(rd8(sc(0x260)), (uint8_t)(int8_t)-5);
    CHECK_EQ(call_method(m.composer, COMPOSER_AUTOTRANSITION, {m.perf, seg, 0, 0x20, 0, 0, 0, 0}),
             S_OK_);
    render(a, 1.5);
    CHECK_EQ(pitch(a, 5.05, 5.45), 441);
    call_method(seg, RELEASE, {});
    close_music(m);
}

// Volume: the port's KS volume in hundredths of a dB, ramped; a game's fade
// in steps; silence at its floor; the performance's master volume on top.
static void test_volume() {
    Music m = open_music();
    set_reverb(m, false, 0, -10, 1000, 0.001f);
    uint32_t seg = load(m, "Song.sgt");
    uint32_t state = play(m, seg, 100);
    if (state)
        call_method(state, RELEASE, {});
    Audio a;
    render(a, 1.0);
    const double full = rms_db(a, 0.1, 0.9);
    set_volume_mb(m, -600);
    render(a, 1.0);
    const double down = rms_db(a, 1.1, 1.9);
    printf("  (0 mB %.2f dB, -600 mB %.2f dB)\n", full, down);
    CHECK(fabs((full - down) - 6.0) < 0.3);
    // GET reads back what was set.
    CHECK_EQ(ks_property(m.ks, PROP_Volume, 1, sc(0x230), 4), S_OK_);
    CHECK_EQ((int32_t)rd32(sc(0x230)), -600);
    CHECK_EQ(rd32(sc(0x220)), 4u);
    // An unknown property set is refused.
    CHECK_EQ(ks_property(m.ks, PROP_Unknown, 1, sc(0x230), 4), DMUS_E_UNKNOWN_PROPERTY_);

    // A fade out the way a game does it: SetVolume(v) in steps of 1/30 every
    // 100 ms, as 1000 ln v hundredths of a dB, and the floor at -20000.
    set_volume_mb(m, 0);
    render(a, 0.2);
    std::vector<double> steps;
    for (int i = 0; i <= 19; ++i) {
        const double v = 0.62 - i / 30.0;
        set_volume_mb(m, v <= 0.01 ? -20000 : (int32_t)lround(1000.0 * log(v)));
        const double t = (double)a.frames() / a.rate;
        render(a, 0.1);
        // Clear of the breath at the end of each bar (the last 23 ms).
        steps.push_back(rms_db(a, t + 0.01, t + 0.07));
    }
    bool falling = true;
    for (size_t i = 1; i < steps.size(); ++i)
        if (steps[i] > steps[i - 1] + 0.3)
            falling = false;
    CHECK(falling);
    if (!falling)
        for (size_t i = 0; i < steps.size(); ++i)
            fprintf(stderr, "  fade step %zu: %.2f dB\n", i, steps[i]);
    // At the floor, once the ramp has run out: silence.
    double t = (double)a.frames() / a.rate;
    render(a, 0.2);
    CHECK(rms_db(a, t + 0.1, t + 0.2) < -90.0);
    // The master volume adds to the port's.
    set_volume_mb(m, 0);
    wr32(sc(0x270), (uint32_t)-600);
    CHECK_EQ(call_method(m.perf, PERF_SETGLOBALPARAM, {gp(GUID_MasterVolume), sc(0x270), 4}),
             S_OK_);
    t = (double)a.frames() / a.rate;
    render(a, 1.0);
    const double master = rms_db(a, t + 0.1, t + 0.9);
    printf("  (master -600 mB: %.2f dB)\n", master);
    CHECK(fabs((full - master) - 6.0) < 0.5);
    call_method(seg, RELEASE, {});
    close_music(m);
}

// The reverb: a tail after a stop when it is on, none when it is off.
static void test_reverb() {
    Music m = open_music();
    uint32_t seg = load(m, "Song.sgt");
    // Off: the stop is silence within a few milliseconds.
    set_reverb(m, false, 0, 0, 2000, 0.5f);
    uint32_t state = play(m, seg, 100);
    if (state)
        call_method(state, RELEASE, {});
    Audio dry;
    render(dry, 1.5);
    call_method(m.perf, PERF_STOP, {0, 0, 0, 0});
    render(dry, 1.0);
    const double dry_tail = rms_db(dry, 1.6, 1.8);
    // On, with a long tail at the mix of the dry signal.
    set_reverb(m, true, 0, 0, 2000, 0.5f);
    state = play(m, seg, 100);
    if (state)
        call_method(state, RELEASE, {});
    Audio wet;
    render(wet, 1.5);
    call_method(m.perf, PERF_STOP, {0, 0, 0, 0});
    render(wet, 2.0);
    const double during = rms_db(wet, 0.5, 1.4), dry_during = rms_db(dry, 0.5, 1.4);
    const double tail = rms_db(wet, 1.6, 1.8), later = rms_db(wet, 3.0, 3.3);
    printf("  (dry %.1f dB, with reverb %.1f dB; after the stop %.1f dB dry, %.1f dB wet, "
           "%.1f dB 1.5 s on)\n",
           dry_during, during, dry_tail, tail, later);
    CHECK(dry_tail < -80.0);
    CHECK(tail > -45.0);
    CHECK(later < tail - 10.0); // and it decays
    CHECK(during > dry_during); // the reverb adds to the dry signal
    CHECK(during - dry_during < 4.0);
    // GET reads the parameters back.
    gm_zero(sc(0x250), 16);
    CHECK_EQ(ks_property(m.ks, PROP_WavesReverb, 1, sc(0x250), 16), S_OK_);
    float p[4];
    memcpy(p, g_mem + sc(0x250), 16);
    CHECK(p[0] == 0.0f && p[1] == 0.0f && p[2] == 2000.0f && p[3] == 0.5f);
    CHECK_EQ(ks_property(m.ks, PROP_Effects, 1, sc(0x240), 4), S_OK_);
    CHECK_EQ(rd32(sc(0x240)), 1u);
    call_method(seg, RELEASE, {});
    close_music(m);
}

// The reverb on its own: a steady noise comes out with its reverberation at
// the mix level, and the decay time is the one asked for.
static void test_reverb_model() {
    WavesReverb r;
    WavesReverbParams p;
    p.in_gain_db = 0.0f;
    p.mix_db = 0.0f;
    p.time_ms = 1500.0f;
    p.hf_ratio = 0.999f;
    const double rate = 48000.0;
    r.configure(rate, p);
    const uint32_t n = (uint32_t)(rate * 3.0);
    std::vector<float> l(n), rr(n), dl(n), dr(n);
    uint32_t seed = 1;
    for (uint32_t i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float v =
            i < (uint32_t)(rate * 2.0) ? ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.5f : 0.0f;
        l[i] = rr[i] = dl[i] = dr[i] = v;
    }
    r.process(l.data(), rr.data(), n);
    auto power = [&](const std::vector<float> &x, const std::vector<float> &y, double t0, double t1,
                     bool difference) {
        double s = 0.0;
        const uint32_t i0 = (uint32_t)(t0 * rate), i1 = (uint32_t)(t1 * rate);
        for (uint32_t i = i0; i < i1; ++i) {
            const double v = difference ? (double)x[i] - dl[i] : (double)x[i];
            const double w = difference ? (double)y[i] - dr[i] : (double)y[i];
            s += 0.5 * (v * v + w * w);
        }
        return 10.0 * log10(s / (i1 - i0) + 1e-30);
    };
    const double dry = power(dl, dr, 1.0, 2.0, false);
    const double wet = power(l, rr, 1.0, 2.0, true);
    printf("  (steady noise: dry %.1f dB, reverberation %.1f dB)\n", dry, wet);
    CHECK(fabs(wet - dry) < 3.0);
    // 60 dB in 1.5 s is 40 dB a second: compare two 100 ms windows 0.5 s apart.
    const double a = power(l, rr, 2.1, 2.2, false), b = power(l, rr, 2.6, 2.7, false);
    printf("  (decay %.1f dB in 0.5 s; 20 asked for)\n", a - b);
    CHECK(a - b > 15.0 && a - b < 25.0);
}

// The General MIDI bank: a DLS collection or a SoundFont named by
// RECOMP_DMUSIC_GM, and no bank at all, which leaves those instruments silent
// but the segment playing.
static void test_gm_bank() {
    struct Case {
        const char *bank;
        bool sounds;
    } cases[] = {{"GmBank.dls", true}, {"GmBank.sf2", true}, {"Nowhere.dls", false}};
    for (const Case &k : cases) {
        os_setenv("RECOMP_DMUSIC_GM", (g_dir + "/" + k.bank).c_str());
        Music m = open_music();
        uint32_t seg = load(m, "GmSong.sgt");
        CHECK(seg != 0);
        uint32_t state = play(m, seg, 100);
        if (state)
            call_method(state, RELEASE, {});
        Audio a;
        render(a, 1.0);
        const int heard = pitch(a, 0.1, 0.9);
        printf("  (%s: %d Hz)\n", k.bank, heard);
        CHECK_EQ(heard, k.sounds ? 1102 : 0);
        uint32_t hr = 0;
        segment_state(m, &hr);
        CHECK_EQ(hr, S_OK_); // it plays, heard or not
        call_method(seg, RELEASE, {});
        close_music(m);
    }
    os_unsetenv("RECOMP_DMUSIC_GM");
    printf("  (with no override the bank is %s)\n", dmusic_gm_bank_path().c_str());
    CHECK(!dmusic_gm_bank_path().empty());
}

// A host whose rate is not the one the performance was made at: the music
// keeps its pitch.
static void test_resample() {
    g_rate = 44100.0;
    Music m = open_music();
    g_rate = 48000.0;
    uint32_t seg = load(m, "Song.sgt");
    uint32_t state = play(m, seg, 100);
    if (state)
        call_method(state, RELEASE, {});
    Audio a;
    render(a, 1.0, 48000.0);
    CHECK_EQ(pitch(a, 0.1, 0.9), 441);
    CHECK(tone(a, 0.1, 0.9, 441.0) > 10.0 * tone(a, 0.1, 0.9, 480.0));
    call_method(seg, RELEASE, {});
    close_music(m);
}

// No audio output: nothing renders, and the state follows the calls alone.
static void test_no_output() {
    g_rate = 0.0;
    Music m = open_music();
    uint32_t seg = load(m, "Song.sgt");
    uint32_t state = play(m, seg, 0);
    uint32_t hr = 0;
    uint32_t current = segment_state(m, &hr);
    CHECK_EQ(hr, S_OK_);
    if (current)
        call_method(current, RELEASE, {});
    CHECK_EQ(call_method(m.perf, PERF_STOP, {0, 0, 0, 0}), S_OK_);
    segment_state(m, &hr);
    CHECK_EQ(hr, DMUS_E_NOT_FOUND_);
    if (state)
        call_method(state, RELEASE, {});
    call_method(seg, RELEASE, {});
    close_music(m);
    g_rate = 48000.0;
}

// Notifications are accepted and never arrive; messages are allocated and freed.
static void test_notifications() {
    Music m = open_music();
    wr32(sc(0x120), 0xdeadbeef);
    CHECK_EQ(call_method(m.perf, PERF_GETNOTIFICATIONPMSG, {sc(0x120)}), S_FALSE_);
    CHECK_EQ(rd32(sc(0x120)), 0u);
    CHECK_EQ(call_method(m.perf, PERF_ALLOCPMSG, {0x40, sc(0x124)}), S_OK_);
    const uint32_t msg = rd32(sc(0x124));
    CHECK(msg != 0);
    CHECK_EQ(rd32(msg), 0x40u);
    CHECK_EQ(call_method(m.perf, PERF_FREEPMSG, {msg}), S_OK_);
    CHECK(call_method(m.perf, PERF_ALLOCPMSG, {4, sc(0x124)}) != S_OK_);
    close_music(m);
}

// After CloseDown the performance renders nothing and refuses to play; every
// object let go is gone.
static void test_closedown() {
    const uint32_t live = com_live_count();
    const size_t engines = dmusic_engine_count();
    Music m = open_music();
    CHECK_EQ(dmusic_engine_count(), engines + 1);
    uint32_t seg = load(m, "Song.sgt");
    const uint32_t perf = m.perf;
    call_method(perf, ADDREF, {});
    close_music(m);
    CHECK_EQ(dmusic_engine_count(), engines);
    CHECK_EQ(call_method(perf, PERF_PLAYSEGMENT, {seg, 0, 0, 0, 0}), DMUS_E_NOT_INIT_);
    CHECK_EQ(call_method(perf, RELEASE, {}), 0u);
    CHECK_EQ(call_method(seg, RELEASE, {}), 0u);
    CHECK_EQ(com_live_count(), live);
}

// The audio thread renders while a guest thread plays, transitions, changes
// the groove and the volume and stops, as a game's music thread does.
static void test_threads() {
    Music m = open_music();
    uint32_t seg = load(m, "Song.sgt");
    std::atomic<bool> done{false};
    std::atomic<uint64_t> blocks{0};
    std::thread audio([&] {
        std::vector<float> l(256), r(256);
        while (!done.load()) {
            std::fill(l.begin(), l.end(), 0.0f);
            std::fill(r.begin(), r.end(), 0.0f);
            if (g_render)
                g_render(g_render_context, l.data(), r.data(), 256, 48000.0);
            ++blocks;
        }
    });
    for (int i = 0; i < 300; ++i) {
        switch (i % 6) {
        case 0: {
            uint32_t state = play(m, seg, 100);
            if (state)
                call_method(state, RELEASE, {});
            break;
        }
        case 1:
            set_groove(m, (int8_t)(i % 4));
            break;
        case 2:
            call_method(m.composer, COMPOSER_AUTOTRANSITION, {m.perf, seg, 0, 0x20, 0, 0, 0, 0});
            break;
        case 3:
            set_volume_mb(m, -(i % 3) * 300);
            break;
        case 4: {
            uint32_t current = segment_state(m);
            if (current)
                call_method(current, RELEASE, {});
            break;
        }
        case 5:
            call_method(m.perf, PERF_STOP, {0, 0, 0, 0});
            break;
        }
        if (i % 50 == 0)
            os_sleep_us(1000);
    }
    done.store(true);
    audio.join();
    printf("  (%llu blocks rendered meanwhile)\n", (unsigned long long)blocks.load());
    CHECK(blocks.load() > 0);
    call_method(seg, RELEASE, {});
    close_music(m);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    mem_init();
    imports_init();
    dmusic_enable_for_test(0);
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    cpu_reset();
    g_scratch = heap_alloc(0x8000, true, 16);
    if (!g_scratch) {
        fprintf(stderr, "cannot allocate scratch\n");
        return 1;
    }
    write_fixtures();

    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"registration", test_registration},
        {"vtable slots", test_slots},
        {"loader", test_loader},
        {"play", test_play},
        {"stop and unload", test_stop_and_unload},
        {"repeats", test_repeats},
        {"tempo", test_tempo},
        {"groove and transitions", test_groove},
        {"volume and fades", test_volume},
        {"reverb", test_reverb},
        {"reverb model", test_reverb_model},
        {"General MIDI bank", test_gm_bank},
        {"resampling", test_resample},
        {"no audio output", test_no_output},
        {"notifications", test_notifications},
        {"close down", test_closedown},
        {"render thread", test_threads},
    };
    for (const auto &t : tests) {
        int before = g_failures;
        cpu_reset();
        t.fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    for (const DmFixture &f : kDmFixtures)
        os_unlink((g_dir + "/" + f.name).c_str());
    os_rmdir(g_dir.c_str());
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        fprintf(stderr, "dmusic_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("dmusic_tests: all passed\n");
    return 0;
}
