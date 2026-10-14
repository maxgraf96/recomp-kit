// dmusic_reverb.cpp - the DirectMusic synthesizer's reverb, approximated (see
// dmusic_reverb.h for what is known and what is chosen).
#include "dmusic_reverb.h"

#include <algorithm>
#include <math.h>

namespace {

// Freeverb's tunings, in samples at 44.1 kHz: mutually prime, so the combs'
// echoes do not line up, and the right channel's are 23 samples longer, which
// is what decorrelates the two sides.
const int kCombTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
const int kAllpassTuning[4] = {556, 441, 341, 225};
const int kStereoSpread = 23;
const float kAllpassFeedback = 0.5f;

float db_to_gain(float db) {
    return powf(10.0f, db / 20.0f);
}

// Keeps a decaying tail out of the denormal range.
inline float flush(float v) {
    return fabsf(v) < 1e-25f ? 0.0f : v;
}

} // namespace

void WavesReverb::configure(double rate, const WavesReverbParams &p) {
    if (rate <= 0.0)
        rate = 44100.0;
    const bool resize = rate != rate_;
    rate_ = rate;
    const double scale = rate / 44100.0;

    // The ranges DirectX documents for DMUS_WAVES_REVERB_PARAMS.
    const double time_s = fmin(fmax(p.time_ms, 1.0), 3000.0) / 1000.0;
    const double ratio = fmin(fmax(p.hf_ratio, 0.001), 0.999);
    const float in_gain_db = fminf(fmaxf(p.in_gain_db, -96.0f), 0.0f);
    const float mix_db = fminf(fmaxf(p.mix_db, -96.0f), 0.0f);

    double sum_g2 = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < kCombs; ++i) {
            Comb &c = comb_[ch][i];
            const uint32_t d = (uint32_t)lround((kCombTuning[i] + ch * kStereoSpread) * scale);
            if (resize || c.line.size() != d) {
                c.line.assign(d ? d : 1, 0.0f);
                c.pos = 0;
                c.store = 0.0f;
            }
            // Jot's rule: after T seconds the loop has run T*fs/d times and
            // must have lost 60 dB, at DC with g and at Nyquist with g_hf.
            const double loops = time_s * rate / (double)d;
            const double g = pow(10.0, -3.0 / loops);
            const double g_hf = pow(10.0, -3.0 / (loops * ratio));
            // A one-pole lowpass (1-a)/(1-a z^-1) passes DC whole and the
            // Nyquist frequency at (1-a)/(1+a); that must be g_hf / g.
            const double r = fmax(g_hf / g, 0.01);
            c.feedback = (float)g;
            c.damp = (float)((1.0 - r) / (1.0 + r));
            if (ch == 0)
                sum_g2 += g * g;
        }
        for (int i = 0; i < kAllpasses; ++i) {
            Allpass &a = allpass_[ch][i];
            const uint32_t d = (uint32_t)lround((kAllpassTuning[i] + ch * kStereoSpread) * scale);
            if (resize || a.line.size() != d) {
                a.line.assign(d ? d : 1, 0.0f);
                a.pos = 0;
            }
        }
    }
    // White noise of power P through a comb with feedback g comes out at
    // P / (1 - g^2); eight of them, summed incoherently, at 8 P / (1 - g^2).
    // The allpasses keep the power. So this input scale puts a steady signal's
    // reverberation at the dry signal's power before the mix applies.
    const double mean_g2 = sum_g2 / kCombs;
    input_scale_ = (float)sqrt((1.0 - mean_g2) / kCombs) * db_to_gain(in_gain_db);
    wet_ = mix_db <= -96.0f ? 0.0f : db_to_gain(mix_db);
}

void WavesReverb::clear() {
    for (int ch = 0; ch < 2; ++ch) {
        for (Comb &c : comb_[ch]) {
            std::fill(c.line.begin(), c.line.end(), 0.0f);
            c.store = 0.0f;
        }
        for (Allpass &a : allpass_[ch])
            std::fill(a.line.begin(), a.line.end(), 0.0f);
    }
}

void WavesReverb::process(float *left, float *right, uint32_t frames) {
    if (rate_ <= 0.0 || wet_ == 0.0f)
        return;
    for (uint32_t n = 0; n < frames; ++n) {
        // A mono send, as Freeverb takes it; the stereo comes from the tunings.
        const float in = (left[n] + right[n]) * 0.5f * input_scale_;
        float out[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < 2; ++ch) {
            for (Comb &c : comb_[ch]) {
                const float y = c.line[c.pos];
                c.store = flush(y * (1.0f - c.damp) + c.store * c.damp);
                c.line[c.pos] = in + c.store * c.feedback;
                if (++c.pos == c.line.size())
                    c.pos = 0;
                out[ch] += y;
            }
            // Schroeder allpasses, (z^-d - g) / (1 - g z^-d): they smear the
            // echoes without changing the level. (Freeverb's own are not
            // allpass and add 3.7 dB each to noise.)
            for (Allpass &a : allpass_[ch]) {
                const float delayed = a.line[a.pos];
                const float w = flush(out[ch] + kAllpassFeedback * delayed);
                a.line[a.pos] = w;
                out[ch] = delayed - kAllpassFeedback * w;
                if (++a.pos == a.line.size())
                    a.pos = 0;
            }
        }
        left[n] += out[0] * wet_;
        right[n] += out[1] * wet_;
    }
}
