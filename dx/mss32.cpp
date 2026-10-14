// mss32.cpp - Miles Sound System 6 (mss32.dll) as a game imports it:
// stdcall exports whose decorated names carry their arity (_AIL_name@bytes).
// Samples play PCM WAVE images and streams decode MP3 through shared host
// audio channels. The 3D provider API reports "no providers" so a game can
// use its plain 2D path. Every entry preserves the guest's stdcall stack.
//
// Where a rule below says "Miles does", it is Miles 6.1's own behaviour, read
// from the library rather than from its documentation: the preference
// defaults AIL_startup installs, the balance pan law, the loop count that
// counts down, a start that always begins at the top, and positions of a
// compressed sample counted in its compressed bytes.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "riff.h"
#include "mp3_source.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../platform/os.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>
#include <iterator>
#include <map>
#include <memory>

namespace {

const uint32_t SMP_FREE = 1, SMP_DONE = 2, SMP_PLAYING = 4;

// Miles' preference table as AIL_startup leaves it. A mixer reads the
// DirectSound fragment settings back (34 DIG_DS_FRAGMENT_SIZE in ms, 35
// DIG_DS_FRAGMENT_CNT, 42 DIG_DS_MIX_FRAGMENT_CNT) and divides by the first,
// so a table of zeroes is a divide error in the guest, not a quiet default.
// The others: 0 resampling tolerance, 1 DIG_MIXER_CHANNELS, 2
// DIG_DEFAULT_VOLUME (a new sample's volume), 3-9 MIDI, 14 the maximum chain
// element, 15 DIG_USE_WAVEOUT, 17 the 3D output rate, 31 the resampling
// filter, 41 the input latency, 44 mutex protection, 45 the minimum chain
// element time.
constexpr std::array<int32_t, 64> kPreferenceDefaults = {
    131,   64,    127, 120,  8,   127, 1, 0,     2, 0,   1536, 49152, 5, 16,  8192, 0,
    32768, 44100, 0,   4096, 120, 24,  0, 1,     0, 1,   0,    1,     1, 0,   0,    1,
    2048,  0,     8,   96,   0,   0,   0, 32768, 0, 250, 8,    1,     1, 100, 0,    0,
};
// Before anything runs the table already holds the defaults, so a mixer that
// reads a preference ahead of AIL_startup still never divides by zero.
std::array<int32_t, 64> g_preferences = kPreferenceDefaults;

void preferences_default() {
    g_preferences = kPreferenceDefaults;
}

int32_t miles_volume_mb(int32_t v) {
    v = std::clamp(v, 0, 127);
    return v == 0 ? -10000
                  : std::clamp((int32_t)std::lround(2000.0 * std::log10(v / 127.0)), -10000, 0);
}

// Miles pans by balance, not by power: a centred sound plays at full level on
// both sides, and moving it right fades the left side linearly to nothing at
// 127 while the right side stays where it was. Each side's scale is a table
// entry out of 128 - twice the distance from the far end, saturating at 63 -
// so pans 63 and 64 are both centre. DirectSound's pan attenuates exactly one
// channel, which is the same shape, so the scale converts to hundredths of a
// decibel on the far side.
int32_t miles_pan_scale(int32_t i) {
    return i < 63 ? 2 * i : 128;
}
int32_t miles_pan_mb(int32_t p) {
    p = std::clamp(p, 0, 127);
    auto attenuation = [](int32_t scale) {
        return scale <= 0
                   ? 10000
                   : std::min(10000, (int32_t)std::lround(-2000.0 * std::log10(scale / 128.0)));
    };
    if (int32_t right = miles_pan_scale(p); right < 128)
        return -attenuation(right);
    if (int32_t left = miles_pan_scale(127 - p); left < 128)
        return attenuation(left);
    return 0;
}

struct Driver {
    uint32_t rate = 22050, bits = 16, channels = 2;
    int master = 127;
    uint32_t hwnd = 0;
};
std::map<uint32_t, Driver> g_drivers;
uint32_t g_next_driver = 0x10000;

uint32_t driver_open(uint32_t rate, uint32_t bits, uint32_t channels) {
    uint32_t id = g_next_driver++;
    g_drivers[id] = Driver{rate, bits, channels, 127, 0};
    LOGV("mss32: opened digital driver %08x (%u Hz, %u-bit, %u channel(s))", id, rate, bits,
         channels);
    return id;
}

// AIL_last_error and AIL_set_redist_directory answer with a pointer to a
// buffer Miles owns. Those buffers live in guest memory, allocated on first
// use and forgotten with the arena.
uint32_t g_last_error = 0, g_redist = 0;
constexpr uint32_t kErrorBytes = 256, kRedistBytes = 260;

uint32_t guest_buffer(uint32_t &slot, uint32_t bytes) {
    if (!slot)
        slot = heap_alloc(bytes, true);
    return slot;
}

void set_error(const char *text) {
    if (uint32_t buffer = guest_buffer(g_last_error, kErrorBytes))
        gm_put_str(buffer, text, kErrorBytes);
    LOGV("mss32: %s", text);
}

// A compressed sample decodes once per distinct image. A mixer plugin hands
// the same image to set_named_sample_file on every load and on every play,
// and a three-minute MP3 takes a perceptible moment to decode, so the PCM is
// kept by content - not by guest address, which a freed image's successor
// may reuse - and shared by every sample playing it. Entries no sample holds
// are dropped oldest first beyond the budget.
struct Decoded {
    std::vector<int16_t> pcm;
    uint32_t rate = 0, channels = 0;
};
struct DecodedEntry {
    uint64_t hash = 0, used = 0;
    uint32_t bytes = 0;
    std::shared_ptr<const Decoded> data;
};
std::vector<DecodedEntry> g_decoded;
uint64_t g_decode_clock = 0, g_decodes = 0, g_decode_hits = 0;
constexpr size_t kDecodedBudget = 256u << 20;
constexpr size_t kDecodedLimit = 64u << 20; // samples of one image, as before

uint64_t image_hash(const uint8_t *p, uint32_t n) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (; i < n; ++i)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void decoded_trim() {
    size_t total = 0;
    for (const auto &e : g_decoded)
        total += e.data->pcm.size() * 2;
    while (total > kDecodedBudget) {
        auto victim = g_decoded.end();
        for (auto it = g_decoded.begin(); it != g_decoded.end(); ++it)
            if (it->data.use_count() == 1 && (victim == g_decoded.end() || it->used < victim->used))
                victim = it;
        if (victim == g_decoded.end())
            return; // everything left is playing
        total -= victim->data->pcm.size() * 2;
        g_decoded.erase(victim);
    }
}

std::shared_ptr<const Decoded> decode_mp3(uint32_t image, uint32_t bytes) {
    const uint8_t *p = g_mem + image;
    uint64_t hash = image_hash(p, bytes);
    for (auto &e : g_decoded)
        if (e.hash == hash && e.bytes == bytes) {
            e.used = ++g_decode_clock;
            ++g_decode_hits;
            return e.data;
        }
    Mp3Source source;
    if (!source.open(std::vector<uint8_t>(p, p + bytes)))
        return nullptr;
    auto out = std::make_shared<Decoded>();
    out->rate = source.rate();
    out->channels = source.channels();
    std::vector<int16_t> frame;
    while (source.decode_next(frame)) {
        if ((out->pcm.size() + frame.size()) * 2 > kDecodedLimit)
            return nullptr;
        out->pcm.insert(out->pcm.end(), frame.begin(), frame.end());
    }
    if (out->pcm.empty())
        return nullptr;
    ++g_decodes;
    g_decoded.push_back({hash, ++g_decode_clock, bytes, out});
    decoded_trim();
    return out;
}

struct Sample {
    uint32_t handle = 0, driver = 0;
    bool alive = false;
    int32_t channel = -1;
    RiffWave wave{}; // the format; .pcm is a guest address unless decoded
    // A compressed image, decoded. Miles counts such a sample's positions in
    // the image's own bytes, so its length is kept beside the PCM.
    std::shared_ptr<const Decoded> decoded;
    uint32_t encoded_bytes = 0;
    int32_t volume = 127, pan = 64;
    // Miles' loop count: 0 plays for ever, n plays n more times counting the
    // pass under way. Miles counts it down at each loop point.
    uint32_t loops = 1;
    uint32_t rate = 11025; // the playback rate, Hz
    uint32_t position = 0; // PCM bytes: where a sample that is not playing stands
    bool playing = false;
    bool retuned = false; // the host channel's rate was changed under a playing sound
    uint32_t eos = 0;     // guest end-of-sample callback, stdcall (HSAMPLE)
    bool eos_due = false;
    uint32_t user[8] = {};
};
std::vector<Sample> g_samples(64);

Sample *sample_for(uint32_t handle) {
    if (!handle || handle > g_samples.size())
        return nullptr;
    Sample &s = g_samples[handle - 1];
    return s.alive ? &s : nullptr;
}

const Driver *driver_of(const Sample &s) {
    auto it = g_drivers.find(s.driver);
    return it == g_drivers.end() ? nullptr : &it->second;
}

uint32_t frame_bytes(const Sample &s) {
    return std::max<uint32_t>(1, (uint32_t)s.wave.channels * (s.wave.bits / 8));
}

// Bytes of sample data per "frame" in Miles' own units: a PCM frame, or one
// byte of a compressed image.
uint32_t granularity(const Sample &s) {
    return s.decoded ? 1 : frame_bytes(s);
}

// Positions cross between the PCM the host plays and the units Miles reports:
// a compressed image's bytes, in proportion, for a decoded sample.
uint32_t to_miles(const Sample &s, uint32_t pcm) {
    if (!s.decoded || !s.wave.pcm_bytes)
        return pcm;
    return (uint32_t)((uint64_t)pcm * s.encoded_bytes / s.wave.pcm_bytes);
}
uint32_t from_miles(const Sample &s, uint32_t pos) {
    uint64_t pcm = pos;
    if (s.decoded && s.encoded_bytes)
        pcm = (uint64_t)pos * s.wave.pcm_bytes / s.encoded_bytes;
    pcm = std::min<uint64_t>(pcm, s.wave.pcm_bytes);
    return (uint32_t)(pcm - pcm % frame_bytes(s));
}

// Miles scales a sample by its driver's master volume in integer steps of
// 1/127, then pans what is left. A mono driver has no sides to pan between.
int32_t sample_volume(const Sample &s) {
    const Driver *d = driver_of(s);
    int32_t master = d ? d->master : 127;
    return miles_volume_mb(master * std::clamp(s.volume, 0, 127) / 127);
}
int32_t sample_pan(const Sample &s) {
    const Driver *d = driver_of(s);
    return d && d->channels == 1 ? 0 : miles_pan_mb(s.pan);
}

// The sample borrows its WAVE image (or shares its decoded PCM). The host
// copies PCM on submission; Miles keeps the shared channel reserved until
// init or handle release.
void sample_stop(Sample &s) {
    if (s.channel >= 0)
        host_audio_stop(s.channel);
    s.playing = false;
}

// Each play states its own rate. A rate changed under a playing sound
// retuned the channel itself, and a retuned channel keeps that rate over the
// rate a later play names, so it is put back first.
void sample_untune(Sample &s) {
    if (!s.retuned || s.channel < 0)
        return;
    host_audio_stop(s.channel);
    host_audio_set_frequency(s.channel, 0);
    s.retuned = false;
}

void sample_init(Sample &s) {
    sample_stop(s);
    sample_untune(s);
    dx_free_audio_channel(s.channel);
    uint32_t handle = s.handle, driver = s.driver, user[8];
    std::copy(std::begin(s.user), std::end(s.user), user);
    s = Sample{};
    s.handle = handle;
    s.driver = driver;
    s.alive = true;
    std::copy(std::begin(user), std::end(user), s.user);
    // AIL_init_sample's defaults: DIG_DEFAULT_VOLUME, centred on a stereo
    // driver, one pass, 11025 Hz until a file states its own rate.
    s.volume = std::clamp(g_preferences[2], 0, 127);
    const Driver *d = driver_of(s);
    s.pan = d && d->channels == 1 ? 0 : 64;
}

void sample_play(Sample &s, uint32_t from) {
    sample_untune(s);
    HostAudioPlay p{};
    p.channel = s.channel;
    p.pcm = s.decoded ? (const void *)s.decoded->pcm.data() : (const void *)(g_mem + s.wave.pcm);
    p.bytes = s.wave.pcm_bytes;
    p.sample_rate = (int32_t)(s.rate ? s.rate : s.wave.rate);
    p.channels = s.wave.channels;
    p.bits = s.wave.bits;
    p.loop = s.loops == 0 ? 1 : 0;
    p.volume = sample_volume(s);
    p.pan = sample_pan(s);
    p.start_offset = from;
    host_audio_play(&p);
    s.playing = true;
    s.position = from;
}

// Where a sample has reached, in PCM bytes from the start of its data. The
// host wraps a loop over the whole buffer, as Miles returns to the loop start.
uint32_t sample_cursor(const Sample &s) {
    if (!s.playing || s.channel < 0)
        return s.position;
    uint32_t pos = std::min(host_audio_position(s.channel), s.wave.pcm_bytes);
    return pos - pos % frame_bytes(s);
}

// A pass reached the end of the data. Miles counts the loop field down and
// goes round again, or finishes: status DONE, the position at the end, and
// the end-of-sample callback due. The host loop flag is boolean, so finite
// repeats restart here on the runtime's frame seam (also observed by status).
void sample_pass_ended(Sample &s) {
    if (s.loops > 1) {
        --s.loops;
        sample_play(s, 0);
        return;
    }
    s.playing = false;
    s.position = s.wave.pcm_bytes;
    s.eos_due = s.eos != 0;
}

void sample_update(Sample &s) {
    if (s.playing && !host_audio_is_playing(s.channel))
        sample_pass_ended(s);
}

// The callback runs on a guest thread that holds the scheduler baton: the
// frame pump, AIL_serve or AIL_end_sample. Never from a status query, which a
// game may make with its own locks held. Miles runs it from its mixer thread.
void sample_fire_eos(X86 *c, Sample &s) {
    if (!s.eos_due || !c)
        return;
    s.eos_due = false;
    if (s.eos)
        guest_call(c, s.eos, s.handle);
}

// The loop flag of a playing sound is fixed when it is scheduled. A change
// of "for ever" while it plays reschedules it from where it is, so a loop
// that is ended finishes its pass and a one-shot that becomes a loop goes round.
void sample_relooped(Sample &s, bool was_forever) {
    if (s.playing && (s.loops == 0) != was_forever && s.channel >= 0) {
        uint32_t at = sample_cursor(s);
        if (at >= s.wave.pcm_bytes)
            at = 0;
        sample_play(s, at);
    }
}

// Use CreateFileA's read resolver, including overlays, drive mapping and
// case folding. Both NULL (ordinary Miles allocation) and FILE_READ_WITH_SIZE
// request a guest allocation here; the returned image begins with file bytes.
void ail_file_read(X86 *c) {
    set_eax(c, 0);
    std::string path = win32_host_path_op(gm_str(arg(c, 0)), WIN32_FILE_READ);
    if (path.empty()) {
        set_error("Unable to open file.");
        return;
    }
    int fd = os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0) {
        set_error("Unable to open file.");
        return;
    }
    OsStat st{};
    if (os_fd_stat(fd, &st) != 0 || !st.is_regular || !st.size || st.size > UINT32_MAX) {
        os_fd_close(fd);
        set_error("Unable to open file.");
        return;
    }
    uint32_t bytes = (uint32_t)st.size, dest = arg(c, 1);
    bool allocate = dest == 0 || dest == 0xffffffffu;
    if (allocate)
        dest = heap_alloc(bytes);
    if (!dest || !gm_valid(dest, bytes)) {
        if (allocate && dest)
            heap_free(dest);
        os_fd_close(fd);
        set_error("Out of memory.");
        return;
    }
    uint32_t got = 0;
    while (got < bytes) {
        int64_t n = os_fd_read(fd, g_mem + dest + got, bytes - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    os_fd_close(fd);
    if (got != bytes) {
        if (allocate)
            heap_free(dest);
        set_error("Unable to read file.");
        return;
    }
    set_eax(c, dest);
}

// AIL_file_size(filename): the size in bytes, or -1 with "Unable to open file."
void ail_file_size(X86 *c) {
    std::string path = win32_host_path_op(gm_str(arg(c, 0)), WIN32_FILE_READ);
    OsStat st{};
    if (path.empty() || os_stat(path.c_str(), &st) != 0 || !st.is_regular || st.size > INT32_MAX) {
        set_error("Unable to open file.");
        set_eax(c, 0xffffffffu);
        return;
    }
    set_eax(c, (uint32_t)st.size);
}

// AIL_mem_alloc_lock(bytes): Miles allocates GMEM_ZEROINIT, so the block is
// zeroed; AIL_mem_free_lock gives it back.
void ail_mem_alloc_lock(X86 *c) {
    uint32_t block = heap_alloc(std::max(arg(c, 0), 1u), true);
    if (!block)
        set_error("Out of memory.");
    set_eax(c, block);
}

void ail_mem_free_lock(X86 *c) {
    heap_free(arg(c, 0));
    set_eax(c, 0);
}

void ail_allocate_sample_handle(X86 *c) {
    set_eax(c, 0);
    for (uint32_t i = 0; i < g_samples.size(); ++i) {
        Sample &s = g_samples[i];
        if (s.alive)
            continue;
        s = Sample{};
        s.handle = i + 1;
        s.driver = arg(c, 0);
        s.alive = true;
        sample_init(s);
        set_eax(c, s.handle);
        return;
    }
    set_error("Out of sample handles.");
}

void ail_release_sample_handle(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        sample_init(*s);
        *s = Sample{};
    }
    set_eax(c, 0);
}

void ail_init_sample(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0)))
        sample_init(*s);
    set_eax(c, 0);
}

