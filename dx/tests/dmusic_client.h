// dmusic_client.h - a DirectX 6/7 game's DirectMusic, driven through the guest ABI,
// and the synthesizer's output captured offline. Shared by dmusic_tests and
// dmusic_render; each binary includes it once, so its definitions are
// file-static, and it defines the host's host_synth_* strongly.
#pragma once
#include "../com.h"
#include "../dmusic.h"
#include "../host_api.h"
#include "../../runtime/memory.h"
#include "../../platform/os.h"
#include "guest_abi.h"

#include <algorithm>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>
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

// The search directory a Music is opened with, as the guest names it.
static std::string g_guest_dir;

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
