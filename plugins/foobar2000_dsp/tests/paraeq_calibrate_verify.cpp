/* ========================================
 *  paraeq_calibrate_verify - the room calibration against synthetic rooms
 *
 *  Music is synthesised, played into a simulated room - a resonance where the
 *  test puts one, a reverberant tail, a microphone on a different and drifting
 *  clock, background noise and a stretch of someone talking - and fed to the
 *  analyser the way the foobar2000 window feeds it: the output seconds ahead
 *  of what the microphone hears. The suggestion has to land on the resonance;
 *  a flat room has to give no confident one.
 *
 *  No SDK.
 * ======================================== */

#include "paraeq_calibrate.h"
#include "paraeq_core.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace paraeq_calib;

static int sFail = 0;
static const double kPi = 3.14159265358979323846;

static void ckTrue(const char * what, bool ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++sFail;
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uni() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; }
    double sym() { return 2.0 * uni() - 1.0; }
};

//! Something with music's habits: notes with harmonics that start and decay,
//! percussive noise bursts, and a little continuous noise so no band is ever
//! silent for long.
static std::vector<float> makeMusic(double fs, double sec, uint64_t seed)
{
    Rng rng(seed);
    const size_t n = (size_t)(fs * sec);
    std::vector<float> out(n, 0.0f);
    for (int voice = 0; voice < 5; ++voice) {
        double t = rng.uni() * 0.3;
        while (t < sec) {
            const double f0 = 70.0 * pow(2.0, rng.uni() * 4.0);
            const double dur = 0.15 + rng.uni() * 0.6;
            const double amp = 0.05 + 0.08 * rng.uni();
            const size_t a = (size_t)(t * fs), b = std::min(n, (size_t)((t + dur * 2.0) * fs));
            for (size_t i = a; i < b; ++i) {
                const double tt = (double)(i - a) / fs;
                const double env = (1.0 - exp(-tt * 200.0)) * exp(-tt / dur * 2.0);
                double s = 0.0;
                for (int h = 1; h <= 10; ++h) {
                    if (f0 * h > fs * 0.45) break;
                    s += sin(2.0 * kPi * f0 * h * tt) / h;
                }
                out[i] += (float)(amp * env * s);
            }
            t += dur * (0.4 + rng.uni());
        }
    }
    // Bursts and a floor of noise, both roughly pink.
    double b0 = 0, b1 = 0, b2 = 0;
    double burst = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double w = rng.sym();
        b0 = 0.99765 * b0 + w * 0.0990460;
        b1 = 0.96300 * b1 + w * 0.2965164;
        b2 = 0.57000 * b2 + w * 1.0526913;
        const double pink = (b0 + b1 + b2 + w * 0.1848) * 0.2;
        if (rng.uni() < 3.0 / fs) burst = 0.4;
        burst *= exp(-1.0 / (0.05 * fs));
        out[i] += (float)(pink * (0.01 + burst));
    }
    return out;
}

struct Room {
    bool   resonance = false;
    double freq = 320.0, q = 4.0, gainDb = 8.0;
    double reverb = 0.3;          //!< level of the diffuse tail
    double micRate = 48000.0;
    double driftPpm = 30.0;
    double noise = 0.002;
    double lagSec = 7.3;
    double talkFrom = 30.0, talkTo = 36.0;

    //! A second resonance, in series with the first; and, if switchSec is
    //! set, the first one only before that point of the walk and the second
    //! only after it - a room that rings differently at each end.
    bool   second = false;
    double freq2 = 700.0, q2 = 4.0, gainDb2 = 7.0;
    double switchSec = 0.0;
};