// Forget the data a sample had, ahead of a new image.
void sample_unload(Sample &s) {
    sample_stop(s);
    s.wave = RiffWave{};
    s.decoded.reset();
    s.encoded_bytes = 0;
    s.position = 0;
}

void ail_set_sample_file(X86 *c) {
    set_eax(c, 0);
    Sample *s = sample_for(arg(c, 0));
    if (!s)
        return;
    sample_unload(*s);
    uint32_t image = arg(c, 1);
    if (!image || !gm_valid(image, 12))
        return;
    // Miles supplies no length. Read it from RIFF, bounded by an allocation
    // when the image is at its start, and always by the guest arena.
    uint64_t bytes = (uint64_t)rd32(image + 4) + 8;
    uint32_t allocated = heap_size(image);
    if (bytes > UINT32_MAX || (allocated != UINT32_MAX && bytes > allocated))
        return;
    if (riff_parse_wave(image, (uint32_t)bytes, &s->wave)) {
        s->rate = s->wave.rate;
        set_eax(c, 1);
    } else {
        s->wave = RiffWave{};
        set_error("Unsupported wave file format.");
    }
}

// AIL_set_named_sample_file(S, name, image, bytes, block). Miles picks the
// decoder by the name (an MP3 decoder provider claims "*.mp3"; ".wav" goes to
// its own RIFF reader); this reads the image itself, which agrees on every
// image Miles plays and also takes packed MP3 effects under other names.
// RIFF PCM plays from guest memory; MP3 decodes once per image (above).
void ail_set_named_sample_file(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, 0);
    if (!s)
        return;
    sample_unload(*s);
    uint32_t data = arg(c, 2), bytes = arg(c, 3);
    if (!data || !bytes || !gm_valid(data, bytes))
        return;
    if (riff_parse_wave(data, bytes, &s->wave)) {
        s->rate = s->wave.rate;
        set_eax(c, 1);
        return;
    }
    s->wave = RiffWave{};
    s->decoded = decode_mp3(data, bytes);
    if (!s->decoded) {
        set_error("Unknown file type.");
        return;
    }
    s->encoded_bytes = bytes;
    s->wave.pcm_bytes = uint32_t(s->decoded->pcm.size() * 2);
    s->wave.rate = s->decoded->rate;
    s->wave.channels = (uint16_t)s->decoded->channels;
    s->wave.bits = 16;
    s->rate = s->wave.rate;
    set_eax(c, 1);
}

