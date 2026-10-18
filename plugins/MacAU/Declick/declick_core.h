/* ========================================
 *  foo_dsp_declick - portable DSP core
 *
 *  A detect-and-interpolate declicker for mono/stereo transfers of shellac and
 *  vinyl. This is the classic autoregressive approach (Vaseghi, Godsill &
 *  Rayner) and it is what dedicated declick tools do:
 *
 *    1. Fit an AR model to a block of audio.
 *    2. Clicks are broadband impulses, so they blow up the AR prediction
 *       residual while music does not. Threshold the residual against a robust
 *       local scale, with hysteresis so the whole burst is captured.
 *    3. Replace the flagged samples with the least squares interpolation
 *       implied by the model and the surrounding good audio - reconstruct what
 *       the waveform should have been, rather than smoothing what is there.
 *
 *  Step 3 is what Airwindows DeCrackle has no equivalent of, and step 2 is why
 *  DeCrackle cannot work on mono material at all: its detector keys on the
 *  L*R cross product, which collapses to x^2 when both channels are the same.
 *
 *  No foobar2000, VST or Win32 dependency, so the maths can be tested against
 *  the Python reference implementation directly.
 * ======================================== */

#ifndef DECLICK_CORE_H
#define DECLICK_CORE_H

#include <stddef.h>
#include <stdint.h>

#include <new>
#include <vector>

namespace declick {

enum {
    kMinOrder = 8,
    //! 256 rather than 64 because model order turned out to be the one lever
    //! that clearly pays: measured against injected-click ground truth, order
    //! 128 takes whole-file error from +0.60 to +1.31 dB and order 256 to
    //! +2.55 dB, with less collateral damage and less residual crackle at the
    //! same time. It costs real CPU, so the default sits at 128 rather than at
    //! the top of the range - see the README for the throughput figures.
    kMaxOrder = 256,
    kBlock    = 512      //!< analysis hop; also sets most of the latency
};

//! User-facing controls, all normalised 0..1 except where noted.
struct Params {
    float sensitivity;  //!< 0 = only the most obvious clicks, 1 = aggressive
    float extent;       //!< how far a detection spreads outwards into its tail
    float maxLengthMs;  //!< longest single repair, in milliseconds
    float depth;        //!< 0 = least added error, 1 = replace damaged samples outright
    int   passes;       //!< 1..3; a second pass catches clicks the first uncovers
    int   order;        //!< AR model order
    float dryWet;       //!< 0 = bypass, 1 = full repair
    //! Output what the repair takes out - input minus output - instead of the
    //! repaired audio. For listening to what is being removed: it should be
    //! crackle and nothing else, and any music heard in it is collateral.
    bool  delta;

    static Params defaults() {
        Params p;
        // 0.9 puts the trigger at 2.85 sigma. It was 0.8 (3.2 sigma), chosen
        // by ear on three full shellac transfers, until weak detections got a
        // partial repair (Config::softMin): with that, 0.9 reaches the small
        // clicks 0.8 did and takes less from clean records. See the README.
        p.sensitivity = 0.9f;
        p.extent      = 0.5f;
        p.maxLengthMs = 4.0f;
        // 1 replaces the damaged samples outright. The old default of 0, a
        // 45% subtraction, was chosen for least whole-file error, and that
        // capped every repair at -5.2 dB however good the estimate was: a
        // -24 dBFS click on a pure tone came out only 5 dB quieter.
        p.depth       = 1.0f;
        p.passes      = 2;
        // 128. Model order is the one lever that clearly pays - see kMaxOrder.
        // On the click probes it is worth about 1 dB of removal over 64 and
        // 1.5 dB less harm, at about 1.9x the CPU - still 24x real time stereo
        // on a 2014 Haswell MacBook Pro, measured. Drop to 64 where that is
        // too much; it gives up surprisingly little.
        // Higher orders model the music better, so they change clean
        // records least: removed from the Malvon master, -35.4 dB at 64,
        // -36.4 at 128, -38.7 at 256. But they also predict some short
        // clicks well enough to miss them - clicks above 8 sigma left on a
        // crackly D'Arienzo MP3 go 1.63/s, 1.93/s, 2.57/s - so 128 stays
        // the balance, 256 is for clean records, and any even order is as
        // valid as a power of two (CPU scales smoothly with it).
        p.order       = 128;
        p.dryWet      = 1.0f;
        p.delta       = false;
        return p;
    }
    void sanitize();
    bool operator==(const Params & o) const;
    bool operator!=(const Params & o) const { return !(*this == o); }
};

//! Everything derived from (params, sample rate).
struct Config {
    int    order        = 32;
    int    maxRun       = 176;   //!< longest repair, samples
    int    passes       = 2;
    int    madWindow    = 6615;  //!< samples used for the robust noise estimate
    double thresholdHi  = 3.5;   //!< trigger
    double thresholdLo  = 1.6;   //!< hysteresis extension
    double depth        = 0.0;   //!< pushes the weighting towards replacement
    double wet          = 1.0;
    bool   delta        = false;   //!< see Params::delta