static std::vector<float> roomResponse(const std::vector<float> & x, double fs, const Room & room)
{
    std::vector<double> y(x.size());
    paraeq::Biquad bq, bq2;
    if (room.resonance) bq = paraeq::design(paraeq::kPeaking, room.freq, room.q, room.gainDb, fs);
    if (room.second) bq2 = paraeq::design(paraeq::kPeaking, room.freq2, room.q2, room.gainDb2, fs);
    // The output is ahead of the microphone by lagSec, so the walk's switch
    // point falls lagSec later in the output's own time.
    const size_t sw = room.switchSec > 0.0 ? (size_t)((room.switchSec + room.lagSec) * fs) : 0;
    double z1 = 0, z2 = 0, w1 = 0, w2 = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double in = x[i];
        const double a = bq.b0 * in + z1;
        z1 = bq.b1 * in - bq.a1 * a + z2;
        z2 = bq.b2 * in - bq.a2 * a;
        const double b = bq2.b0 * in + w1;
        w1 = bq2.b1 * in - bq2.a1 * b + w2;
        w2 = bq2.b2 * in - bq2.a2 * b;
        y[i] = (sw == 0 || i < sw) ? a : b;
    }
    // Both at once: the second in series after the first.
    if (sw == 0 && room.second) {
        double v1 = 0, v2 = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double in = y[i];
            const double out = bq2.b0 * in + v1;
            v1 = bq2.b1 * in - bq2.a1 * out + v2;
            v2 = bq2.b2 * in - bq2.a2 * out;
            y[i] = out;
        }
    }
    // Four Schroeder combs, about a second of tail.
    const double delays[4] = { 0.0297, 0.0371, 0.0411, 0.0437 };
    std::vector<double> wet(x.size(), 0.0);
    for (int c = 0; c < 4; ++c) {
        const size_t d = (size_t)(delays[c] * fs);
        std::vector<double> buf(x.size(), 0.0);
        for (size_t i = 0; i < x.size(); ++i) {
            buf[i] = y[i] + (i >= d ? 0.8 * buf[i - d] : 0.0);
            wet[i] += buf[i] * 0.25;
        }
    }
    std::vector<float> out(x.size());
    for (size_t i = 0; i < x.size(); ++i) out[i] = (float)(y[i] + room.reverb * wet[i]);
    return out;
}

//! Linear interpolation onto the microphone's clock. Crude, but the analyser
//! only looks below 3.4 kHz, where it is good enough.
static std::vector<float> toMicClock(const std::vector<float> & x, double fs, double micFs)
{
    const double step = fs / micFs;
    std::vector<float> out;
    out.reserve((size_t)(x.size() / step) + 1);
    for (double p = 0.0; p + 1.0 < (double)x.size(); p += step) {
        const size_t i = (size_t)p;
        const double f = p - (double)i;
        out.push_back((float)(x[i] * (1.0 - f) + x[i + 1] * f));
    }
    return out;
}

static Result run(const Room & room, double sec, uint64_t seed, double * msPerBlock)
{
    const double fs = 44100.0;
    const std::vector<float> music = makeMusic(fs, sec + room.lagSec + 1.0, seed);
    std::vector<float> heard = roomResponse(music, fs, room);
    const std::vector<float> talk = makeMusic(fs, sec + 1.0, seed + 1000);
    for (size_t i = (size_t)(room.talkFrom * fs); i < (size_t)(room.talkTo * fs) && i < heard.size(); ++i)
        heard[i] = talk[i];
    const double micFs = room.micRate * (1.0 + room.driftPpm * 1e-6);
    std::vector<float> mic = toMicClock(heard, fs, micFs);
    Rng rng(seed + 7);
    for (size_t i = 0; i < mic.size(); ++i) mic[i] = (float)(0.3 * mic[i] + room.noise * rng.sym());

    Analyzer an;
    an.setMinLag(room.lagSec - 1.0);   // as the window does with the buffer
    const double step = 0.01;
    size_t outPos = 0, micPos = 0;
    double spent = 0.0;
    int blocks = 0;
    for (double t = 0.0; t < sec; t += step) {
        const size_t outEnd = std::min(music.size(), (size_t)((t + room.lagSec) * fs));
        if (outEnd > outPos) { an.feedOutput(music.data() + outPos, outEnd - outPos, fs); outPos = outEnd; }
        const size_t micEnd = std::min(mic.size(), (size_t)(t * room.micRate));
        if (micEnd > micPos) {
            const auto t0 = std::chrono::steady_clock::now();
            if (an.feedMic(mic.data() + micPos, micEnd - micPos, room.micRate)) {
                spent += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ++blocks;
            }
            micPos = micEnd;
        }
    }
    if (msPerBlock) *msPerBlock = blocks ? spent / blocks : 0.0;
    return an.result();
}