// A successful waveOutOpen must populate HDIGDRIVER, not just return zero.
void ail_wave_open(X86 *c) {
    uint32_t out = arg(c, 0), fmt = arg(c, 3);
    set_eax(c, 1);
    if (!out || !gm_valid(out, 4))
        return;
    wr32(out, 0);
    if (!fmt || !gm_valid(fmt, 16) || rd16(fmt) != 1 || !rd32(fmt + 4) ||
        (rd16(fmt + 2) != 1 && rd16(fmt + 2) != 2) || (rd16(fmt + 14) != 8 && rd16(fmt + 14) != 16))
        return;
    uint32_t id = driver_open(rd32(fmt + 4), rd16(fmt + 14), rd16(fmt + 2));
    wr32(out, id);
    if (arg(c, 1) && gm_valid(arg(c, 1), 4))
        wr32(arg(c, 1), id);
    set_eax(c, 0);
}

void driver_close(uint32_t id) {
    for (auto &s : g_samples)
        if (s.alive && s.driver == id) {
            sample_init(s);
            s = Sample{};
        }
    g_drivers.erase(id);
}

void ail_wave_close(X86 *c) {
    driver_close(arg(c, 0));
    set_eax(c, 0);
}

// AIL_open_digital_driver(rate, bits, channels, flags) -> HDIGDRIVER, or 0.
// Miles builds the PCM format the way it does for every output: any channel
// count but 1 is stereo, any width but 8 is 16-bit, rate 0 is 22050 Hz. Flag
// bit 0 keeps the caller's DIG_USE_WAVEOUT; without it DirectSound is tried
// first, which here is the same device.
void ail_open_digital_driver(X86 *c) {
    uint32_t rate = arg(c, 0) ? arg(c, 0) : 22050;
    uint32_t bits = arg(c, 1) != 8 ? 16 : 8, channels = arg(c, 2) != 1 ? 2 : 1;
    if (!(arg(c, 3) & 1))
        g_preferences[15] = 0;
    set_eax(c, driver_open(rate, bits, channels));
}