    //! Subtractive repair. Calibration constants, not derived from params and
    //! not user-facing.
    //!
    //! `wienerMax` is the fraction of the click estimate that gets subtracted
    //! at depth 0; depth scales it towards 1, so at the default depth of 1 it
    //! plays no part. 0.45 was measured against injected-click ground truth
    //! and against four reference transfers; it beats the length-indexed
    //! curve it replaced on every axis of both.
    //!
    //! `wienerAlpha` scales the model's own per-sample estimate variance,
    //! turning the flat fraction into a true Wiener gain. It is 0 - i.e. off -
    //! because it was measured and does not pay on 78 rpm material: the click
    //! is nearly always far larger than the estimate's uncertainty, so the
    //! gain saturates and only the cap binds. Retained because it costs
    //! nothing when off and material with different click statistics may
    //! behave differently; declick_cli can sweep it. See the README.
    double wienerAlpha  = 0.0;
    double wienerMax    = 0.45;

    //! Re-repair. Not user-facing; declick_cli can sweep it.
    //! Each pass refits the model on the window the previous one cleaned, but
    //! the runs that pass repaired were repaired with the model it had, which
    //! was fitted on the crackle as well as the music. 1 repairs them again
    //! with the refitted model at the start of every later pass; 2 also
    //! refits once more after the last pass and repairs everything found in
    //! the block again; 0 repairs every run exactly once.
    //!
    //! 1 because on dense crackle it takes the largest events from -6.8 to
    //! -8.9 dB removed for ~1 dB of harm and 9% more CPU; 2 adds another
    //! 0.6 dB on those for a further 27%. It needs passes >= 2 to do
    //! anything at all at 1.
    int    rerepair     = 1;

    //! How many samples below the hysteresis floor a run may step over, and
    //! how far above the floor - as a fraction of the way up to the trigger -
    //! the detection function must be on the other side. Not user-facing;
    //! declick_cli sweeps them as --dip and --dip-far. The figures in this
    //! paragraph predate dipPeak, below.
    //! A click's prediction error rings, and on band-limited material - an
    //! MP3 is empty above 16 kHz - it swings through zero inside the click.
    //! Where that dipped under the floor the run ended early, and the repair
    //! leaned on damaged samples just past its edge: it left the click in,
    //! sometimes rebuilt it larger. On such a transfer (Canaro, Hotel
    //! Victoria) these settings take the clicks left above 8 sigma from
    //! 1.1/s to 0.48/s; the injected crackle loses -6.8 dB instead of -6.1,
    //! -10.4 instead of -8.9 on the largest events, for 1.1 dB of whole-file
    //! SNR. Raising Extent instead pays more for less.
    //! dipFar 0 (any return above the floor) went further, to 0.3/s, but
    //! also spread into the attacks of clean, crisp recordings - audibly so
    //! on a clean D'Arienzo transfer, where it changed the music by -30.9 dB
    //! against -46.4 with no dip at all; 0.5 changes it by -35.6 and was
    //! heard to remove no more attack than none. dip 0 is plain hysteresis.
    //!
    //! dipPeak (--dip-peak): only a run whose detection peaks at this many
    //! sigma may step over a dip. Most runs that are stretched barely cross
    //! the trigger - noise alone crosses 3.2 sigma ~100 times a second - and
    //! on clean material those carried most of what the dip removed: two
    //! thirds of it on the Malvon master. At 6 the Malvon master goes from
    //! -33.6 to -36.4 dB removed (-36.8 with no dip), the clean D'Arienzo
    //! from -35.6 to -45.5 (-46.4), while a crackly D'Arienzo MP3 keeps
    //! 85% of what the dip gained on clicks above 8 sigma. Benchmark: SNR
    //! back to 25.4 dB, change to clean music -34.6, largest crackle -9.8.
    //! Preferred by ear over 5 and over no limit.
    int    dip          = 1;
    double dipFar       = 0.5;
    double dipPeak      = 6.0;

    //! Partial repair of weak detections. Not user-facing; declick_cli sweeps
    //! them as --soft-min and --soft-span. A run whose detection peaks right
    //! at the trigger has only softMin of its discrepancy taken out, rising
    //! linearly to all of it softSpan sigma above the trigger.
    //! A run that barely crosses the trigger is about as likely noise or
    //! music as a click - noise alone crosses 3.2 sigma ~100 times a second -
    //! and on clean records those runs carry most of what is removed. Raising
    //! the trigger instead loses the small clicks with the false ones;
    //! repairing them by half keeps most of what they were worth. Together
    //! with Sensitivity 0.9 (trigger 2.85 sigma), against the hard repair at
    //! 0.8: the Malvon master -36.4 -> -38.6 dB removed, the clean D'Arienzo
    //! -45.5 -> -47.3, while a crackly D'Arienzo MP3 keeps 2.08/s clicks
    //! above 8 sigma against 1.93 (Sensitivity 0.7 alone: -37.2 dB, 2.28/s).
    //! Benchmark: small crackle -3.9 dB removed against -3.8, all of it
    //! -6.2 against -6.4, harm -15.5 against -13.9. Chosen by ear over 0.6.
    //! The half repair is what m_win holds, so the refits and the detection
    //! see it, but a repair's estimate is solved against Channel::m_full,
    //! where every run is in full; see there for why.
    double softMin      = 0.5;
    double softSpan     = 3.5;

