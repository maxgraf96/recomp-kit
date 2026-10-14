// dmusic_reverb.h - the DirectMusic synthesizer's reverb, approximated.
//
// The software synthesizer behind a DirectMusic port has one effect a game
// controls: a reverb licensed from Waves, switched by DMUS_EFFECT_REVERB and
// set through GUID_DMUS_PROP_WavesReverb with four numbers:
//
//   fInGain           dB on the signal entering the reverb (0 = unchanged)
//   fReverbMix        dB of the reverberated signal against the dry one
//   fReverbTime       ms for the tail to fall 60 dB
//   fHighFreqRTRatio  how much faster the high frequencies fall (0.001..0.999)
//
// Its algorithm is not published. This is a Schroeder-Moorer network in the
// shape Jezar's public-domain Freeverb made common - eight damped feedback
// combs per channel into four Schroeder allpasses - with each comb's feedback set from
// the decay time (Jot's rule, g = 10^(-3 d / (T fs))) and a one-pole lowpass
// in each loop that makes the decay at the Nyquist frequency T * ratio. The
// wet signal is normalised so that a steady input comes out at the same power
// with the mix at 0 dB, so `fReverbMix` reads as the reverb's level under the
// dry signal; the dry signal passes unchanged. How loud the Waves reverb is at
// a given mix has not been measured against Windows: this is the calibration
// a reference recording with the reverb on and off settles.
#pragma once
#include <stdint.h>
#include <vector>

struct WavesReverbParams {
    float in_gain_db = 0.0f;
    float mix_db = -10.0f;   // DirectX's default for the synthesizer's reverb
    float time_ms = 1000.0f; // default
    float hf_ratio = 0.001f; // default
};

class WavesReverb {
  public:
    // Sets the sample rate and the parameters; the tail is cleared when the
    // rate changes, kept when only the parameters do.
    void configure(double rate, const WavesReverbParams &p);
    // Adds the reverberated signal of `frames` stereo frames, deinterleaved,
    // into the same buffers: dry in, dry plus wet out.
    void process(float *left, float *right, uint32_t frames);
    // Empties the tail.
    void clear();
    double rate() const {
        return rate_;
    }

  private:
    struct Comb {
        std::vector<float> line;
        uint32_t pos = 0;
        float feedback = 0.0f, damp = 0.0f, store = 0.0f;
    };
    struct Allpass {
        std::vector<float> line;
        uint32_t pos = 0;
    };
    static constexpr int kCombs = 8, kAllpasses = 4;
    Comb comb_[2][kCombs];
    Allpass allpass_[2][kAllpasses];
    double rate_ = 0.0;
    float input_scale_ = 0.0f; // in gain times the normalisation
    float wet_ = 0.0f;         // the mix as a linear gain
};
