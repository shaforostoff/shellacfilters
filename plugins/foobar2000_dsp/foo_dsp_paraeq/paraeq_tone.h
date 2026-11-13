/* ========================================
 *  foo_dsp_paraeq - tone profile of a whole track
 *
 *  Measures how much bass, brilliance and hiss a transfer carries, each as a
 *  level in dB against the same track's 500-2000 Hz body. Against the body
 *  rather than absolute, so that a quiet transfer and a loud one of the same
 *  record read the same, and so that the numbers mean "this one is duller"
 *  rather than "this one was mastered lower".
 *
 *      bass        60-200 Hz     what the low shelf at 100 Hz moves
 *      brilliance  3.5-7 kHz     what the high-mid bell at 5 kHz moves
 *      hiss        8-12 kHz      what the high shelf at 8 kHz moves
 *
 *  The hiss band stops at 12 kHz because a good part of the library is ALAC
 *  or AAC low-passed somewhere from 11 to 16 kHz; above 12 the measurement
 *  would be of the codec, not of the record.
 *
 *  Each band is a 4th-order Butterworth band-pass (two high-pass and two
 *  low-pass sections), energy summed over 50 ms frames. Only frames whose body
 *  is above the track's 30th percentile count, so the run-in groove and the
 *  gaps do not read as an enormous amount of hiss.
 *
 *  Used two ways, and the point of it being one file is that the two agree:
 *  tools/paraeq_tone_cli builds the library reference from it, and the
 *  foobar2000 component measures the playing track with it.
 *
 *  Cost: sixteen biquads per sample, mono. A three-minute track is about 0.2 s
 *  on a 2014 MacBook Air, run off the audio thread.
 *
 *  No foobar2000, VST or Win32 dependency.
 * ======================================== */

#ifndef PARAEQ_TONE_H
#define PARAEQ_TONE_H

#include <stddef.h>
#include <vector>

#include "paraeq_core.h"

namespace paraeq_tone {

enum Measure {
    kBass = 0,
    kBrilliance,
    kHiss,
    kMeasures
};

//! Edges of each measured band, Hz.
const double kBandLo[kMeasures] = {   60.0, 3500.0,  8000.0 };
const double kBandHi[kMeasures] = {  200.0, 7000.0, 12000.0 };
const double kBodyLo = 500.0, kBodyHi = 2000.0;

struct Profile {
    bool  valid = false;
    float db[kMeasures] = { 0, 0, 0 };   //!< band level against the body, dB
    int   frames = 0;                     //!< 50 ms frames that counted
};

struct Section {
    double b0, b1, b2, a1, a2;
    double z1, z2;
};

class Analyzer {
public:
    //! Starts a new track. Returns false for a rate too low to measure hiss.
    bool start(double sampleRate, unsigned channels);

    //! Interleaved samples, any number of frames per call.
    void feed(const float * interleaved, size_t frames);
    void feed(const double * interleaved, size_t frames);

    //! The profile of everything fed since start(). Invalid for under 10 s
    //! of programme.
    Profile finish();

private:
    template<typename T> void feedT(const T * in, size_t frames);
    void endFrame();

    enum { kFilters = kMeasures + 1, kSections = 4 };   // body is the last
    Section  m_f[kFilters][kSections];
    double   m_acc[kFilters] = {};
    unsigned m_channels = 0;
    size_t   m_frameLen = 0, m_inFrame = 0;
    bool     m_ok = false;
    std::vector<float> m_energy;    //!< kFilters per frame
};

//! How far the playing track is from the reference, per band, in dB. Positive
//! means it has more of that band than the library's median.
struct Deviation {
    bool  valid = false;
    float db[kMeasures] = { 0, 0, 0 };
};

//! The library medians, measured by tools/paraeq_tone_cli over C:\TangoTunes:
//! tango, vals and milonga dated before 1954. See paraeq_tone.cpp.
const Profile & reference();

//! The library's 10th and 90th percentiles, as offsets from its median: a
//! track is "clearly different" when, with the equaliser applied, it falls
//! outside them. The middle of a collection of good transfers is not something
//! to correct.
struct Spread {
    float below[kMeasures];   //!< 10th percentile minus median, negative
    float above[kMeasures];   //!< 90th percentile minus median, positive
};
const Spread & spread();

Deviation deviation(const Profile & track);

//! Gains for the three bands that would bring this track to the reference,
//! given everything else the equaliser is doing. Measured on the equaliser's
//! own curve over the same bands the analyser uses, so a shelf moved to 70 Hz
//! or a bell narrowed to Q 3 gets a suggestion that accounts for it.
//! `gainDb` is indexed by Measure: bass -> low shelf, brilliance -> high-mid
//! bell, hiss -> high shelf. `show[m]` is true where the track, as the
//! equaliser leaves it, is outside the library's spread(); following a hint
//! therefore makes it go away.
struct Suggestion {
    bool  valid = false;
    float gainDb[kMeasures] = { 0, 0, 0 };
    bool  show[kMeasures] = { false, false, false };
};

Suggestion suggest(const Deviation & d, const paraeq::Params & p, double sampleRate);

//! Whether a track's tags put it in the population the reference describes.
//! `genre` and `date` as stored; a missing date fails.
bool eligible(const char * genre, const char * date);

} // namespace paraeq_tone

#endif // PARAEQ_TONE_H