    //! The body stage. Not user-facing; declick_cli sweeps it as --body,
    //! --body-floor, --body-peak, --body-rate0 and --body-rate1.
    //! Each crackle on a shellac transfer is a sharp core and a smooth bump
    //! around it, 0.3-0.6 ms wide. The detector finds and repairs the core,
    //! but an AR model predicts the bump as if it were music. Under a held note
    //! the bumps come at ~750 a second, and in 1-2 kHz they are heard as a
    //! "squirt" that the decrackled version removes. At that density they are
    //! not events that could be detected one by one: they are a carpet about
    //! 12 dB under the note in their band, as loud next to a repaired core as
    //! away from one.
    //! So the stage treats them as noise. In an STFT of the repaired audio
    //! (N ~46 ms, hop N/4), a bin in bodyLo..bodyHi is cut by its non-tonal
    //! share H^2 / (H^2 + P^2), never below bodyFloor. P is the median over
    //! ~370 Hz of the same frame. H is the louder of the medians over the
    //! frames before and the frames after, so that an onset or a release
    //! counts as tonal. How much of that cut is applied is the key: the
    //! number of runs per second that peak at bodyPeak sigma or more, over
    //! 0.1 s, and then the minimum of that over the last 0.5 s. Crackle on a
    //! surface is steady, and the bursts musical attacks fire come and go. The
    //! cut ramps in from bodyRate0 to bodyRate1, and once open it is held for
    //! bodyRelease seconds: at 0.1 s the count is a handful of runs, and its
    //! minimum dips below bodyRate0 under crackle that has not changed, which
    //! let the squirt back in patches. Where the key never opens the stage
    //! subtracts nothing, and the output is the repaired audio to the bit.
    //! On the held note of Tanturi's *Cuatro compases* this takes 45% of what
    //! the decrackled version takes from 1-2 kHz, against 8% without it. On
    //! the Malvon master and clean D'Arienzo transfers the key never opens.
    //! The floor was chosen by ear: at 0.5 (-6 dB) the squirt could still be
    //! heard, and at 0.25 it could not. See the README.
    //! It costs bodyLatency samples of latency, ~70 ms at 44.1 kHz. A host
    //! that reads ahead does not hear that as delay.
    int    body         = 1;
    double bodyLo       = 500.0;
    double bodyHi       = 3000.0;
    double bodyFloor    = 0.25;
    double bodyPeak     = 6.0;
    double bodyRate0    = 20.0;    //!< strong runs per second
    double bodyRate1    = 60.0;
    double bodyRateWin  = 0.1;     //!< seconds
    double bodyHold     = 0.5;     //!< seconds
    double bodyRelease  = 2.0;     //!< seconds
    int    bodyN        = 2048;    //!< from the sample rate alone
    int    bodyLatency  = 0;       //!< bodyDelay(bodyN) if body, else 0

    //! The fine stage. Not user-facing; declick_cli sweeps it as --fine,
    //! --fine-lo, --fine-hi, --fine-alpha, --fine-q and --fine-floor.
    //! Above ~4 kHz a shellac transfer's music is weak and its crackle is not:
    //! each crackle leaves a burst of ringing 1-2 ms long there, 5-10 times
    //! the floor between them, and on dense crackle they come every few
    //! milliseconds. The detector repairs the largest. The decrackled version
    //! takes nearly all of them, and on Tanturi's *Tu olvido* (Videla, Ribo,
    //! 1946) only its version of the band above 4 kHz was heard as clean.
    //! In an STFT of the repaired audio with frames of ~2.9 ms (hop N/4, a
    //! square-root Hann either side), a bin from fineLo up is clipped to
    //! fineAlpha times its background, never below fineFloor. The background
    //! is the louder of the fineQ quantiles over the fineM frames before and
    //! the fineM after: a quantile low enough to fall between bursts that
    //! come every 4 ms, which a median does not, and the louder side so an
    //! onset or a release is left alone. It runs alongside the body stage on
    //! the same repaired audio and adds to what that stage removes; the two
    //! bands do not meet. It is gated by the body stage's key, so where that
    //! is shut it subtracts nothing, and the Malvon master and the clean
    //! D'Arienzo transfers come out to the bit as without it. Its delay, a
    //! frame and the larger of fineM hops and the voice guard's 24 ms (below),
    //! fits inside the body stage's, so it adds no latency. It needs the body
    //! stage, and is off when that is.
    //! alpha 2 was heard as good on *Tu olvido*; it is the gentler of the two
    //! settings auditioned (1.5 takes more). See the README.
    int    fine         = 1;
    double fineLo       = 4500.0;
    //! Bins above this are not processed. Nothing in them can be heard, and at
    //! 96 kHz they would be two thirds of the stage's work.
    double fineHi       = 20000.0;
    double fineAlpha    = 2.0;
    double fineQ        = 0.2;
    double fineFloor    = 0.125893;  //!< -18 dB
    int    fineM        = 16;        //!< frames either side; sizes buffers
    int    fineN        = 128;       //!< from the sample rate alone
    //! The voice guard. A voice's top band, and a reed's, comes as one burst
    //! per period, every 4-8 ms, and the clip above cannot tell that from
    //! crackle: on *Tu olvido* it cut the singers' 4.5-8 kHz pulse by pulse,
    //! heard as vocals "split", where the decrackled version leaves that band
    //! almost alone. So below fineMid the cut backs off as far as the band's
    //! envelope repeats: by the normalised autocorrelation of the fineLo ..
    //! fineMid envelope over +-24 ms, at its best lag in 2.5-12 ms, it is full
    //! at finePeriodic0 and gone at finePeriodic1. Above fineMid a voice on
    //! shellac has little, and the clip stays as it was. Back-offs from 0.3 to
    //! 0.6 and from 0.2 to 0.4 were heard as keeping the vocals whole, and as
    //! leaving less crackle than starting the stage at fineMid. Asked for the
    //! least destructive, the user chose 0.1 to 0.3, which backs off sooner
    //! still.
    double fineMid       = 8000.0;
    double finePeriodic0 = 0.1;
    double finePeriodic1 = 0.3;
    double rate         = 44100.0;   //!< the sample rate compute() was given