static void print(const char * name, const Result & r, double ms)
{
    printf("%s: %d/%d blocks matched, lag %.3f s, conf %.1f, %.1f ms/block\n",
           name, r.matched, r.blocks, r.lagSec, r.confidence, ms);
    for (int i = 0; i < r.peaks; ++i)
        printf("    peak %d: %.0f Hz  Q %.1f  gain %.1f dB  (height %.1f dB)\n", i,
               r.peak[i].frequency, r.peak[i].q, r.peak[i].gainDb, r.peak[i].heightDb);
}

static void printOptions(const Result & r)
{
    printf("    %s, %d option(s):", r.sure ? "sure" : "unsure", r.options);
    for (int i = 0; i < r.options; ++i)
        printf("  %.0f Hz Q %.1f %.1f dB", r.option[i].frequency, r.option[i].q, r.option[i].gainDb);
    printf("\n");
}

static bool hasOption(const Result & r, double hz)
{
    for (int i = 0; i < r.options; ++i)
        if (fabs(log2(r.option[i].frequency / hz)) < 1.0 / 6.0) return true;
    return false;
}

static bool allInRange(const Result & r)
{
    for (int i = 0; i < r.options; ++i)
        if (r.option[i].frequency > kSearchHiHz || r.option[i].frequency < kSearchLoHz) return false;
    for (int i = 0; i < r.peaks; ++i)
        if (r.peak[i].frequency > kSearchHiHz || r.peak[i].frequency < kSearchLoHz) return false;
    return true;
}