void ail_close_digital_driver(X86 *c) {
    driver_close(arg(c, 0));
    set_eax(c, 0);
}

void ail_configuration(X86 *c) {
    auto it = g_drivers.find(arg(c, 0));
    if (it != g_drivers.end()) {
        if (arg(c, 1) && gm_valid(arg(c, 1), 4))
            wr32(arg(c, 1), it->second.rate);
        if (arg(c, 2) && gm_valid(arg(c, 2), 4))
            wr32(arg(c, 2), (it->second.channels == 2 ? 2 : 0) | (it->second.bits == 16 ? 1 : 0));
        if (arg(c, 3) && gm_valid(arg(c, 3), 128))
            gm_put_str(arg(c, 3), "Native digital audio", 128);
    }
    set_eax(c, 0);
}
void ail_master_volume(X86 *c) {
    auto it = g_drivers.find(arg(c, 0));
    if (it != g_drivers.end())
        it->second.master = std::clamp((int32_t)arg(c, 1), 0, 127);
    for (auto &s : g_samples)
        if (s.alive && s.channel >= 0 && s.driver == arg(c, 0))
            host_audio_set_volume(s.channel, sample_volume(s));
    set_eax(c, 0);
}
void ail_preference(X86 *c) {
    uint32_t index = arg(c, 0);
    set_eax(c, index < 64 ? g_preferences[index] : 0);
    if (index < 64)
        g_preferences[index] = (int32_t)arg(c, 1);
}
void ail_get_preference(X86 *c) {
    set_eax(c, arg(c, 0) < 64 ? g_preferences[arg(c, 0)] : 0);
}