    int    pad          = 272;   //!< context needed either side of a block
    int    latency      = 784;

    //! Buffer envelope: every allocation Channel makes is sized from these
    //! three, and they are derived from the sample rate alone - never from the
    //! parameters.
    //!
    //! That is what makes a parameter change allocation-free even when it
    //! resizes the pipeline. configure() reassigns its vectors to sizes they
    //! already have, and neither assign() nor resize() reallocates below the
    //! capacity it is already holding. Only a sample rate change - or the first
    //! configure() - is allowed to touch the heap, because a real-time audio
    //! thread must not.
    //!
    //! bufMaxRun is the longest repair Params::sanitize() can produce (its
    //! 20 ms ceiling), and bufOrder is kMaxOrder, so no setting of Max repair
    //! or Model order can exceed them.
    int    bufOrder     = (int)kMaxOrder;
    int    bufMaxRun    = 882;
    int    bufPad       = 1074;

    //! Largest single push() the caller promises to make between pulls. The
    //! output ring is sized for it; a longer push is handled correctly but
    //! grows the ring once, which allocates. Callers that push and pull in
    //! lockstep never come near it.
    int    maxBlock     = 16384;

    void compute(const Params & p, double sampleRate);

    //! The body stage's delay for transform length n: 1.5 n - 1, in whole
    //! blocks. See compute().
    static int bodyDelay(int n);

    //! The fine stage at transform length n, hop n/4, at `rate`: the voice
    //! guard's half window, 24 ms in frames; and how many frames the cut one
    //! trails the newest, the larger of that and fineM.
    int fineWindow(int n) const;
    int fineDelay(int n) const;

    //! True if `o` needs exactly the buffers this config already has, i.e. it
    //! can be swapped into a running Channel with retune() instead of
    //! configure(), which reallocates and therefore resets.
    //!
    //! Only `order`, `maxRun`, `madWindow`, `body`, `bodyN`, `fineN` and
    //! `fineM` size anything or set the latency (`pad` and `latency` are
    //! derived from them); thresholds, depth, wet, delta, passes and whether
    //! the fine stage runs are read per block and can change under a live
    //! stream.
    bool structurallyEquals(const Config & o) const {
        return order == o.order && maxRun == o.maxRun && madWindow == o.madWindow
            && body == o.body && bodyN == o.bodyN && fineN == o.fineN
            && fineM == o.fineM && rate == o.rate;
    }
};

//! Flush-to-zero for the duration of a scope. The AR fit and the Cholesky
//! solve both run recursions down towards zero, so denormals are reachable and
//! expensive. Every wrapper must hold one of these across its processing loop:
//! FTZ changes results in the last bits, so it is part of the numerical
//! contract rather than an optimisation, and the ports are only comparable to
//! each other while they all agree about it.
//!
//! MXCSR.FTZ on x86, FPCR.FZ on AArch64. Holding only the x86 half - which is
//! all this did until the ARM builds appeared - leaves the two ports disagreeing,
//! which is the one thing the contract above exists to prevent.
class scoped_flush_denormals {
public:
    scoped_flush_denormals(const scoped_flush_denormals &) = delete;
    void operator=(const scoped_flush_denormals &) = delete;
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__) || \
    defined(_M_ARM64) || defined(__aarch64__)
    scoped_flush_denormals();
    ~scoped_flush_denormals();
private:
    unsigned m_saved;   //!< MXCSR, or the defined low half of FPCR
#else
    scoped_flush_denormals() {}
#endif
};