int main()
{
    // Resampler: a 1 kHz sine at 44.1 kHz comes out at 8 kHz with its level.
    {
        Resampler rs;
        rs.reset(44100.0);
        std::vector<float> in(44100), out;
        for (size_t i = 0; i < in.size(); ++i) in[i] = (float)sin(2.0 * kPi * 1000.0 * i / 44100.0);
        rs.process(in.data(), 1000, out);
        rs.process(in.data() + 1000, in.size() - 1000, out);
        double s = 0.0, c = 0.0, e = 0.0;
        const size_t a = 1000, b = out.size() - 100;
        for (size_t i = a; i < b; ++i) {
            s += out[i] * sin(2.0 * kPi * 1000.0 * i / 8000.0);
            c += out[i] * cos(2.0 * kPi * 1000.0 * i / 8000.0);
            e += (double)out[i] * out[i];
        }
        const double amp = 2.0 * sqrt(s * s + c * c) / (double)(b - a);
        const double rms = sqrt(e / (double)(b - a));
        printf("resampler: %zu out of 44100, 1 kHz amplitude %.4f, rms %.4f\n", out.size(), amp, rms);
        ckTrue("resampler keeps 1 kHz at unity", fabs(amp - 1.0) < 0.01);
        ckTrue("resampler output is the sine and nothing else", fabs(rms - sqrt(0.5)) < 0.01);
        ckTrue("resampler output count", out.size() > 7900 && out.size() <= 8000);

        // 4.6 kHz must be gone - it would alias to 3.4, inside the grid.
        rs.reset(48000.0);
        out.clear();
        std::vector<float> hi(48000);
        for (size_t i = 0; i < hi.size(); ++i) hi[i] = (float)sin(2.0 * kPi * 4600.0 * i / 48000.0);
        rs.process(hi.data(), hi.size(), out);
        e = 0.0;
        for (size_t i = 1000; i < out.size() - 100; ++i) e += (double)out[i] * out[i];
        const double db = 10.0 * log10(e / (double)(out.size() - 1100) + 1e-30);
        printf("resampler: 4.6 kHz at 48 kHz leaves %.1f dB\n", db);
        ckTrue("resampler stops above Nyquist", db < -60.0);
    }

    // FFT round trip.
    {
        Fft fft;
        fft.init(10);
        std::vector<double> re(1024), im(1024), r0(1024);
        Rng rng(3);
        for (size_t i = 0; i < 1024; ++i) { re[i] = r0[i] = rng.sym(); im[i] = 0.0; }
        fft.run(re.data(), im.data(), false);
        fft.run(re.data(), im.data(), true);
        double err = 0.0;
        for (size_t i = 0; i < 1024; ++i) err = std::max(err, fabs(re[i] / 1024.0 - r0[i]));
        ckTrue("fft round trip", err < 1e-12);
    }

    const double sec = 90.0;
    double ms = 0.0;

    {
        Room room;
        room.resonance = true;
        const Result r = run(room, sec, 1, &ms);
        print("320 Hz, Q 4, +8 dB", r, ms);
        ckTrue("most blocks matched", r.matched >= r.blocks - 6);
        ckTrue("talking blocks rejected", r.matched <= r.blocks - 2);
        ckTrue("lag found", fabs(r.lagSec - 7.3) < 0.1);
        ckTrue("ready", r.ready());
        ckTrue("frequency within 6%", r.peaks > 0 && fabs(r.peak[0].frequency / 320.0 - 1.0) < 0.06);
        ckTrue("Q between 2.5 and 6", r.peaks > 0 && r.peak[0].q >= 2.5 && r.peak[0].q <= 6.0);
        ckTrue("gain between -10 and -5 dB", r.peaks > 0 && r.peak[0].gainDb <= -5.0 && r.peak[0].gainDb >= -10.0);
        ckTrue("sure, one option", r.sure && r.options == 1);
        // Blocks arrive every 2 s; this leaves a slow machine, or a busy test
        // run, a quarter of that.
        ckTrue("block analysis under 500 ms", ms < 500.0);
    }
    {
        Room room;
        room.resonance = true;
        room.freq = 1200.0; room.q = 1.5; room.gainDb = 6.0;
        room.micRate = 44100.0; room.driftPpm = -80.0; room.lagSec = 11.5;
        const Result r = run(room, sec, 2, &ms);
        print("1.2 kHz, Q 1.5, +6 dB, 11.5 s lag", r, ms);
        ckTrue("lag found", fabs(r.lagSec - 11.5) < 0.1);
        ckTrue("frequency within 10%", r.peaks > 0 && fabs(r.peak[0].frequency / 1200.0 - 1.0) < 0.10);
        ckTrue("Q between 1 and 3", r.peaks > 0 && r.peak[0].q >= 1.0 && r.peak[0].q <= 3.0);
        ckTrue("gain between -8 and -3 dB", r.peaks > 0 && r.peak[0].gainDb <= -3.0 && r.peak[0].gainDb >= -8.0);
    }
    {
        // A linear room and nothing else: the response must read the filter.
        Room room;
        room.resonance = true;
        room.freq = 1200.0; room.q = 1.5; room.gainDb = 6.0;
        room.reverb = 0.0; room.noise = 0.0; room.driftPpm = 0.0;
        room.micRate = 44100.0; room.talkFrom = room.talkTo = 0.0;
        const Result r = run(room, 40.0, 4, &ms);
        print("clean 1.2 kHz, Q 1.5, +6 dB", r, ms);
        const paraeq::Biquad b = paraeq::design(paraeq::kPeaking, 1200.0, 1.5, 6.0, 44100.0);
        double worst = 0.0;
        for (size_t j = 0; j < r.frequency.size(); ++j) {
            const double want = paraeq::magnitudeDb(b, r.frequency[j], 44100.0);
            const double got  = r.responseDb[j] - (r.responseDb[0] - paraeq::magnitudeDb(b, r.frequency[0], 44100.0));
            if (j % 24 == 0) printf("    %6.0f Hz  want %5.2f  got %5.2f\n", r.frequency[j], want, got);
            worst = std::max(worst, fabs(got - want));
        }
        ckTrue("clean response within 0.7 dB of the filter", worst < 0.7);
        ckTrue("clean height within 1 dB of 6", r.peaks > 0 && fabs(r.peak[0].heightDb - 6.0) < 1.0);
    }
    {
        // Two resonances of nearly the same size: not sure, both offered.
        Room room;
        room.resonance = true;
        room.freq = 300.0; room.q = 4.0; room.gainDb = 7.0;
        room.second = true;
        room.freq2 = 750.0; room.q2 = 4.0; room.gainDb2 = 6.5;
        const Result r = run(room, sec, 5, &ms);
        print("300 Hz +7 and 750 Hz +6.5", r, ms);
        printOptions(r);
        ckTrue("unsure", !r.sure);
        ckTrue("two or three options", r.options >= 2);
        ckTrue("both resonances offered", hasOption(r, 300.0) && hasOption(r, 750.0));
        ckTrue("all options at or below 3 kHz", allInRange(r));
    }
    {
        // A narrow mode on a broad midrange hump, as a hall measured with a
        // phone looked: the mode is the first option, the hump still offered.
        Room room;
        room.resonance = true;
        room.freq = 600.0; room.q = 0.7; room.gainDb = 9.0;
        room.second = true;
        room.freq2 = 320.0; room.q2 = 4.0; room.gainDb2 = 5.0;
        const Result r = run(room, sec, 7, &ms);
        print("320 Hz +5 on a 600 Hz Q 0.7 +9 hump", r, ms);
        printOptions(r);
        ckTrue("ready", r.ready());
        ckTrue("the mode first", r.options > 0 && fabs(log2(r.option[0].frequency / 320.0)) < 1.0 / 12.0);
        ckTrue("the mode's Q 2.5 or more", r.options > 0 && r.option[0].q >= 2.5);
        ckTrue("the mode's gain -3 to -7 dB", r.options > 0 && r.option[0].gainDb <= -3.0 && r.option[0].gainDb >= -7.0);
        bool hump = false;
        for (int i = 0; i < r.options; ++i) if (r.option[i].q < 1.4) hump = true;
        ckTrue("the hump offered too", hump);
        ckTrue("unsure", !r.sure);
    }
    {
        // One end of the room rings at 300 Hz, the other at 1 kHz: the halves
        // of the walk disagree.
        Room room;
        room.resonance = true;
        room.freq = 300.0; room.q = 4.0; room.gainDb = 8.0;
        room.second = true;
        room.freq2 = 1000.0; room.q2 = 3.0; room.gainDb2 = 6.0;
        room.switchSec = 45.0;
        room.talkFrom = room.talkTo = 0.0;
        const Result r = run(room, sec, 6, &ms);
        print("300 Hz then 1 kHz", r, ms);
        printOptions(r);
        ckTrue("unsure", !r.sure);
        ckTrue("both ends offered", hasOption(r, 300.0) && hasOption(r, 1000.0));
        ckTrue("all options at or below 3 kHz", allInRange(r));
    }
    {
        // A resonance above the band: never suggested.
        Room room;
        room.resonance = true;
        room.freq = 3400.0; room.q = 3.0; room.gainDb = 9.0;
        const Result r = run(room, sec, 7, &ms);
        print("3.4 kHz only", r, ms);
        printOptions(r);
        ckTrue("all options at or below 3 kHz", allInRange(r));
    }
    {
        Room room;
        room.resonance = false;
        room.micRate = 16000.0;
        const Result r = run(room, sec, 3, &ms);
        print("no resonance, 16 kHz mic", r, ms);
        ckTrue("matched", r.matched >= r.blocks - 6);
        ckTrue("nothing above 2 dB", r.peaks == 0 || r.peak[0].heightDb < 2.0);
    }

    printf(sFail ? "\n%d FAILED\n" : "\nall passed\n", sFail);
    return sFail ? 1 : 0;
}