// AIL_startup installs the preference defaults (a preference set earlier is
// lost, as in Miles) and reports success.
void ail_startup(X86 *c) {
    preferences_default();
    set_eax(c, 1);
}

// AIL_shutdown closes every driver, and so every sample on it, and every stream.
void streams_close();
void ail_shutdown(X86 *c) {
    while (!g_drivers.empty())
        driver_close(g_drivers.begin()->first);
    for (auto &s : g_samples)
        if (s.alive) {
            sample_init(s);
            s = Sample{};
        }
    streams_close();
    set_eax(c, 0);
}

// AIL_get_DirectSound_info(S, LPDIRECTSOUND *, LPDIRECTSOUNDBUFFER *): the
// DirectSound object the driver plays through, which is the one the runtime
// shares with DirectMusic and Bink. A sample here never has a DirectSound
// buffer of its own, and Miles leaves that out-parameter untouched then; for
// S = 0 Miles reports its mixing buffer, and there is none to report.
void ail_get_directsound_info(X86 *c) {
    uint32_t ds_out = arg(c, 1), dsb_out = arg(c, 2);
    if (ds_out && gm_valid(ds_out, 4))
        wr32(ds_out, dsound_shared_device());
    if (!arg(c, 0) && dsb_out && gm_valid(dsb_out, 4)) {
        log_once("mss32.dsb", "mss32: AIL_get_DirectSound_info reports no mixing buffer");
        wr32(dsb_out, 0);
    }
    set_eax(c, 0);
}

// AIL_set_DirectSound_HWND(driver, hwnd): the window DirectSound's
// cooperative level names. Nothing here depends on it.
void ail_set_directsound_hwnd(X86 *c) {
    auto it = g_drivers.find(arg(c, 0));
    if (it != g_drivers.end())
        it->second.hwnd = arg(c, 1);
    set_eax(c, it != g_drivers.end() ? 1 : 0);
}

// AIL_set_redist_directory(dir): where Miles looks for its providers. It
// keeps its own copy, ending in a backslash unless empty or a bare drive, and
// returns that copy.
void ail_set_redist_directory(X86 *c) {
    uint32_t buffer = guest_buffer(g_redist, kRedistBytes);
    if (!buffer) {
        set_eax(c, 0);
        return;
    }
    std::string dir = arg(c, 0) ? gm_str(arg(c, 0), kRedistBytes - 2) : std::string();
    if (!dir.empty() && dir.back() != '\\' && dir.back() != ':')
        dir += '\\';
    gm_put_str(buffer, dir.c_str(), kRedistBytes);
    set_eax(c, buffer);
}

// AIL_last_error(): the message of the last failure, "" before any.
void ail_last_error(X86 *c) {
    set_eax(c, guest_buffer(g_last_error, kErrorBytes));
}

// The host mixer is not Miles' mixer: it runs on the audio device's own
// thread, so there is no Miles service timer to be late and no mixing cost to
// report against it.
void ail_digital_cpu_percent(X86 *c) {
    set_eax(c, 0);
}
void ail_get_timer_highest_delay(X86 *c) {
    set_eax(c, 0);
}

void ail_start_sample(X86 *c) {
    // Miles starts at the top: AIL_start_sample zeroes the position, so a
    // position set before it does not survive it.
    if (Sample *s = sample_for(arg(c, 0)); s && s->wave.pcm_bytes) {
        if (s->channel < 0)
            s->channel = dx_alloc_audio_channel();
        if (s->channel >= 0) {
            s->eos_due = false;
            sample_play(*s, 0);
        }
    }
    set_eax(c, 0);
}

// Ending a sample that is still playing is an end of sample to Miles, so the
// callback runs before this returns.
void ail_end_sample(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        sample_update(*s);
        if (s->playing) {
            s->position = sample_cursor(*s);
            sample_stop(*s);
            s->eos_due = s->eos != 0;
        }
        sample_fire_eos(c, *s);
    }
    set_eax(c, 0);
}