//! Allocator that starts every block on a 64-byte cache line, for the buffers
//! the residual filters read and write. The Windows heap and glibc only
//! promise 16 bytes, and on Haswell under Windows x64 the window landed 4
//! doubles off a line, or the residuals did, depending on the heap's state.
//! With them on a line, declick ran 1-2% faster end to end there at orders
//! 128 and 256, and the output did not change.
//!
//! macOS malloc serves these sizes from a zone with a 512-byte quantum, so they
//! are on a line already and Apple builds keep the plain std::vector. That is
//! also what lets the Xcode projects stay C++11: this needs C++17 aligned new,
//! which every other build of this file now compiles with.
//!
//! That only lines up the hot reads while maxRun + 2*order is a multiple of 8,
//! which the defaults are at every rate but 176.4 kHz; see
//! alignment-research.md for the lead in front of the window that would hold
//! it at any setting, and why it was not worth it.
#if !defined(__APPLE__)
template <class T> struct CacheAlignedAlloc {
    typedef T value_type;
    CacheAlignedAlloc() {}
    template <class U> CacheAlignedAlloc(const CacheAlignedAlloc<U> &) {}
    T * allocate(size_t n) {
        return (T *)::operator new(n * sizeof(T), std::align_val_t(64));
    }
    void deallocate(T * p, size_t) {
        ::operator delete(p, std::align_val_t(64));
    }
    template <class U> bool operator==(const CacheAlignedAlloc<U> &) const { return true; }
    template <class U> bool operator!=(const CacheAlignedAlloc<U> &) const { return false; }
};
#endif

//! One independent channel of processing.
class Channel {
public:
    //! Allocates nothing: configure() is what sizes the pipeline, and nothing
    //! else may be called until it has been. The constructor used to configure
    //! itself at 44.1 kHz defaults, which meant every Channel built a whole
    //! envelope for its caller to replace on the next line - 2.1 MB of it, on
    //! whatever thread the caller happened to be.
    Channel();

    void configure(const Config & cfg);
    void reset();

    //! Swap in a config that needs the same buffers, without resetting - so a
    //! host moving Sensitivity, Extent, Repair depth, Passes or Dry/Wet does
    //! not punch a `latency`-sample hole in the audio. Returns false if the new
    //! config is structurally different, in which case the caller has to
    //! configure() and accept the discontinuity.
    bool retune(const Config & cfg);

    //! Streaming, push/pull because the pipeline holds config().latency
    //! samples: feed everything, then take whatever is ready.
    //!
    //!   ch.push(in, n, stride);
    //!   size_t k = ch.available();
    //!   ch.pull(out, k, stride);
    //!
    //! All channels configured alike consume and produce in lockstep, so one
    //! available() query covers the whole frame.
    template<typename Sample>
    void push(const Sample * in, size_t frames, size_t stride);

    size_t available() const { return m_outCount; }

    template<typename Sample>
    void pull(Sample * out, size_t frames, size_t stride);

    //! End of stream: run `latency` zeros through so the tail comes out.
    void drain();

    //! Pre-roll, for a host that insists on n samples out for n samples in.
    //! Feeding `latency` zeros up front makes `available() >= n` hold after
    //! every subsequent push of n - for any n, any block size - so push/pull
    //! pairs can run forever without underrunning. The cost is that the output
    //! is then delayed by `latency` relative to the input, which is what a VST
    //! declares through setInitialDelay(). Mechanically the same operation as
    //! drain(); named separately because the intent is the opposite one.
    //!
    //! A host that can READ AHEAD does not have to pay that delay and should
    //! not call this at all. What the repair needs is the samples that come
    //! after the ones it is fixing, not a delay as such; a caller that can
    //! reach them - a file player can, a live input cannot - satisfies the
    //! model by feeding them early instead of by holding the output back:
    //!
    //!     while (ch.available() < n) { read n more from the source; ch.push(...); }
    //!     ch.pull(out, n, stride);
    //!
    //! No arithmetic about `latency` is needed, because available() already
    //! knows, and an underrun cannot happen because the pull only runs once
    //! available() says it can be satisfied. Measured against this core run
    //! over a whole file in one go, the output is then bit-identical and output
    //! frame i is input frame i - no delay at all - at n of 64, 128, 256, 512,
    //! 600, 1024 and 2048. It costs latency/n extra reads on the first block
    //! and one per block after that, which leaves the source between `pad` and
    //! `pad + kBlock` ahead of what is being heard.
    //!
    //! Two things the caller owes in return. The source ends up ahead, so
    //! anything derived from its read position - elapsed time, end of stream -
    //! runs early by that much and has to be offset; otherwise the last samples
    //! are still in the pipeline when the stream is declared over, and the tail
    //! is lost. And being ahead is a state to maintain, not something to do
    //! once at the start: feed `latency` up front and then go back to n in,
    //! n out, and the head start decays by a block per call until it arrives at
    //! a 512-sample delay, by way of one dropout where it runs out.
    //!
    //! The zeros are marked as not audio - before they go in, since the last of
    //! them completes the first block - so no repair reaches back into them and
    //! they stay out of the noise estimate: the stream's first `latency`
    //! samples are exactly silent, and the first real ones are judged against
    //! real noise rather than against silence.
    void prime() { m_silentUntil = m_fill + m_cfg.latency; drain(); }

