// dmusic_render.cpp - renders a game's DirectMusic segment offline through the
// DirectMusic shim, the way a DirectX 6/7 game sets it up and plays it, and
// reports its level. For checking a game's own music against reference
// numbers: nothing about any game is in here, and nothing is played aloud.
//
//   dmusic_render --dir DIR --segment NAME [--seconds S] [--repeats N]
//                 [--groove G] [--schedule T:G,T:G...] [--volume-mb MB]
//                 [--reverb IN,MIX,TIME,RATIO] [--rate HZ] [--wav OUT]
//
// DIR is the music folder (the search directory), NAME the segment file in it.
// The master groove level starts at G (default 0); each T:G of the schedule
// sets it to G at T seconds and asks for a groove transition at the next
// measure, as a game's intensity change does. The port volume is MB hundredths
// of a dB (default 0); the reverb is off unless given. Prints one JSON object:
// the level of the whole render and of each schedule phase in dB of full
// scale, the peak, the samples beyond full scale, the share of silent 100 ms
// windows, the load time and the render speed. --wav writes the render as
// 16-bit stereo, which stays wherever OUT is: never in a repository.
#include "dmusic_client.h"

#include "../dx.h"
#include "../../runtime/win32.h"

#include <stdlib.h>

namespace {

struct Phase {
    double at = 0.0;
    int groove = 0;
};

void wav_u32(FILE *f, uint32_t v) {
    fwrite(&v, 4, 1, f);
}
void wav_u16(FILE *f, uint16_t v) {
    fwrite(&v, 2, 1, f);
}

void write_wav(const char *path, const Audio &a) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "dmusic_render: cannot write %s\n", path);
        return;
    }
    const uint32_t frames = (uint32_t)a.frames(), rate = (uint32_t)a.rate;
    fwrite("RIFF", 1, 4, f);
    wav_u32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    wav_u32(f, 16);
    wav_u16(f, 1);
    wav_u16(f, 2);
    wav_u32(f, rate);
    wav_u32(f, rate * 4);
    wav_u16(f, 4);
    wav_u16(f, 16);
    fwrite("data", 1, 4, f);
    wav_u32(f, frames * 4);
    for (uint32_t i = 0; i < frames; ++i) {
        for (float v : {a.left[i], a.right[i]}) {
            const float c = std::max(-1.0f, std::min(1.0f, v));
            const int16_t s = (int16_t)lrintf(c * 32767.0f);
            fwrite(&s, 2, 1, f);
        }
    }
    fclose(f);
}

double seconds_since(uint64_t t0) {
    return (double)(os_monotonic_ns() - t0) * 1e-9;
}