void ail_sample_status(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    if (s)
        sample_update(*s);
    set_eax(c, !s ? SMP_FREE : s->playing ? SMP_PLAYING : SMP_DONE);
}

void ail_set_sample_volume(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        s->volume = std::clamp((int32_t)arg(c, 1), 0, 127);
        if (s->channel >= 0)
            host_audio_set_volume(s->channel, sample_volume(*s));
    }
    set_eax(c, 0);
}

void ail_set_sample_pan(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        s->pan = std::clamp((int32_t)arg(c, 1), 0, 127);
        if (s->channel >= 0)
            host_audio_set_pan(s->channel, sample_pan(*s));
    }
    set_eax(c, 0);
}

void ail_set_sample_loop_count(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        bool was_forever = s->loops == 0;
        s->loops = arg(c, 1);
        sample_relooped(*s, was_forever);
    }
    set_eax(c, 0);
}

void ail_sample_loop_count(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, s ? s->loops : 0);
}

// AIL_sample_granularity: the bytes a position moves in - a PCM frame, or 1
// for a compressed image. A mixer divides a sample's length by it.
void ail_sample_granularity(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, s ? granularity(*s) : 0);
}

void ail_sample_playback_rate(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, s ? s->rate : 0);
}

// A rate set before the sample plays is what its next play states; one set
// while it plays retunes the channel from where it is, without a restart.
void ail_set_sample_playback_rate(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        s->rate = arg(c, 1);
        sample_update(*s);
        if (s->playing && s->rate && s->channel >= 0) {
            host_audio_set_frequency(s->channel, s->rate);
            s->retuned = true;
        }
    }
    set_eax(c, 0);
}

// AIL_sample_position: bytes into the sample's data - PCM bytes, or the
// compressed image's bytes for a compressed sample.
void ail_sample_position(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    if (s)
        sample_update(*s);
    set_eax(c, s ? to_miles(*s, sample_cursor(*s)) : 0);
}

// AIL_set_sample_position: Miles rounds to the nearest whole granule. A
// playing sample moves there at once; at or past the end, that is the end of
// a pass. A sample that is not playing keeps it until the next start, which
// begins at the top regardless.
void ail_set_sample_position(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0))) {
        uint32_t g = granularity(*s);
        uint64_t pos = ((uint64_t)arg(c, 1) + g / 2) / g * g;
        uint32_t pcm = from_miles(*s, (uint32_t)std::min<uint64_t>(pos, UINT32_MAX));
        sample_update(*s);
        if (!s->playing) {
            s->position = pcm;
        } else if (pcm < s->wave.pcm_bytes) {
            sample_play(*s, pcm);
        } else if (s->loops == 0) {
            sample_play(*s, 0);
        } else {
            sample_stop(*s);
            sample_pass_ended(*s);
        }
    }
    set_eax(c, 0);
}

// AIL_sample_ms_position(S, &total_ms, &current_ms). Miles converts at the
// data's byte rate: a PCM frame per sample at the current playback rate, or
// the compressed image's bit rate, which is spread over the decoded length.
void ail_sample_ms_position(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    uint32_t total_out = arg(c, 1), current_out = arg(c, 2);
    set_eax(c, 0);
    if (!s)
        return;
    sample_update(*s);
    double per_second;
    uint32_t length = s->wave.pcm_bytes, at = sample_cursor(*s);
    if (s->decoded) {
        double seconds =
            s->wave.rate ? (double)(s->wave.pcm_bytes / frame_bytes(*s)) / s->wave.rate : 0.0;
        per_second = seconds > 0 ? s->encoded_bytes / seconds : 0.0;
        length = s->encoded_bytes;
        at = to_miles(*s, at);
    } else {
        per_second = (double)frame_bytes(*s) * s->rate;
    }
    auto ms = [per_second](uint32_t bytes) {
        return per_second > 0 ? (uint32_t)std::nearbyint(bytes * 1000.0 / per_second) : 0u;
    };
    if (total_out && gm_valid(total_out, 4))
        wr32(total_out, ms(length));
    if (current_out && gm_valid(current_out, 4))
        wr32(current_out, ms(at));
}

// AIL_register_EOS_callback(S, callback) -> the previous callback.
void ail_register_eos_callback(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, s ? s->eos : 0);
    if (s)
        s->eos = arg(c, 1);
}

// Eight words of the application's own per sample, as Miles keeps them.
void ail_set_sample_user_data(X86 *c) {
    if (Sample *s = sample_for(arg(c, 0)); s && arg(c, 1) < 8)
        s->user[arg(c, 1)] = arg(c, 2);
    set_eax(c, 0);
}
void ail_sample_user_data(X86 *c) {
    Sample *s = sample_for(arg(c, 0));
    set_eax(c, s && arg(c, 1) < 8 ? s->user[arg(c, 1)] : 0);
}

// Stream handles own the encoded image, decoder and shared host channel.
// All state is serviced on the guest frame seam; the host copies each PCM
// submission. A refused chunk remains pending until a later frame accepts it.
struct Stream {
    bool alive = false, playing = false;
    int32_t channel = -1, volume = 127;
    uint32_t loops = 1, remaining = 1;
    uint64_t submitted = 0;
    Mp3Source source;
    std::vector<int16_t> pending;
    size_t pending_pos = 0;
};
std::vector<Stream> g_streams(64);

Stream *stream_for(uint32_t handle) {
    if (!handle || handle > g_streams.size())
        return nullptr;
    Stream &s = g_streams[handle - 1];
    return s.alive ? &s : nullptr;
}