    const Config & config() const { return m_cfg; }

    //! Diagnostics: how many samples this channel has repaired.
    uint64_t repairedSamples() const { return m_repaired; }
    uint64_t seenSamples() const { return m_seen; }

    //! Diagnostics: how many repaired samples had their estimate clamped to
    //! the local range in repairRun(), on first repairs and on re-repairs. A
    //! clamped estimate means the solve went wrong; on clean records it
    //! should stay near zero. A re-repair counts every time it is done.
    uint64_t clampedSamples() const { return m_clamped; }
    uint64_t reclampedSamples() const { return m_reclamped; }

    //! Diagnostics: total bytes this channel is holding. Nothing in the
    //! processing path adds to this - see the buffer envelope in Config.
    size_t heapBytes() const;

private:
    void pushOne(double v);
    void outPush(double v);
    //! The dry/wet (or delta) mix of one finished sample, into the output ring.
    void emitMixed(double y, double d);
    //! One repaired sample and its dry counterpart, into the body stage.
    void bodyPush(double y, double d);
    //! The frame starting at stage sample `start` is complete: transform it,
    //! and cut the one two hops before it.
    void bodyHop(uint64_t start);
    //! The key's strength, 0..1, from the strong-run rates of the last
    //! Config::bodyHold seconds, as of the latest repair block.
    double bodyKey() const;
    //! The key as the stage applies it: the strongest of the last
    //! Config::bodyRelease seconds of bodyKey().
    double bodyBeta() const;
    //! The fine stage's frame starting at stage sample `start` is complete:
    //! transform it, and cut the one fineM hops before it. See Config::fine.
    void fineHop(uint64_t start);
    void growOut();               //!< only reachable by overrunning maxBlock
    void processBlock();          //!< repairs m_win[m_pad .. m_pad+kBlock)
    void detect(int from, int to, int pass);
    //! The median of the noise ring's first `have` entries; see the .cpp.
    double ringMedian(size_t have);
    //! Repairs bursts that start in [from, to); a burst may run on to `reach`.
    void interpolate(int from, int to, int reach);
    //! Repairs m_win[s..e] with the current model. False if it could not.
    //! `again` for a re-repair: the click is taken from m_dry, and the
    //! repair is not counted a second time. `gain` scales what is taken out
    //! of m_win; m_full takes all of it. See Config::softMin.
    bool repairRun(int s, int e, bool again, double gain);
    //! Repairs every run recorded this block again, with the current model.
    void rerepairRuns();
    void fitModel(int from, int to);
    //! The fit's autocorrelation from the last one's; see the .cpp.
    bool updateAutocorr(int n, int order);
    //! a[0] = 1, rest zero: pass the audio through unmodelled. Keeps m_arev in
    //! step, which is why the degenerate paths in fitModel() all come here.
    void setUnitModel();

    Config m_cfg;

    //! The window and the two residuals are what the filters stream through,
    //! and m_dry is read alongside m_win; see CacheAlignedAlloc.
#if defined(__APPLE__)
    typedef std::vector<double> AlignedBuf;
#else
    typedef std::vector<double, CacheAlignedAlloc<double> > AlignedBuf;
#endif

    AlignedBuf m_win;             //!< sliding window of input/repaired audio
    std::vector<uint8_t> m_flag;  //!< parallel to m_win: repair mask
    AlignedBuf m_dry;             //!< untouched copy, for the dry/wet mix
    //! m_win with every repair in full, whatever its Config::softMin gain:
    //! what repairRun() solves against. Solved against m_win, a run's context
    //! held its neighbours' half clicks. After the first pass the refit has
    //! the clicks out and whitens harder, and through it those half clicks
    //! pushed a re-repair out of the local range, to the clamp, which was the
    //! context of the next. Through a cluster of weak runs the estimates went
    //! 0.066, 0.10, 0.18, 0.71, 1.0 against limits of 0.053-0.066, and the
    //! gaps between the runs then read up to 440 sigma and were repaired in
    //! full, at the clamp: a loud burst on Tanturi's Voz de tango at 14.37 s
    //! after Dehum, and smaller ones elsewhere. Equal to m_win wherever every
    //! gain is 1, so the hard repair is unchanged bit for bit.
    AlignedBuf m_full;
    int    m_fill = 0;            //!< samples currently held in m_win
    int    m_silentUntil = 0;     //!< m_win below this is prime()'s zeros
    int    m_pad = 0;

