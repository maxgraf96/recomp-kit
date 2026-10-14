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

#include "dmusic_client.h"

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

static std::string g_dir; // the fixtures on disk

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

// A game's volume: a float set as 1000 ln v hundredths of a dB, and the
// floor at -20000 below 0.01.
static void game_volume(const Music &m, double v) {
    set_volume_mb(m, v <= 0.01 ? -20000 : (int32_t)lround(1000.0 * log(v)));
}

// A fade in steps of 1/30 every 100 ms, rendering between them, as a game's
// music thread does it. Returns each step's level, clear of the breath that
// ends each bar.
static std::vector<double> fade(const Music &m, Audio &a, double from, double to) {
    std::vector<double> levels;
    const double step = to > from ? 1.0 / 30.0 : -1.0 / 30.0;
    for (double v = from;; v += step) {
        if ((step > 0 && v > to) || (step < 0 && v < to))
            v = to;
        game_volume(m, v);
        const double t = (double)a.frames() / a.rate;
        render(a, 0.1);
        levels.push_back(rms_db(a, t + 0.01, t + 0.07));
        if (v == to)
            break;
    }
    return levels;
}

// The sequence a game's music thread runs to change tracks: load the next
// score, fade out, stop, unload the old score (retrying while it still
// plays), set the new one's repeats, intensity and reverb, play it and fade
// in. The old track is 441 Hz and the new one 1102.5 Hz.
static void test_track_change() {
    os_setenv("RECOMP_DMUSIC_GM", (g_dir + "/GmBank.dls").c_str());
    Music m = open_music();
    set_reverb(m, true, 0.0f, -9.0f, 2100.0f, 0.25f);
    uint32_t old_seg = load(m, "Song.sgt");
    CHECK_EQ(call_method(old_seg, SEG_SETPARAM, {gp(GUID_Download), 0xffffffffu, 0, 0, m.perf}),
             S_OK_);
    game_volume(m, 0.0);
    CHECK_EQ(call_method(old_seg, SEG_SETREPEATS, {100000}), S_OK_);
    set_groove(m, 1);
    CHECK_EQ(call_method(m.perf, PERF_PLAYSEGMENT, {old_seg, 0x4000, 0, 0, 0}), S_OK_);
    Audio a;
    fade(m, a, 0.0, 0.62);
    render(a, 1.5);
    const double t_steady = (double)a.frames() / a.rate;
    const double steady = rms_db(a, t_steady - 1.0, t_steady - 0.1);
    CHECK_EQ(pitch(a, t_steady - 1.0, t_steady - 0.1), 660); // groove 1 + 1

    // The change.
    uint32_t next = load(m, "GmSong.sgt");
    CHECK(next != 0);
    CHECK_EQ(call_method(next, SEG_SETPARAM, {gp(GUID_Download), 0xffffffffu, 0, 0, m.perf}),
             S_OK_);
    const std::vector<double> out = fade(m, a, 0.62, 0.0);
    bool falling = true;
    for (size_t i = 1; i < out.size(); ++i)
        falling = falling && out[i] <= out[i - 1] + 1.0; // a bar's breath may fall in a window
    CHECK(falling);
    if (!falling)
        for (size_t i = 0; i < out.size(); ++i)
            fprintf(stderr, "  fade out step %zu: %.2f dB\n", i, out[i]);
    CHECK(out.size() == 20 && out.back() < out.front() - 25.0); // the reverb's tail is what is left
    CHECK_EQ(call_method(m.perf, PERF_STOP, {0, 0, 0, 0}), S_OK_);
    int tries = 0;
    for (; tries < 100; ++tries) {
        uint32_t hr = 0;
        uint32_t state = segment_state(m, &hr);
        if (!state)
            break;
        wr32(sc(0xe8), 0);
        call_method(state, STATE_GETSEGMENT, {sc(0xe8)});
        const bool still = rd32(sc(0xe8)) == old_seg;
        call_method(state, RELEASE, {});
        if (!still)
            break;
    }
    CHECK_EQ(tries, 0);
    CHECK_EQ(call_method(old_seg, SEG_SETPARAM, {gp(GUID_Unload), 0xffffffffu, 0, 0, 0x1234}),
             S_OK_);
    call_method(old_seg, RELEASE, {});
    CHECK_EQ(call_method(next, SEG_SETREPEATS, {100000}), S_OK_);
    set_groove(m, 0); // groove 1: A4, which the stand-in GM instrument sounds at 1102.5 Hz
    set_reverb(m, true, 0.0f, -8.0f, 1600.0f, 0.15f);
    CHECK_EQ(call_method(m.perf, PERF_PLAYSEGMENT, {next, 0x4000, 0, 0, 0}), S_OK_);
    game_volume(m, 0.0);
    fade(m, a, 0.0, 0.62);
    render(a, 1.5);
    const double t_end = (double)a.frames() / a.rate;
    CHECK_EQ(pitch(a, t_end - 1.0, t_end - 0.1), 1102);
    printf("  (old track %.1f dB, new track %.1f dB at the same volume)\n", steady,
           rms_db(a, t_end - 1.0, t_end - 0.1));
    uint32_t state = segment_state(m);
    CHECK(state != 0);
    if (state) {
        wr32(sc(0xe8), 0);
        call_method(state, STATE_GETSEGMENT, {sc(0xe8)});
        CHECK_EQ(rd32(sc(0xe8)), next);
        call_method(next, RELEASE, {});
        call_method(state, RELEASE, {});
    }
    call_method(next, RELEASE, {});
    close_music(m);
    os_unsetenv("RECOMP_DMUSIC_GM");
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

// A performance holds the DirectSound it is given, as DirectMusic does, and
// given none, the one the audio libraries share; CloseDown lets it go.
static void test_directsound() {
    ComObj *shared = com_this(dsound_shared_device());
    CHECK(shared != nullptr);
    if (!shared)
        return;
    const int32_t before = shared->refs;
    Music m = open_music(); // Init(&dm, NULL, NULL)
    CHECK_EQ(shared->refs, before + 1);
    close_music(m);
    CHECK_EQ(shared->refs, before);
    // Handed one explicitly, it holds that one.
    const uint32_t perf = cocreate(CLSID_Performance, IID_Performance);
    wr32(sc(0x20), 0);
    CHECK_EQ(call_method(perf, PERF_INIT, {sc(0x20), dsound_shared_device(), 0}), S_OK_);
    CHECK_EQ(shared->refs, before + 1);
    CHECK_EQ(call_method(perf, PERF_CLOSEDOWN, {}), S_OK_);
    CHECK_EQ(shared->refs, before);
    call_method(rd32(sc(0x20)), RELEASE, {});
    CHECK_EQ(call_method(perf, RELEASE, {}), 0u);
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
        {"a game's track change", test_track_change},
        {"reverb", test_reverb},
        {"reverb model", test_reverb_model},
        {"General MIDI bank", test_gm_bank},
        {"resampling", test_resample},
        {"no audio output", test_no_output},
        {"notifications", test_notifications},
        {"close down", test_closedown},
        {"DirectSound", test_directsound},
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