// Read through the runtime resolver, sharing CreateFileA's overlays, drive
// mapping and case folding. Open does not play until AIL_start_stream.
void ail_open_stream(X86 *c) {
    set_eax(c, 0);
    uint32_t name = arg(c, 1);
    if (!name || !gm_valid(name, 1))
        return;
    std::string guest = gm_str(name);
    std::string path = win32_host_path_op(guest, WIN32_FILE_READ);
    int fd = path.empty() ? -1 : os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0) {
        LOGW("mss32: open_stream(%s): cannot open %s", guest.c_str(), path.c_str());
        return;
    }
    OsStat st{};
    if (os_fd_stat(fd, &st) != 0 || !st.is_regular || !st.size || st.size > UINT32_MAX) {
        os_fd_close(fd);
        return;
    }
    std::vector<uint8_t> bytes((size_t)st.size);
    size_t got = 0;
    while (got < bytes.size()) {
        int64_t n = os_fd_read(fd, bytes.data() + got, bytes.size() - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    os_fd_close(fd);
    if (got != bytes.size()) {
        LOGW("mss32: open_stream(%s): short read of %s", guest.c_str(), path.c_str());
        return;
    }
    for (uint32_t i = 0; i < g_streams.size(); ++i) {
        Stream &s = g_streams[i];
        if (s.alive)
            continue;
        s = Stream{};
        if (!s.source.open(bytes)) {
            LOGW("mss32: open_stream(%s): not an MPEG audio file: %s", guest.c_str(), path.c_str());
            return;
        }
        s.channel = dx_alloc_audio_channel();
        if (s.channel < 0) {
            s = Stream{};
            return;
        }
        s.alive = true;
        LOGV("mss32: open_stream(%s): %s, %u Hz, %u channel(s)", guest.c_str(), path.c_str(),
             s.source.rate(), s.source.channels());
        set_eax(c, i + 1);
        return;
    }
}

// Keep one second of appended PCM ahead. Decoder EOF is not playback EOF:
// status stays playing until the host consumes the first play and all queues.
void stream_update(Stream &s) {
    if (!s.playing)
        return;
    uint32_t ahead = s.source.rate() * s.source.channels() * 2;
    while (host_audio_queued_bytes(s.channel) < ahead) {
        if (s.pending.empty()) {
            if (!s.source.decode_next(s.pending)) {
                if (s.loops != 0 && s.remaining <= 1)
                    break;
                if (s.remaining > 1)
                    --s.remaining;
                s.source.seek_frames(0);
                if (!s.source.decode_next(s.pending))
                    break;
            }
            s.pending_pos = 0;
        }
        uint32_t bytes = (uint32_t)(s.pending.size() - s.pending_pos) * 2;
        int32_t taken = host_audio_queue(s.channel, s.pending.data() + s.pending_pos, bytes);
        if (taken <= 0)
            return;
        s.submitted += (uint32_t)taken;
        s.pending_pos += (uint32_t)taken / 2;
        if (s.pending_pos >= s.pending.size())
            s.pending.clear();
    }
    if (s.source.drained() && s.pending.empty() &&
        host_audio_played_bytes(s.channel) >= s.submitted)
        s.playing = false;
}

// Start with one decoded frame, then convert the channel so subsequent
// frames append without restarting playback. Starting again rewinds.
void ail_start_stream(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0))) {
        host_audio_stop(s->channel);
        s->playing = false;
        s->pending.clear();
        s->source.seek_frames(0);
        s->remaining = s->loops;
        if (s->source.decode_next(s->pending)) {
            HostAudioPlay p{};
            p.channel = s->channel;
            p.pcm = s->pending.data();
            p.bytes = (uint32_t)s->pending.size() * 2;
            p.sample_rate = (int32_t)s->source.rate();
            p.channels = (int32_t)s->source.channels();
            p.bits = 16;
            p.volume = miles_volume_mb(s->volume);
            host_audio_play(&p);
            s->submitted = p.bytes;
            s->pending.clear();
            s->playing = host_audio_stream(s->channel) >= 0;
            if (!s->playing)
                host_audio_stop(s->channel);
        }
    }
    set_eax(c, 0);
}

void ail_close_stream(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0))) {
        host_audio_stop(s->channel);
        dx_free_audio_channel(s->channel);
        *s = Stream{};
    }
    set_eax(c, 0);
}

void ail_stream_status(X86 *c) {
    Stream *s = stream_for(arg(c, 0));
    if (s)
        stream_update(*s);
    set_eax(c, s && s->playing ? SMP_PLAYING : SMP_DONE);
}

void ail_set_stream_volume(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0))) {
        s->volume = std::clamp((int32_t)arg(c, 1), 0, 127);
        host_audio_set_volume(s->channel, miles_volume_mb(s->volume));
    }
    set_eax(c, 0);
}

void ail_stream_volume(X86 *c) {
    Stream *s = stream_for(arg(c, 0));
    set_eax(c, s ? (uint32_t)s->volume : 0);
}

void ail_set_stream_loop_count(X86 *c) {
    if (Stream *s = stream_for(arg(c, 0)))
        s->remaining = s->loops = arg(c, 1);
    set_eax(c, 0);
}

void streams_close() {
    for (auto &s : g_streams)
        if (s.alive) {
            host_audio_stop(s.channel);
            dx_free_audio_channel(s.channel);
            s = Stream{};
        }
}

// Ends passes, restarts finite loops, runs the end-of-sample callbacks that
// fell due and keeps streams fed. From the frame pump and from AIL_serve,
// both on a guest thread holding the baton.
void service(X86 *c) {
    for (auto &s : g_samples)
        if (s.alive)
            sample_update(s);
    for (auto &s : g_samples)
        if (s.alive && s.eos_due)
            sample_fire_eos(c, s);
    for (auto &s : g_streams)
        if (s.alive)
            stream_update(s);
}