    AlignedBuf m_fwd, m_bwd;
    std::vector<double> m_det;
    std::vector<double> m_a, m_ra;
    //! m_a reversed. The forward residual and the interpolation residual both
    //! want sum_k a[k]*x[i-k], which walks one array backwards; holding the
    //! coefficients the other way round turns both into plain inner products.
    std::vector<double> m_arev;
    //! Autocorrelation of m_a, ridged: the first column of every repair's
    //! normal equations. It depends on the model alone, so it is built by the
    //! first repair after each fit and shared by the rest - and blocks with
    //! nothing to repair never build it at all.
    std::vector<double> m_aa;
    std::vector<double> m_aarev;     //!< m_aa reversed, for repairRun()
    bool   m_aaValid = false;
    //! Hann window for the model fit, precomputed. It depends only on the fit
    //! length, which is fixed once configured, so computing it per block cost a
    //! cos() per sample - about nine per output sample at high orders.
    std::vector<double> m_hann;
    int    m_hannLen = 0;
    std::vector<double> m_scratch;   //!< model fitting / median
    //! The last fit's tapered window, behind m_fitLead zeros, and its
    //! autocorrelation before the ridge: what the next pass over the same
    //! window updates instead of recomputing. m_fitLen is its length, 0 once
    //! the window has moved on.
    std::vector<double> m_fitWin;
    std::vector<double> m_fitR;
    std::vector<double> m_delta;     //!< new taper minus m_fitWin
    std::vector<double> m_corr;      //!< updateAutocorr()'s two filters
    int    m_fitLen = 0;
    int    m_fitLead = 0;
    std::vector<double> m_rhs;       //!< interpolation right hand side
    std::vector<double> m_solve;     //!< Cholesky factor
    std::vector<double> m_sinv;      //!< banded slice of G^-1
    std::vector<double> m_pvar;      //!< diag(G^-1), per damaged sample
    //! Runs repaired in the current block, as (start, end) pairs, for
    //! Config::rerepair. Runs are disjoint and start inside the block, so
    //! there can be no more than kBlock of them.
    std::vector<int> m_runs;
    std::vector<double> m_runGain;   //!< each run's repair gain, one per pair
    int    m_nruns = 0;

    //! The body stage; see Config::body. Rings of repaired (m_bY) and dry
    //! (m_bD) audio and of what it removes from them (m_bR), indexed by the
    //! stage's sample count m_bT. All sized in configure() from bodyN, which
    //! depends on the sample rate alone.
    enum { kBodyFrames = 5 };     //!< the cut frame, two either side
    int    m_bN = 0, m_bH = 0;
    size_t m_bMask = 0;           //!< ring size - 1
    uint64_t m_bT = 0;            //!< samples into the stage
    //! m_bT + 1 at the next hop. A countdown rather than a modulo of m_bT:
    //! a 64-bit divide every sample was a third of the stage's cost on
    //! Haswell with the key shut.
    uint64_t m_bNextHop = 0;
    int    m_bK0 = 0, m_bK1 = 0;  //!< the band, as bins
    int    m_bM0 = 0, m_bM1 = 0;  //!< bins whose magnitudes are kept
    int    m_bHalf = 0;           //!< half-width of the median over frequency
    int    m_bHist = 0;           //!< consecutive frames transformed, to 5
    int    m_bSlot = 0;           //!< where the next frame goes in the rings
    double m_bNorm = 1.0;         //!< sum of the squared window over the hops
    std::vector<double> m_bY, m_bD, m_bR;
    std::vector<double> m_bWin;            //!< periodic Hann
    std::vector<double> m_bRe, m_bIm;      //!< FFT work, N/2 points
    std::vector<double> m_bTwRe, m_bTwIm;  //!< N's twiddles, for unpacking
    std::vector<double> m_bStRe, m_bStIm;  //!< N/2's, stage by stage
    std::vector<int>    m_bRev;            //!< bit reversal of N/2
    std::vector<double> m_bMag;            //!< kBodyFrames x (m_bM1-m_bM0+1)
    std::vector<double> m_bXRe, m_bXIm;    //!< kBodyFrames x (m_bK1-m_bK0+1)
    //! The median's window, kept sorted, with one place of pad either side
    //! for sortedReplace()'s vector paths.
    std::vector<double> m_bMed;
    //! Strong runs per repair block, and the rate over bodyRateWin, as rings.
    std::vector<int>    m_bCount;
    std::vector<double> m_bRate;
    std::vector<double> m_bKey;   //!< bodyKey() per block, over bodyRelease
    int    m_bCountPos = 0, m_bRatePos = 0, m_bRateHave = 0, m_bStrong = 0;
    int    m_bKeyPos = 0;