int usage() {
    fprintf(stderr, "usage: dmusic_render --dir DIR --segment NAME [--seconds S] [--repeats N] "
                    "[--groove G] [--schedule T:G,...] [--volume-mb MB] "
                    "[--reverb IN,MIX,TIME,RATIO] [--rate HZ] [--wav OUT]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    std::string dir, segment, schedule_text, wav;
    double seconds = 30.0, rate = 48000.0;
    uint32_t repeats = 100000;
    int groove = 0, volume_mb = 0;
    bool reverb = false;
    float reverb_params[4] = {0.0f, -10.0f, 1000.0f, 0.001f};
    for (int i = 1; i < argc; ++i) {
        const std::string opt = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (!value)
            return usage();
        ++i;
        if (opt == "--dir")
            dir = value;
        else if (opt == "--segment")
            segment = value;
        else if (opt == "--seconds")
            seconds = atof(value);
        else if (opt == "--repeats")
            repeats = (uint32_t)strtoul(value, nullptr, 10);
        else if (opt == "--groove")
            groove = atoi(value);
        else if (opt == "--schedule")
            schedule_text = value;
        else if (opt == "--volume-mb")
            volume_mb = atoi(value);
        else if (opt == "--rate")
            rate = atof(value);
        else if (opt == "--wav")
            wav = value;
        else if (opt == "--reverb") {
            reverb = sscanf(value, "%f,%f,%f,%f", &reverb_params[0], &reverb_params[1],
                            &reverb_params[2], &reverb_params[3]) == 4;
            if (!reverb)
                return usage();
        } else
            return usage();
    }
    if (dir.empty() || segment.empty() || seconds <= 0.0 || rate <= 0.0)
        return usage();
    std::vector<Phase> schedule;
    for (const char *p = schedule_text.c_str(); *p;) {
        char *end = nullptr;
        Phase ph;
        ph.at = strtod(p, &end);
        if (!end || *end != ':')
            return usage();
        ph.groove = (int)strtol(end + 1, &end, 10);
        schedule.push_back(ph);
        p = *end == ',' ? end + 1 : end;
    }

    setvbuf(stdout, nullptr, _IONBF, 0);
    mem_init();
    imports_init();
    dmusic_enable_for_test(1);
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    cpu_reset();
    g_scratch = heap_alloc(0x8000, true, 16);
    win32_init(dir);
    g_guest_dir = win32_guest_path(dir);
    g_rate = rate;

    Music m = open_music();
    set_groove(m, (int8_t)groove);
    set_volume_mb(m, volume_mb);
    set_reverb(m, reverb, reverb_params[0], reverb_params[1], reverb_params[2], reverb_params[3]);
    uint64_t t0 = os_monotonic_ns();
    uint32_t hr = 0;
    const uint32_t seg = load(m, segment.c_str(), &hr);
    const double load_ms = seconds_since(t0) * 1000.0;
    if (!seg) {
        printf("{\"segment\": \"%s\", \"error\": \"GetObject returned %#x\"}\n", segment.c_str(),
               hr);
        return 1;
    }
    const uint32_t state = play(m, seg, repeats);
    if (state)
        call_method(state, RELEASE, {});

    // Render, changing the groove where the schedule says.
    Audio a;
    t0 = os_monotonic_ns();
    std::vector<double> bounds = {0.0};
    for (const Phase &ph : schedule)
        if (ph.at > 0.0 && ph.at < seconds)
            bounds.push_back(ph.at);
    bounds.push_back(seconds);
    for (size_t i = 0; i + 1 < bounds.size(); ++i) {
        for (const Phase &ph : schedule)
            if (ph.at == bounds[i] || (i == 0 && ph.at <= 0.0)) {
                set_groove(m, (int8_t)ph.groove);
                call_method(m.composer, COMPOSER_AUTOTRANSITION,
                            {m.perf, seg, 0 /* GROOVE */, 0x20 /* MEASURE */, 0, 0, 0, 0});
            }
        render(a, bounds[i + 1] - bounds[i], rate);
    }
    const double cpu = seconds_since(t0);

    // The measurements.
    double peak = 0.0;
    uint64_t clipped = 0;
    for (size_t i = 0; i < a.frames(); ++i)
        for (float v : {a.left[i], a.right[i]}) {
            peak = std::max(peak, (double)fabsf(v));
            clipped += fabsf(v) > 1.0f;
        }
    size_t windows = 0, silent = 0;
    for (double t = 0.0; t + 0.1 <= seconds; t += 0.1, ++windows)
        silent += rms_db(a, t, t + 0.1) < -60.0;
    printf("{\"segment\": \"%s\", \"seconds\": %.1f, \"rate\": %.0f, \"rms_db\": %.2f, "
           "\"peak\": %.4f, \"clipped\": %llu, \"silent_pct\": %.1f, \"load_ms\": %.1f, "
           "\"x_realtime\": %.0f, \"phases\": [",
           segment.c_str(), seconds, rate, rms_db(a, 0.0, seconds), peak,
           (unsigned long long)clipped, windows ? 100.0 * silent / windows : 0.0, load_ms,
           cpu > 0.0 ? seconds / cpu : 0.0);
    for (size_t i = 0; i + 1 < bounds.size(); ++i)
        printf("%s{\"from\": %.1f, \"to\": %.1f, \"rms_db\": %.2f}", i ? ", " : "", bounds[i],
               bounds[i + 1], rms_db(a, bounds[i], bounds[i + 1]));
    printf("]}\n");
    if (!wav.empty())
        write_wav(wav.c_str(), a);
    call_method(seg, RELEASE, {});
    close_music(m);
    return 0;
}
