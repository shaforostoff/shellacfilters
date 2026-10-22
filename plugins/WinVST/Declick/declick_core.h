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
    kBlock    = 512,     //!< analysis hop; also sets most of the latency
    kCarpetMax = 64      //!< seconds the carpet key can look back; see Config::carpet
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
    //! D'Arienzo transfers come out to the bit as without it - and by the
    //! carpet key, which opens it on a steady carpet of crackle where the body
    //! key stays shut (see Config::carpet). Its delay, a
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

    //! The micro stage. Not user-facing; declick_cli sweeps it as --micro,
    //! --micro-ms, --micro-alpha and --micro-q.
    //! On a dense carpet the crackle's micro-clicks come closer together than
    //! the fine stage's 2.9 ms frame: on five Calo .m4a, measured against
    //! vinyl transfers of the same records warped onto them, those above 8
    //! times their floor came 350-500 a second, a median 1.5-1.75 ms apart,
    //! 67-80% of the gaps shorter than the frame. The fine stage's background
    //! is then made of clicks, and in passages with no voice the output kept
    //! 53-86 such events a second where the decrackled version kept 15-26.
    //! So the fine stage's clip runs again on its output, at a quarter of its
    //! frame (~0.73 ms, hop N/2), with a background of the louder of the
    //! microQ quantiles over microMs either side, over the fine stage's band
    //! and under its key, floor and voice guard: below fineMid it backs off as
    //! far as the fine stage's guard did for the frame nearest. It reads the
    //! fine stage's output once that is final, so its delay is the fine
    //! stage's plus microM hops and a frame, and that fits inside the body
    //! stage's: it adds no latency. It needs the fine stage, and is off when
    //! that is. Against the vinyl, with the wide pad below, 7-10 kHz came
    //! 0.3-1.7 dB closer and the strong events left fell by 7-50%; nothing
    //! moved below 3 kHz. A variant with a guard that read the voice's period
    //! from the 0.2-3.5 kHz band was liked more on those five, but took 1.25-2
    //! times a decrackled version's voice; this one, with the fine stage's own
    //! guard, was chosen as the safer.
    int    micro        = 1;
    double microMs      = 6.0;
    double microAlpha   = 2.0;
    double microQ       = 0.2;
    int    microN       = 32;        //!< fineN / 4, from the sample rate alone
    int    microM       = 17;        //!< frames either side, from microMs and the rate

    //! The mid stage. Not user-facing; declick_cli sweeps it as --mid,
    //! --mid-lo, --mid-peak, --mid-p0, --mid-p1 and --mid-yield.
    //! A shellac click's 2-4.5 kHz part outlasts its run. On four Pugliese
    //! .m4a, measured against vinyl copies of the same records warped onto
    //! them, 96-98% of the 2-4.5 kHz click impulses had a repaired run within
    //! 0.5 ms, but the run was the onset, a median 5 samples, while the band
    //! stayed above its level 3 ms away for +-0.6-0.9 ms around it: the AR
    //! model predicts that ringing, so it is never flagged, and the repair
    //! left 56-85% of its energy, where the decrackled version left 4-35%.
    //! Interpolating a wider span took 1.5-11% of the music at 2-3 kHz; the
    //! fine stage's clip moved down to the band with no guard took 21-59% of
    //! the record's own 2-4.5 kHz events.
    //! What tells the two apart in the .m4a alone is how the band's envelope
    //! repeats: by the fine guard's statistic on this band (+-24 ms, lags of
    //! 2.5-12 ms), 0.21-0.24 at the clicks, 0.81-0.83 at the record's own
    //! events. So the fine stage's frames also take the bins from midLo up to
    //! fineLo, clipped as its own are - fineAlpha times the louder of the
    //! fineQ quantiles either side, floor fineFloor, under its key - but only
    //! in frames that hold, or neighbour, a repaired run that peaked at
    //! midPeak sigma or more, and backing off as this band's envelope
    //! repeats, from midPeriodic0 (none) to midPeriodic1 (all). In the body
    //! stage's band, where the body key is open, it yields to that stage by
    //! the key (midYield): the two read the same audio, and their cuts would
    //! add on one click. No latency added.
    //! Against the vinyl it took a median 30-72% of the click impulses' energy
    //! where the stages before took 15-44%, and 1-5% at the record's own
    //! events, as before; 0.3-1.7% of the music at 2-3 kHz (before 0.2-0.9%,
    //! the decrackled version 0.4-2.3%). Heard as right against a stronger guard
    //! (0.4-0.6) and a lower key; the Malvon master, Olga, El Marne and 51 of
    //! a 98-song sample are untouched, as their keys never open.
    int    mid          = 1;
    double midLo        = 2000.0;
    double midPeak      = 4.0;       //!< sigma
    double midPeriodic0 = 0.3;
    double midPeriodic1 = 0.5;
    int    midYield     = 1;

    //! The flank repair. Not user-facing; declick_cli sweeps it as --flank,
    //! --flank-pad-ms, --flank-hz, --flank-edge0 and --flank-edge1.
    //! On a transfer band-limited near 11 kHz - the .m4a copies of D'Arienzo's
    //! 1940s sides fall 40 dB by 11.7-14.4 kHz, their .flac copies by 21 kHz -
    //! a click is a 3-4 sample spike that rings for +-10-15 samples, and that
    //! one shape runs through all four songs. A run ends inside the ringing,
    //! the interpolation is pinned to flanks that are still click, and it
    //! rebuilds part of it: measured against the .flac copy, warped onto the
    //! .m4a to a fraction of a sample, the detector found 99-100% of the
    //! crackle above 4.5 kHz and the repairs left 36-55% of its energy. A full
    //! repair of every weak run changed that by a point.
    //! So once a block is repaired, each of its runs is widened by flankPad
    //! samples either side, into the samples between it and its neighbours but
    //! never into one, and interpolated again against every repair in full, at
    //! its own gain. Only the high-passed (flankHz) difference from the output
    //! is kept, so below that the audio is exactly what the repair made of it.
    //! The difference goes into m_flAcc, which is added as each block leaves
    //! for the body stage, a block later than it would have: room for the next
    //! block's flanks and the filter's spill. On *La madrugada* (.m4a) the
    //! crackle events left above 4.5 kHz go 36 -> 22%, the decrackled
    //! version's 21%, and 3-7 kHz comes as close to the clean copy as that
    //! version or closer; below 3 kHz nothing moves by more than 0.1 dB.
    //! It acts only as far as the transfer is band-limited: the pad is full
    //! where the long-term spectrum falls 40 dB under its 2-5 kHz level by
    //! flankEdge0, and gone by flankEdge1. A full-band record gets none, so the
    //! Malvon master, *No esta*, *Va a cantar un ruisenor* and *Cornetin* come
    //! out to the bit as without it, with flankStage more latency. Off below
    //! 32 kHz, where a low sample rate cannot be told from a band-limited
    //! transfer.
    //! And it acts where the body stage's key is open, then fades out over
    //! flankHold seconds once it shuts. The key counts runs at 6 sigma, and a
    //! dense carpet inflates sigma until few reach it: on *Amarras* (.m4a) it
    //! shuts over 2:04-2:08, where the .m4a stands furthest above the clean
    //! copy, and the repair stopped there. Over 12 s the fade covers that and
    //! brings it 0.6-1.3 dB closer, and the events left over the song go
    //! 40 -> 36% and 69 -> 61%. A record whose key never opens is untouched by
    //! it, which a softer repair everywhere was not: at half strength it took
    //! -20 to -27 dB of 3-10 kHz from nearly every band-limited 1950s
    //! remaster in a sample of 98. 12 s was heard as right on *Amarras*, and
    //! as taking no music from two remasters it reaches further on.
    //! Its strength is also at least the carpet key's; see Config::carpet.
    int    flank        = 1;
    double flankPadMs   = 0.18;      //!< 8 samples at 44.1 kHz
    double flankHz      = 3000.0;
    double flankEdge0   = 13000.0;
    double flankEdge1   = 15000.0;
    double flankHold    = 12.0;      //!< seconds
    //! Where the key is shut, past the hold: the strength left there, on a
    //! band-limited transfer only. 0: see flankHold for what it costs.
    double flankShut    = 0.0;
    //! ... and there only for runs repaired with at least this gain (see
    //! Config::softMin). The gain there comes from the weak runs, the carpet:
    //! strong runs alone did nothing for *Amarras*.
    double flankShutGain = 0.0;
    //! Where the key is open: the share of the pad a full-band transfer gets.
    //! It was 0 because on *No esta*, *Ruisenor* and *Cornetin* what it took
    //! correlated as much with what the decrackled version keeps as with what
    //! it removes. Against vinyl transfers of five Calo .m4a, warped onto
    //! them, the full pad brought the three whose body key opens 0.1-0.3 dB
    //! closer at 3-4.5 kHz and 0.2-0.6 dB at 4.5-7 kHz, and their quietest
    //! third's 3-4.5 kHz crackle left from 56-64% to 42-52%, for 1.7-3.1% of
    //! the music at 3-4.5 kHz, where the decrackled version takes 1.3-2.9%.
    //! See Config::micro, which it was heard with.
    double flankWide    = 1.0;
    int    flankPad     = 8;         //!< from flankPadMs and the rate
    int    flankHalf    = 63;        //!< the high-pass's half length, from the rate
    //! Samples the repaired audio waits before the body stage, for the flank
    //! repair: a block, or 0 when it is off. Part of the latency.
    int    flankStage   = 0;

    //! The carpet key. Not user-facing; declick_cli sweeps it as --carpet,
    //! --carpet-peak, --carpet-win, --carpet-q, --carpet-rate0, --carpet-rate1,
    //! --carpet-warm and --carpet-min.
    //! The body key counts runs that peak at bodyPeak sigma, and on a crackly
    //! transfer that count sits right at its ramp: dense crackle inflates
    //! sigma, so fewer of its clicks reach 6 sigma just where it is densest,
    //! and the key flickers or stays shut. *Yo quiero cantar un tango* (.m4a)
    //! had it open 3% of the song, *Si supiera* 36%, *Viejo ciego* 34%, and
    //! where it is shut the fine stage and the flank repair do nothing.
    //! Measured against a clean copy of the record, the crackle there was not
    //! denser from one second to the next: on *Amarras* 1:48-2:21 the detector
    //! repaired 1200-1500 runs a second throughout, ~300 of them at 4 sigma
    //! or more, and the worst seconds were where the music dropped by 8 dB.
    //! What tells a crackly transfer from a clean one is the record: its
    //! surface keeps a carpet of runs going through the quiet passages, where
    //! on a clean record runs come and go with the music.
    //! So in every repair block this counts the runs that peak at carpetPeak
    //! sigma, where the body key counts its own, and takes their rate per
    //! 44.1k samples over each second. The carpetQ quantile of the last
    //! carpetWin seconds of those rates is the key's value, ramped in from
    //! carpetRate0 to carpetRate1. It may open once carpetMin seconds are in,
    //! and while the ring fills both thresholds stand up to carpetWarm higher:
    //! a loud opening of a few seconds is little evidence. Crackly transfers
    //! read 100-350 here; the clean band-limited 1958-75 remasters of a
    //! 98-song sample read up to 77, *El Marne* 108 after its opening (120 on
    //! its first seconds), the Malvon master 66.
    //! It opens the fine stage and the flank repair, never the body stage: the
    //! fine stage cuts by the larger of this key and the body key's, and the
    //! flank repair's strength is at least this key. Against clean copies of
    //! seven songs - four D'Arienzo .m4a against their .flac copies, three Calo
    //! .m4a against vinyl transfers, each warped onto the .m4a - the output
    //! below 3 kHz is the same as without it and above 3 kHz closer on all
    //! seven: on *Yo quiero* the crackle events left at 4.5-10 kHz go from
    //! 56/86% to 37/65%. Opening the body stage as well moved six of them
    //! 1.1-3.1 dB further from the clean copy at 0.5-1 kHz, which is music.
    //! The Malvon master, *Olga*, *El Marne* and 44 more clean records come
    //! out to the bit as without it. Heard as helping on *Homero* and *Si
    //! supiera*.
    int    carpet       = 1;
    double carpetPeak   = 4.0;       //!< sigma
    int    carpetWin    = 20;        //!< seconds, 1 .. kCarpetMax
    double carpetQ      = 0.2;
    double carpetRate0  = 110.0;     //!< runs per second, per 44.1k samples
    double carpetRate1  = 150.0;
    double carpetWarm   = 0.5;       //!< thresholds x (1 + this x the window's empty share)
    int    carpetMin    = 3;         //!< seconds, 1 .. carpetWin
    //! The key's first seconds. Its first rate needs a whole second, and it
    //! may not open before carpetMin, so on a crackly record the stages it
    //! opens stayed off for three seconds, and those were heard as noisier
    //! than the rest (*La vida me engano*: 25 and 13 impulses a second left at
    //! 7-10 kHz in its first two seconds, 0-2 after). So from carpetFirst
    //! seconds until carpetMin, the key follows the rate of runs at
    //! carpetPeak sigma over everything seen so far, against the thresholds
    //! raised by the whole of carpetWarm: 25 and 13 became 4 and 0. On a
    //! 98-song sample 51 songs change, only in their first three seconds and
    //! by -46 to -116 dB, mostly the run-in at the needle drop; the Malvon
    //! master and Olga are untouched. 0 turns it off.
    double carpetFirst  = 0.25;      //!< seconds
    //! The Poisson key, a second ramp for the carpet key. Not user-facing;
    //! declick_cli sweeps it as --carpet-fano, --fano-rate0, --fano-rate1,
    //! --fano0 and --fano1.
    //! On Pugliese's *El rodeo* (.m4a, 1943) the carpet count reads ~104 at
    //! its median, under the 110-150 ramp, and the stages the key opens were
    //! off at 40-90 s and from 120 s on. No threshold on the count separates
    //! it from clean records: *El Marne* reaches 108 after its opening. How the
    //! runs arrive does. A surface's crackle is a Poisson process, and a clean
    //! record's runs bunch on the music's attacks. So the runs at carpetPeak
    //! sigma are also counted in bins of ~11.6 ms (a repair block, or as many
    //! as make that at the rate), and each second's Fano factor (variance /
    //! mean of those counts) goes into a ring beside the rates. The key also
    //! opens as far as ramp(K, fanoRate0 .. fanoRate1) x ramp(median Fano,
    //! fano0 down to fano1), K the carpet value above, with the same warm-up;
    //! the carpet key is the larger of the two. Over the songs: the four
    //! Pugliese read 1.01-1.09, other crackly .m4a 1.0-1.1, *El Marne* 7.6,
    //! *Cachirulo* .flac 1.57, the Malvon master 1.61, *La tablada* .flac
    //! 1.33, and every song whose count is near *El rodeo*'s 1.5-6.
    //! *El rodeo*'s key goes from a mean of 0.19 to 0.72, open on 99% of the
    //! song. Of a 101-song check set 99 are bit-identical, among them the
    //! Malvon master, *Olga*, *El Marne* and *La tablada*; *Bien milonga* (1951,
    //! crackly) changes by -53 dB and *Cachirulo* .flac by -78 dB on 4% of its
    //! seconds. From fanoFirst until carpetMin it reads the counts so far, as
    //! the carpet key does from carpetFirst, so on *El rodeo* it is open from
    //! 0.5 s, not 3 s. Not from carpetFirst: over a quarter second the Fano
    //! factor is too rough, and *La tablada*'s .flac read 1.28 there, at the
    //! needle drop. 0 turns it off.
    int    carpetFano   = 1;
    double fanoRate0    = 80.0;      //!< runs per second, per 44.1k samples
    double fanoRate1    = 110.0;
    double fano0        = 1.4;       //!< Fano factor where it starts to open
    double fano1        = 1.2;       //!< ... and where it is fully open
    double fanoFirst    = 0.5;       //!< seconds
    int    fanoBlocks   = 1;         //!< repair blocks per bin, from the rate

    //! The crackle floor stage (named `denoise` in b525d0a7f). On by default
    //! since its mid-song openings were fixed, with no control of its own in
    //! the plug-ins. declick_cli turns it off with --crackle-floor 0 and sweeps
    //! --crackle-floor-lo, -hi, -beta, -min, -q, -win and -release, and the
    //! shelf's --crackle-floor-shelf, -shelf-gain, -shelf-x0, -shelf-x1 and
    //! -shelf-release.
    //! On a dense carpet - 600-900 runs a second on Pugliese's *El rodeo* and
    //! *El abrojito* - the crackle merges into a floor, heard as sizzle and
    //! frying, and the click stages cut only what stands above it. Against
    //! vinyl copies of the two, *El abrojito*'s output already had no more
    //! clicks standing out than the copy, but its floor at 4.5-7 kHz was 5 dB
    //! over the copy's; *El rodeo*'s was 12-13 dB over at 2-7 kHz.
    //! So this is a spectral denoiser on the finished output, under the key the
    //! fine stage follows. An STFT of ~23 ms (hop N/4, Hann either side); per
    //! bin from crackleFloorLo up (ramped in over the half octave below), the
    //! noise floor is the crackleFloorQ quantile of the bin's power, smoothed
    //! over 5 frames, every fourth frame over the last crackleFloorWin seconds.
    //! The gain is Wiener's on the power averaged over 7 bins x 5 frames
    //! (~300 Hz x 29 ms) against crackleFloorBeta x that floor, never below
    //! crackleFloorMin, rising at once and falling back over
    //! crackleFloorRelease, then averaged over 3 bins: a stretch of pure noise
    //! gets one steady cut rather than bins that twinkle.
    //! The history leaves out the stream's first second (before 2 s, the first
    //! half of it): the TangoTunes transfers fade in over ~0.5 s from 30 dB
    //! under their crackle, and those frames held the quantile under the floor
    //! for seconds, so the stage started late. Where the key is shut nothing is
    //! cut. In the stream's first two seconds the history then waits, as it
    //! always did, so a record's start is cut as before; from then on it goes
    //! on under the shut key too, a quarter as often (each value standing for
    //! four), so the window is the last crackleFloorWin seconds whatever the
    //! key did.
    //! Heard as the prototype's "A", which looked ahead and behind +-3 s for the
    //! floor; this looks behind only, and takes 0.7 dB more. It brought
    //! *El rodeo*'s quiet floor from 12.2 to 9.6 dB over the copy at 2-3 kHz,
    //! for 1.1-1.9% of *El abrojito*'s music at 1-4.5 kHz (before 0.4-1.2%).
    //! Over the whole band it was heard as removing crackle but leaving
    //! high-frequency noise that feels unnatural. The reason is that above
    //! ~5 kHz these records' music lies ~15 dB under the crackle, so the cut
    //! there follows the crackle itself. At 6-10 kHz it opened 0.5 dB per dB
    //! of the crackle's level on *El abrojito* (0.3 on *El rodeo*) and not
    //! for the music, against the vinyl copies. It let the crackle's swells
    //! through and pushed the bed between them down, which stretched the
    //! band's level swings 1.3-1.5 times, as a 2-8 Hz wobble in 300 Hz
    //! patches. Wiener's law alone does it to steady noise.
    //! So the per-bin cut stops at crackleFloorHi: full up to
    //! crackleFloorHi / sqrt 2, none from crackleFloorHi up, and the top is
    //! left as it was. Of the four remedies heard, that was liked best.
    //! crackleFloorHi <= 0 cuts to Nyquist as before.
    //! The history used to wait wherever the key was shut, so where the key
    //! opened again mid-song the first-second rule threw out what it held,
    //! and the window was a few frames of the music itself: *Y_ahora*'s
    //! opens at 82 s on loud music, and for ~3 s the stage took up to
    //! 0.8-1.5 dB of 1-3 kHz, following the music's spectrum. Now the history
    //! goes on under a shut key, and the first-second rule leaves out only
    //! what was taken in the first two seconds. On its own that took more
    //! than before where the old history had kept a lower floor from the last
    //! opening: a floor taken under the open key is the crackle's, one taken
    //! while it was shut may be the music's. So after the key opens again,
    //! until the window has been renewed under it, the floor is no higher
    //! than it stood when the key last shut (not in the first two seconds,
    //! which are cut as before). On the 109-song check set the stage then
    //! takes no more than before from any song, over the whole song.
    //! The floor is followed only as far up as the cut needs (~0.4 MB a
    //! channel at 44.1 kHz rather than ~2.2 MB). The stage adds
    //! crackleFloorLatency. Where its key is open it costs 20-30% more than
    //! the rest of the core (the full band 80-95%); where it is shut, 1-3%
    //! more than when the history waited there.
    int    crackleFloor        = 1;
    double crackleFloorLo      = 1000.0;
    double crackleFloorHi      = 4500.0;  //!< Hz; <= 0 for no upper edge
    double crackleFloorBeta    = 1.2;
    double crackleFloorMin     = 0.32;    //!< about -10 dB
    double crackleFloorQ       = 0.1;
    double crackleFloorWin     = 6.0;     //!< seconds of history
    double crackleFloorRelease = 0.06;    //!< seconds
    int    crackleFloorN       = 1024;    //!< from the sample rate alone
    int    crackleFloorHist    = 259;     //!< floor values over crackleFloorWin
    int    crackleFloorLatency = 0;       //!< bodyDelay(crackleFloorN), if on

    //! The top shelf: off, heard and not chosen (the top untouched was liked
    //! better), kept as an option. Above crackleFloorHi (crossfading over the
    //! same half octave) every bin gets one gain, so nothing moves in patches.
    //! The gain is crackleFloorShelfGain while the 4-11 kHz band as a whole
    //! sits near its floor (the sum of its power over the sum of its floors),
    //! and it opens to 1 from crackleFloorShelfX0 to crackleFloorShelfX1 times
    //! that, smoothstep in the log. The ratio is a median over 9 frames, so a
    //! crackle shorter than ~25 ms cannot open it. The 9 frames end two hops
    //! after the cut frame, where the prototype's were centred on it, so the
    //! shelf opens up to 12 ms late. It rises at once and falls back over
    //! crackleFloorShelfRelease.
    //! It takes the top's crackle down 1.5-2.4 dB with the music taken as
    //! A's. But it still opens a little with the crackle (0.08-0.17 dB per
    //! dB, the level swings 1.08-1.17 times), as anything driven by the top
    //! band's own level does; only a steady cut does not, and that is a
    //! treble cut (-4 dB took 29-39% of the 4.5-7 kHz music). Needs
    //! crackleFloorHi > 0; it follows the floor up to 11 kHz.
    int    crackleFloorShelf        = 0;
    double crackleFloorShelfGain    = 0.63;   //!< about -4 dB
    double crackleFloorShelfX0      = 5.0;
    double crackleFloorShelfX1      = 40.0;
    double crackleFloorShelfRelease = 0.15;   //!< seconds

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

    //! The micro stage reads the fine stage's output this many samples after
    //! the stage takes a sample in: when the fine removal of it is final.
    int microLag() const { return fineDelay(fineN) * (fineN / 4) + fineN - 1; }

    //! Seconds of the coming audio a host that reads ahead gives
    //! Channel::beginWarm(): with the crackle floor stage on, its window and
    //! the first second it leaves out; else a second more than the carpet
    //! key's carpetMin, so that it reads its ring rather than its early value.
    double warmSeconds() const;

    //! True if `o` needs exactly the buffers this config already has, i.e. it
    //! can be swapped into a running Channel with retune() instead of
    //! configure(), which reallocates and therefore resets.
    //!
    //! Only `order`, `maxRun`, `madWindow`, `body`, `bodyN`, `fineN`, `fineM`,
    //! `microN`, `microM`, `mid`, `midLo`, `flankStage`, `flankHalf` and the
    //! crackle floor stage's `crackleFloor`, `crackleFloorN`, `crackleFloorLo`,
    //! `crackleFloorHi`, `crackleFloorShelf` and `crackleFloorHist` size
    //! anything or set the latency (`pad` and `latency` are derived from them);
    //! thresholds, depth, wet, delta, passes and whether the fine and micro
    //! stages run are read per block and can change under a live stream.
    bool structurallyEquals(const Config & o) const {
        return order == o.order && maxRun == o.maxRun && madWindow == o.madWindow
            && body == o.body && bodyN == o.bodyN && fineN == o.fineN
            && fineM == o.fineM && microN == o.microN && microM == o.microM
            && mid == o.mid && midLo == o.midLo
            && flankStage == o.flankStage
            && flankHalf == o.flankHalf && flankHz == o.flankHz
            && crackleFloor == o.crackleFloor && crackleFloorN == o.crackleFloorN
            && crackleFloorLo == o.crackleFloorLo && crackleFloorHi == o.crackleFloorHi
            && crackleFloorShelf == o.crackleFloorShelf && crackleFloorHist == o.crackleFloorHist && rate == o.rate;
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

class Channel;

//! What a warm pass keeps (see Channel::beginWarm()), held apart from the
//! channel that ran it: Channel::saveWarm() fills it, Channel::adoptWarm()
//! starts a channel from it as from the warmed channel itself. For a host
//! that goes on streaming the channel it warmed and still starts others warm
//! afterwards - VirtualDJ's, which catches that channel up with the play head
//! and swaps it in - without a third channel to hold the pass. A few kB: the
//! carpet key's two rings and the crackle floor ceiling.
class WarmState {
public:
    WarmState() {}
    //! Holds a pass, which adoptWarm() will take.
    bool valid() const { return m_valid; }
    void clear() { m_valid = false; }
    size_t heapBytes() const { return m_nWarm.capacity() * sizeof(double); }

private:
    friend class Channel;
    bool   m_valid = false;
    Config m_cfg;
    double m_cRuns = 0.0, m_cSamp = 0.0;
    double m_cTotRuns = 0.0, m_cTotSamp = 0.0;
    double m_cRing[kCarpetMax] = {};
    int    m_cPos = 0, m_cHave = 0;
    double m_cKey = 0.0;
    double m_cBin = 0.0;
    int    m_cBinBlk = 0;
    double m_cS1 = 0.0, m_cS2 = 0.0, m_cBinN = 0.0;
    double m_cTotS1 = 0.0, m_cTotS2 = 0.0, m_cTotBinN = 0.0;
    double m_cFRing[kCarpetMax] = {};
    uint64_t m_warmBlocks = 0;
    uint64_t m_nWarmT = 0;
    std::vector<double> m_nWarm;
};

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

    //! End of stream: run `latency` zeros through so the tail comes out. The
    //! zeros are not audio, as prime()'s are not: the stream's last samples
    //! are judged against the music before them, not against the silence
    //! after. Anything pushed afterwards starts as a stream does after prime().
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
    void prime() { m_silentUntil = m_fill + m_cfg.latency; feedZeros(); }

    //! WARM START, the same idea for the core's slow state. A host that can
    //! read the track ahead - foobar2000 and VirtualDJ can, a VST or an AU
    //! cannot - gives the core the stream's coming seconds before the stream
    //! itself, and the first seconds are then processed with the keys and the
    //! crackle floor the core has once those seconds are in, instead of with
    //! empty history:
    //!
    //!     ch.beginWarm();
    //!     ch.push(ahead, n, stride);   // Config::warmSeconds() of it; nothing comes out
    //!     ch.endWarm();
    //!     ...then push the stream from the same place, as usual.
    //!
    //! The warm pass is the ordinary pipeline with its output held back, so it
    //! costs what processing those seconds costs (less with the crackle floor
    //! stage off: it runs only the repair, which is all the key reads).
    //! endWarm() puts the audio path back to its opening state, as reset()
    //! does, and keeps what the pass learned:
    //!  - The carpet key, with its Poisson ramp. It stands still while the
    //!    stream goes back over the seconds the pass has seen - it already
    //!    counts them - and goes on from there exactly as it would have: for
    //!    t under the warm length it is what it is at that length, and past it
    //!    it is the cold stream's to the bit. On a clean record whose loud
    //!    opening used to open it for the first half second (*Jueves*, *Ahora
    //!    no me conoces*, *El Marne*), seven seconds' evidence keeps it shut;
    //!    on a crackly one it is open from the first sample.
    //!  - The crackle floor the pass ended with, as a ceiling. The stage's
    //!    history starts again as it always has, and until the stream is past
    //!    the warm length the floor is no higher than that one, as after a
    //!    reopening it is no higher than when the key shut. A loud start can no
    //!    longer make its own few frames the floor; a quiet one keeps the
    //!    floor it had, which is lower than one taken over the loud seconds
    //!    after it (*El abrojito*'s opening bars).
    //! The body key starts afresh: it reads half a second, and was never slow
    //! to open. So does the long-term spectrum the flank repair's pad reads,
    //! which has no pad for the first three seconds: kept, it brought the pad
    //! in from the first sample, and on clean band-limited records that let
    //! the repair act on the body key's opening at the needle drop: 3-25 dB
    //! more at 4.5-10 kHz in their first five seconds, on 15 of a 109-song set.
    //! On that set, in no second of the first seven did the warm start take
    //! 3 dB more than the cold stream, at 1-4.5 or 4.5-10 kHz, with the crackle
    //! floor stage on or off. Past the warm length the output is the cold
    //! stream's to the bit with the stage off; with it on, from a second past
    //! it, within -55 dB (its history takes every fourth frame, counted from a
    //! different start).
    //! Without these calls nothing changes. Neither allocates.
    void beginWarm();
    void endWarm();

    //! The warm start, run on another channel: reset(), then what `warmed`'s
    //! warm pass kept - the carpet key and the crackle floor ceiling - copied
    //! in. For a host whose audio thread cannot afford the warm pass
    //! (VirtualDJ's): it warms a channel of its own on a worker thread, and
    //! starts the stream on this one at each restart. From where the warm
    //! pass started, the stream then comes out as `warmed`'s own would, to
    //! the bit; from anywhere else, it starts there with the key and the
    //! ceiling of those seconds, standing still for as long as they did.
    //!
    //! `warmed` must have ended its warm pass and taken nothing since, with
    //! a config that structurallyEquals() this one's; otherwise this is
    //! reset() and returns false. `warmed` is not changed, and can be adopted
    //! from again. Does not allocate.
    bool adoptWarm(const Channel & warmed);

    //! What adoptWarm() would take from this channel, into `s`, so that this
    //! one can go on streaming and others still start warm from the pass
    //! afterwards. Same conditions as adoptWarm()'s `warmed`; otherwise `s`
    //! is cleared and this returns false. Allocates only when `s` has not held
    //! a state of this config's shape before: a host that must not saves once
    //! at configure(), from the freshly configured channel, and clears it.
    bool saveWarm(WarmState & s) const;

    //! adoptWarm() from a saved state: from where the pass started, to the
    //! bit what the warmed channel itself would have streamed. A state that
    //! is not valid() or of another shape is reset() and false. Does not
    //! allocate.
    bool adoptWarm(const WarmState & s);

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

    //! Diagnostics for the flank repair: spans interpolated again, spans
    //! passed over (longer than maxRun, or too near the window's edge), and
    //! the -40 dB edge of the long-term spectrum in Hz with the pad it gives
    //! now. See Config::flank.
    uint64_t flankSpans() const { return m_flSpans; }
    uint64_t flankSkipped() const { return m_flSkipped; }
    double bandEdge() const { return m_wEdge; }
    int flankPadNow() const { return m_flPOpen; }

    //! Diagnostics for the carpet key: repair blocks it was open in, and its
    //! value now, 0..1. See Config::carpet.
    uint64_t carpetOpenBlocks() const { return m_cOpen; }
    double carpetKey() const { return m_cKey; }

    //! Diagnostics for the micro stage: frames it took something from. See
    //! Config::micro.
    uint64_t microFrames() const { return m_mCuts; }

    //! Diagnostics for the mid stage: frames it took something from. See
    //! Config::mid.
    uint64_t midFrames() const { return m_dCuts; }

    //! Diagnostics for the crackle floor stage: frames it took something from.
    //! See Config::crackleFloor.
    uint64_t crackleFloorFrames() const { return m_nCuts; }

    //! Diagnostics: total bytes this channel is holding. Nothing in the
    //! processing path adds to this - see the buffer envelope in Config.
    size_t heapBytes() const;

private:
    void pushOne(double v);
    void outPush(double v);
    void feedZeros();             //!< `latency` zeros: prime()'s and drain()'s
    //! reset() in parts: the audio path and everything that restarts with it,
    //! and the state a warm start keeps (see beginWarm()).
    void resetPath();
    void resetCarpet();
    void resetFloorHistory();
    //! What endWarm() keeps, from one holder to another: a Channel or a
    //! WarmState either side, which name it alike.
    template <class To, class From> static void copyWarm(To & to, const From & from);
    //! The dry/wet (or delta) mix of one finished sample, into the output ring.
    void emitMixed(double y, double d);
    //! One repaired sample and its dry counterpart, into the body stage, and
    //! whether it lies in a run the mid stage counts.
    void bodyPush(double y, double d, uint8_t strong);
    //! The frame starting at stage sample `start` is complete: transform it,
    //! and cut the one two hops before it.
    void bodyHop(uint64_t start);
    //! The key's strength, 0..1, from the strong-run rates of the last
    //! Config::bodyHold seconds, as of the latest repair block.
    double bodyKey() const;
    //! The key as the stage applies it: the strongest of the last
    //! Config::bodyRelease seconds of bodyKey().
    double bodyBeta() const;
    //! The carpet key, once per repair block; see Config::carpet.
    void carpetBlock();
    //! The carpet ramp of a carpet value K, and the Poisson key's of K and a
    //! Fano factor F, with the thresholds raised by wf; see Config::carpetFano.
    double carpetRamp(double K, double wf) const;
    double fanoKey(double K, double F, double wf) const;
    //! The Fano factor of n bins whose counts sum to s1, their squares to s2.
    static double fanoOf(double s1, double s2, double n);
    //! A finished sample: into the crackle floor stage if it is on, else out.
    void finishSample(double y, double d);
    //! One finished sample and its dry counterpart, into the crackle floor
    //! stage; out crackleFloorLatency samples later. See Config::crackleFloor.
    void crackleFloorPush(double y, double d);
    //! The crackle floor stage's frame starting at its sample `start` is
    //! complete: transform it, and cut the one two hops before it.
    void crackleFloorHop(uint64_t start);
    //! The fine stage's frame starting at stage sample `start` is complete:
    //! transform it, and cut the one fineM hops before it. See Config::fine.
    void fineHop(uint64_t start);
    //! The micro stage's frame starting at stage sample `start` is complete
    //! (its fine removal is final): transform it, and cut the one microM hops
    //! before it. See Config::micro.
    void microHop(uint64_t start);
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
    //! The interpolation of m_win[s..e] from m_full around it, into m_rhs.
    //! The caller has checked that the span and its context fit the window.
    bool solveSpan(int s, int e);
    //! Repairs every run recorded this block again, with the current model.
    void rerepairRuns();
    //! The flank repair of this block's runs, into m_flAcc; see Config::flank.
    void flankPass();
    void flankSpan(int s, int e, double gain, double strength);
    //! The long-term spectrum of the input, every fourth block, and from it
    //! the -40 dB edge and the flank repair's pad.
    void bandwidthBlock();
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
    //! Parallel to m_win: in a run repaired at Config::midPeak sigma or more.
    std::vector<uint8_t> m_strong;
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
    //! m_win below this is not audio: reset()'s pad, or prime()'s zeros. From
    //! m_tailFrom on, while drain() runs, it is drain()'s; 1 << 30 otherwise.
    int    m_silentUntil = 0;
    int    m_tailFrom = 1 << 30;
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

    //! The flank repair; see Config::flank. Every run repaired this block, on
    //! any pass, as (start, end) pairs with its gain, whatever Config::rerepair
    //! says; and the high-passed corrections, aligned with m_win and slid with
    //! it, which the output adds as it leaves for the body stage.
    std::vector<int> m_flRun;
    std::vector<double> m_flGain;
    std::vector<int> m_flOrder;      //!< the runs in start order
    int    m_flN = 0;
    std::vector<double> m_flAcc;
    //! The high-pass, 2 flankHalf + 1 taps, twice: with kFlankPad zeros either
    //! side, -0.0 in m_flHpP and +0.0 in m_flHpN. See flankAddAvx2().
    std::vector<double> m_flHpP, m_flHpN;
    //! The last sample a flank span reached, in m_win's coordinates: the next
    //! block's spans start after it, so no sample is corrected twice.
    int    m_flEnd = 0;
    int    m_flP = 0;                //!< the pad where the key is shut: from the edge alone
    int    m_flPOpen = 0;            //!< ... and where it is open, with flankWide
    //! Blocks since the key was last open, for flankHold; huge until it opens.
    int    m_flSince = 1 << 30;
    //! The repaired block held back for the flank repair, its dry copy, and
    //! m_bBeta as it stood when it was repaired, which is what the body and
    //! fine stages see while it goes through them.
    std::vector<double> m_stY, m_stD;
    std::vector<uint8_t> m_stS;
    bool   m_stHave = false;
    double m_bBetaPrev = 0.0;
    uint64_t m_flSpans = 0, m_flSkipped = 0;
    //! The long-term spectrum for the edge: a 512-point transform of every
    //! fourth block of input, averaged over ~10 s.
    std::vector<double> m_wSpec, m_wWin, m_wRe, m_wIm, m_wStRe, m_wStIm;
    std::vector<int>    m_wRev;
    int    m_wBlock = 0;             //!< blocks since the last transform
    double m_wFrames = 0.0;          //!< transforms averaged so far
    double m_wEdge = 0.0;            //!< Hz; 0 until there is enough of them

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
    std::vector<double> m_fMag;            //!< m_fSlots x (m_dNb + m_fNb)
    std::vector<double> m_fXRe, m_fXIm;    //!< m_fSlots x (m_dNb + m_fNb)
    //! Per bin, the magnitudes of the last fineM frames, kept sorted: a hop
    //! moves one value out and one in.
    std::vector<double> m_fSorted;   //!< (m_dNb + m_fNb) x fineM, a place of pad either side
    //! Whether m_fSorted holds the last fineM frames: not where the kernels
    //! take the quantile straight from the frames. See frameQuantile().
    bool   m_fSortedLive = false;
    //! The fineQ quantile of each window of fineM frames, by the frame that
    //! ends it: frame f in row f % m_fQSlots. The windows before and after
    //! the cut frame are two of these, fineM + 1 frames apart, so each window
    //! is ranked once rather than twice.
    std::vector<double> m_fQ;        //!< m_fQSlots x (m_dNb + m_fNb)
    std::vector<double> m_fE;        //!< the guard's envelope per frame, m_fESlots
    std::vector<double> m_fSeg;      //!< 2 m_fW + 1 of it, around the cut frame
    //! What the fine stage alone removes, a ring like m_bR: the micro stage
    //! reads m_bY less this. And the guard's back-off for each frame it cut,
    //! with the frame's number + 1, for the micro stage's frames near it.
    std::vector<double> m_fR;
    std::vector<double> m_fKeep;
    std::vector<uint64_t> m_fKeepAt;
    size_t m_fKeepMask = 0;

    //! The mid stage; see Config::mid. Its m_dNb bins come first in the fine
    //! stage's band, which then starts at bin m_fK0 - m_dNb: the per-bin rings
    //! above hold m_dNb + m_fNb. The first m_dYieldNb of them are in the body
    //! stage's band. Its guard's envelope, as m_fE; and per hop of the body
    //! stage's count, whether a sample in it lay in a run of midPeak sigma.
    int    m_dNb = 0, m_dYieldNb = 0;
    std::vector<double> m_dE;
    std::vector<uint8_t> m_dRun;
    size_t m_dRunMask = 0;
    int    m_fHShift = 0;         //!< m_fH = 1 << this
    uint64_t m_dCuts = 0;

    //! The micro stage; see Config::micro. As the fine stage, at microN, hop
    //! microN/2, its input m_bY - m_fR read m_mLag samples behind the body
    //! stage's count.
    int    m_mN = 0, m_mH = 0;
    int    m_mK0 = 0, m_mNb = 0, m_mNbLow = 0;
    int    m_mSlots = 0, m_mQSlots = 0;
    int    m_mHist = 0;
    uint64_t m_mLag = 0;
    uint64_t m_mFrame = 0;
    uint64_t m_mNextHop = 0;      //!< in the micro stage's count, m_bT - m_mLag
    uint64_t m_mCuts = 0;
    double m_mNorm = 1.0;
    std::vector<double> m_mWin;
    std::vector<double> m_mRe, m_mIm;
    std::vector<double> m_mStRe, m_mStIm;
    std::vector<double> m_mTwRe, m_mTwIm;
    std::vector<int>    m_mRev;
    std::vector<double> m_mMag, m_mXRe, m_mXIm;  //!< m_mSlots x m_mNb
    std::vector<double> m_mSorted;   //!< m_mNb x microM, a place of pad either side
    bool   m_mSortedLive = false;  //!< as m_fSortedLive
    std::vector<double> m_mQ;        //!< m_mQSlots x m_mNb
    //! bodyBeta() as of the latest repair block. It only changes there, and
    //! the fine stage, hopping every N/4 samples, cannot afford to rescan
    //! Config::bodyRelease seconds of key for it each time.
    double m_bBeta = 0.0;

    //! The carpet key; see Config::carpet. Runs at carpetPeak sigma in this
    //! repair block, and in the second being counted with the samples it has
    //! spanned so far; the last carpetWin seconds' rates, as a ring; the key.
    //! A fixed array: twenty numbers a second need no allocation.
    int    m_cBlk = 0;
    double m_cRuns = 0.0, m_cSamp = 0.0;
    double m_cTotRuns = 0.0, m_cTotSamp = 0.0;   //!< since the start, for carpetFirst
    double m_cRing[kCarpetMax];
    int    m_cPos = 0, m_cHave = 0;
    double m_cKey = 0.0;
    uint64_t m_cOpen = 0;
    //! The Poisson key; see Config::carpetFano. The bin being counted and its
    //! blocks so far; the sums of the finished bins' counts and their squares,
    //! over the second being counted and since the start; each second's Fano
    //! factor, beside m_cRing.
    double m_cBin = 0.0;
    int    m_cBinBlk = 0;
    double m_cS1 = 0.0, m_cS2 = 0.0, m_cBinN = 0.0;
    double m_cTotS1 = 0.0, m_cTotS2 = 0.0, m_cTotBinN = 0.0;
    double m_cFRing[kCarpetMax];

    //! The crackle floor stage; see Config::crackleFloor. Rings of the finished
    //! audio (m_nY), its dry counterpart (m_nD) and what the stage removes
    //! (m_nR), indexed by its sample count m_nT. Per kept bin, m_nM0 .. m_nM0 +
    //! m_nNb - 1: the last five frames' transform and power, the per-bin cut's
    //! weight (the band ramped in and out) and the shelf's. Per followed bin,
    //! the first m_nTop of those: the power summed over 7 bins, the floor's
    //! history as a ring and kept sorted, the floor, the floor as it stood
    //! when the key last shut, the gain with its release. Per place in the
    //! ring, the take it stands at. The shelf: its bins m_nKd0 .. m_nKd1
    //! (relative), the last kShelfMed ratios, its gain. Held only while the
    //! stage is on.
    enum { kFloorFrames = 5, kFloorDec = 4, kShelfMed = 9, kShutCycle = 16 };
    int    m_nN = 0, m_nH = 0;
    size_t m_nMask = 0;
    uint64_t m_nT = 0;
    uint64_t m_nNextHop = 0;      //!< as m_bNextHop
    int    m_nK0 = 0, m_nM0 = 0, m_nNb = 0, m_nTop = 0;
    bool   m_nShelf = false;
    int    m_nKd0 = 0, m_nKd1 = 0;
    double m_nShelfG = 1.0, m_nShelfRel = 0.0;
    double m_nDRing[kShelfMed];
    int    m_nDHave = 0, m_nDPos = 0;
    int    m_nHist = 0;           //!< consecutive frames transformed, to 5
    int    m_nSlot = 0;
    uint64_t m_nCutCount = 0;     //!< frames whose gain has been worked out
    int    m_nHave = 0, m_nPos = 0;  //!< floor values held, and where the next goes
    uint64_t m_nTaken = 0;        //!< takes since the start; a shut key's value stands for kShutCycle / kFloorDec
    uint64_t m_nEarly = 0;        //!< of those, the ones in the stream's first two seconds
    uint64_t m_nShutHops = 0;     //!< hops since the key shut
    uint64_t m_nSinceOpen = 0;    //!< floor values taken since the key opened
    bool   m_nOpen = false;       //!< the key was open at the last hop
    bool   m_nHasSaved = false;   //!< m_nSaved holds a floor
    bool   m_nKeep = false;       //!< this opening keeps under m_nSaved
    double m_nNorm = 1.0;
    double m_nRel = 0.0;          //!< the release's step, per hop
    uint64_t m_nCuts = 0;
    std::vector<double> m_nY, m_nD, m_nR;
    std::vector<double> m_nWin;
    std::vector<double> m_nRe, m_nIm;
    std::vector<double> m_nStRe, m_nStIm;
    std::vector<double> m_nTwRe, m_nTwIm;
    std::vector<int>    m_nRev;
    std::vector<double> m_nP, m_nPb, m_nXRe, m_nXIm;   //!< kFloorFrames x m_nNb
    std::vector<double> m_nRing;     //!< m_nTop x crackleFloorHist
    std::vector<double> m_nSorted;   //!< m_nTop x (crackleFloorHist + 2), padded either side
    std::vector<double> m_nFloor, m_nSaved, m_nG;   //!< m_nTop
    //! m_nTop: the value going into each bin's window and the one it replaces,
    //! gathered so that the whole row goes through the tiled kernel at once.
    std::vector<double> m_nIn, m_nOut;
    std::vector<double> m_nWarm;     //!< m_nTop: the floor a warm pass ended with
    std::vector<uint64_t> m_nAt;     //!< crackleFloorHist
    std::vector<double> m_nWf, m_nWs;     //!< m_nNb

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

    //! The warm start; see beginWarm(). Whether output is held back; repair
    //! blocks processed; how many of them the warm pass has already counted;
    //! and the crackle floor stage's samples it saw, over which m_nWarm caps
    //! the floor.
    bool     m_warming = false;
    uint64_t m_blocks = 0;
    uint64_t m_warmBlocks = 0;
    uint64_t m_nWarmT = 0;
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