    //! The fine stage; see Config::fine. It reads the body stage's ring of
    //! repaired audio and adds what it removes to m_bR. Sized in configure()
    //! from fineN and fineM.
    int    m_fN = 0, m_fH = 0;
    int    m_fK0 = 0, m_fNb = 0;  //!< the band: m_fNb bins from m_fK0
    int    m_fNbLow = 0;          //!< of them, the first m_fNbLow are below fineMid
    //! The voice guard's window, +-m_fW frames (24 ms), and its lags.
    int    m_fW = 0, m_fLmin = 0, m_fLmax = 0;
    //! How far the cut frame trails the newest: the larger of fineM, for the
    //! quantile after it, and m_fW, for the guard's window.
    int    m_fD = 0;
    //! Frames held: back to the cut frame and the one that has just left the
    //! latest quantile window, whose value has to be taken out of it.
    int    m_fSlots = 0;
    int    m_fQSlots = 0;         //!< rows of m_fQ: windows ending m_fD + 1 back to the newest
    int    m_fESlots = 0;         //!< the guard's envelope, back to the cut frame's window
    int    m_fHist = 0;           //!< consecutive frames transformed, to 2 m_fD + 1
    uint64_t m_fFrame = 0;        //!< frames transformed; frame f is in slot f % m_fSlots
    uint64_t m_fNextHop = 0;      //!< as m_bNextHop
    double m_fNorm = 1.0;         //!< sum of the squared window over the hops
    std::vector<double> m_fWin;            //!< square-root periodic Hann
    std::vector<double> m_fRe, m_fIm;      //!< FFT work, N/2 points
    std::vector<double> m_fStRe, m_fStIm;  //!< N/2's twiddles, stage by stage
    std::vector<double> m_fTwRe, m_fTwIm;  //!< N's, for the packing, k <= N/2
    std::vector<int>    m_fRev;            //!< bit reversal of N/2
    std::vector<double> m_fMag;            //!< m_fSlots x m_fNb
    std::vector<double> m_fXRe, m_fXIm;    //!< m_fSlots x m_fNb
    //! Per bin, the magnitudes of the last fineM frames, kept sorted: a hop
    //! moves one value out and one in.
    std::vector<double> m_fSorted;   //!< m_fNb x fineM, a place of pad either side
    //! The fineQ quantile of each window of fineM frames, by the frame that
    //! ends it: frame f in row f % m_fQSlots. The windows before and after
    //! the cut frame are two of these, fineM + 1 frames apart, so each window
    //! is ranked once rather than twice.
    std::vector<double> m_fQ;        //!< m_fQSlots x m_fNb
    std::vector<double> m_fE;        //!< the guard's envelope per frame, m_fESlots
    std::vector<double> m_fSeg;      //!< 2 m_fW + 1 of it, around the cut frame
    //! bodyBeta() as of the latest repair block. It only changes there, and
    //! the fine stage, hopping every N/4 samples, cannot afford to rescan
    //! Config::bodyRelease seconds of key for it each time.
    double m_bBeta = 0.0;

    std::vector<double> m_madRing; //!< |forward residual| history
    int    m_madPos = 0;
    bool   m_madFull = false;
    double m_scale = 1e-6;
    double m_medHint = 0.0;        //!< last ringMedian(), where to look next

    //! Finished samples, as a fixed-capacity ring - m_out.size() IS the
    //! capacity, so reset() must not clear() it. It used to be a vector that
    //! grew by push_back to 65536 samples and was then compacted with erase(),
    //! which meant a 720 kB reallocation a second or so into every stream, on
    //! the audio thread.
    std::vector<double> m_out;
    size_t m_outHead = 0;          //!< read position
    size_t m_outCount = 0;         //!< samples held

    uint64_t m_repaired = 0, m_seen = 0;
    uint64_t m_clamped = 0, m_reclamped = 0;
};

//! The inner-product kernels this process runs: "avx2" where the CPU and OS
//! support AVX2 and FMA, chosen when the module loads, otherwise "sse2";
//! "neon" on every AArch64 CPU; "scalar" anywhere else. Wide and baseline
//! differ in the last bits, so output is bit-reproducible per kernel, not
//! across machines.
const char * kernelName();

//! For tests and benchmarks: false pins the baseline kernels even where AVX2
//! or NEON is available, true goes back to choosing. Process-wide and not
//! thread-safe - call it before any processing starts.
void allowWideKernels(bool allow);

//! Levinson-Durbin. `a` receives order+1 coefficients with a[0] == 1.
//! Returns false if the autocorrelation is degenerate.
bool levinson(const double * r, int order, double * a);

//! Banded symmetric Toeplitz Cholesky solve, half-bandwidth `band`.
//! `d` holds the Toeplitz first column (band+1 entries). Solves G x = b in
//! place on `b`. Returns false if not positive definite. `scratch` is left
//! holding the Cholesky factor, for bandedInverseDiagonal() below.
bool solveBandedToeplitz(const double * d, int band, double * b, int n,
                         std::vector<double> & scratch);

//! Diagonal of G^-1, from the factor solveBandedToeplitz() left behind.
//! Pass the same `band` and `n`; `chol` is that call's scratch vector and
//! `work` is separate scratch of the caller's. Writes n entries to `diag`.
//!
//! Uses Takahashi's recursion, which computes the banded slice of the inverse
//! in O(n * band^2) rather than inverting outright: filling from the bottom
//! right, every term needed is either already written or the symmetric
//! counterpart of one that is.
bool bandedInverseDiagonal(const std::vector<double> & chol, int band, int n,
                           std::vector<double> & work, double * diag);

} // namespace declick

#endif // DECLICK_CORE_H