// AIL_serve: a game's own call to service the mixer between timer ticks.
void ail_serve(X86 *c) {
    service(c);
    set_eax(c, 0);
}

void ret0(X86 *c) {
    set_eax(c, 0);
}
void ret1(X86 *c) {
    set_eax(c, 1);
}
void ret_done(X86 *c) {
    set_eax(c, SMP_DONE);
}

#define AIL(name, bytes, fn) {"mss32.dll", "_AIL_" #name "@" #bytes, (bytes) / 4, fn}

const ImportShim g_mss32_shims[] = {
    AIL(startup, 0, ail_startup),
    AIL(shutdown, 0, ail_shutdown),
    AIL(set_preference, 8, ail_preference),
    AIL(get_preference, 4, ail_get_preference),
    AIL(last_error, 0, ail_last_error),
    AIL(set_redist_directory, 4, ail_set_redist_directory),
    AIL(serve, 0, ail_serve),
    AIL(get_timer_highest_delay, 0, ail_get_timer_highest_delay),
    AIL(waveOutOpen, 16, ail_wave_open),
    AIL(waveOutClose, 4, ail_wave_close),
    AIL(open_digital_driver, 16, ail_open_digital_driver),
    AIL(close_digital_driver, 4, ail_close_digital_driver),
    AIL(digital_configuration, 16, ail_configuration),
    AIL(digital_CPU_percent, 4, ail_digital_cpu_percent),
    AIL(set_digital_master_volume, 8, ail_master_volume),
    AIL(get_DirectSound_info, 12, ail_get_directsound_info),
    AIL(set_DirectSound_HWND, 8, ail_set_directsound_hwnd),
    AIL(set_named_sample_file, 20, ail_set_named_sample_file),
    AIL(mem_alloc_lock, 4, ail_mem_alloc_lock),
    AIL(mem_free_lock, 4, ail_mem_free_lock),
    AIL(file_read, 8, ail_file_read),
    AIL(file_size, 4, ail_file_size),
    AIL(allocate_sample_handle, 4, ail_allocate_sample_handle),
    AIL(release_sample_handle, 4, ail_release_sample_handle),
    AIL(init_sample, 4, ail_init_sample),
    AIL(set_sample_file, 12, ail_set_sample_file),
    AIL(start_sample, 4, ail_start_sample),
    AIL(end_sample, 4, ail_end_sample),
    AIL(sample_status, 4, ail_sample_status),
    AIL(set_sample_volume, 8, ail_set_sample_volume),
    AIL(set_sample_pan, 8, ail_set_sample_pan),
    AIL(set_sample_loop_count, 8, ail_set_sample_loop_count),
    AIL(sample_loop_count, 4, ail_sample_loop_count),
    AIL(set_sample_reverb, 16, ret0),
    AIL(sample_granularity, 4, ail_sample_granularity),
    AIL(sample_playback_rate, 4, ail_sample_playback_rate),
    AIL(set_sample_playback_rate, 8, ail_set_sample_playback_rate),
    AIL(sample_position, 4, ail_sample_position),
    AIL(set_sample_position, 8, ail_set_sample_position),
    AIL(sample_ms_position, 12, ail_sample_ms_position),
    AIL(register_EOS_callback, 8, ail_register_eos_callback),
    AIL(set_sample_user_data, 12, ail_set_sample_user_data),
    AIL(sample_user_data, 8, ail_sample_user_data),
    AIL(open_stream, 12, ail_open_stream),
    AIL(start_stream, 4, ail_start_stream),
    AIL(close_stream, 4, ail_close_stream),
    AIL(stream_status, 4, ail_stream_status),
    AIL(set_stream_volume, 8, ail_set_stream_volume),
    AIL(stream_volume, 4, ail_stream_volume),
    AIL(set_stream_loop_count, 8, ail_set_stream_loop_count),
    AIL(enumerate_3D_providers, 12, ret0),
    AIL(open_3D_provider, 4, ret1),
    AIL(close_3D_provider, 4, ret0),
    AIL(set_3D_provider_preference, 12, ret0),
    AIL(3D_provider_attribute, 12, ret0),
    AIL(allocate_3D_sample_handle, 4, ret0),
    AIL(release_3D_sample_handle, 4, ret0),
    AIL(set_3D_sample_file, 8, ret0),
    AIL(start_3D_sample, 4, ret0),
    AIL(end_3D_sample, 4, ret0),
    AIL(3D_sample_status, 4, ret_done),
    AIL(set_3D_sample_volume, 8, ret0),
    AIL(set_3D_sample_loop_count, 8, ret0),
    AIL(set_3D_position, 16, ret0),
    AIL(set_3D_orientation, 28, ret0),
    AIL(3D_update_position, 8, ret0),
};

} // namespace

void mss32_frame_pump(X86 *c) {
    if (!c)
        return;
    service(c);
}

void mss32_decode_stats(uint64_t *decodes, uint64_t *hits) {
    if (decodes)
        *decodes = g_decodes;
    if (hits)
        *hits = g_decode_hits;
}

void mss32_reset() {
    // The samples' channels went with dx_reset's channel table, and their
    // images, the static strings and every stream's channel with the arena.
    for (auto &s : g_samples)
        s = Sample{};
    for (auto &s : g_streams)
        s = Stream{};
    g_drivers.clear();
    g_next_driver = 0x10000;
    g_last_error = g_redist = 0;
    g_decoded.clear();
    g_decodes = g_decode_hits = 0;
    preferences_default();
}

void mss32_register() {
    static bool done = false;
    if (done)
        return;
    done = true;
    imports_register(g_mss32_shims, std::size(g_mss32_shims));
}
