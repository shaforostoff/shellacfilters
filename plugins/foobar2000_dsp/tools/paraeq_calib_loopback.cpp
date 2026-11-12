/* ========================================
 *  paraeq_calib_loopback - the room calibration over real hardware
 *
 *  Plays music out of one device and records another - a USB interface with
 *  its output cabled to its input, or a loudspeaker and a microphone - and
 *  runs the same Analyzer and the same WASAPI capture the foobar2000 window
 *  does. A cable is a flat room, so --room puts a known resonance into what
 *  is played (and not into the reference), and the suggestion has to find it.
 *  --lead feeds the reference that many seconds before it is played, the way
 *  foobar2000's output buffer does.
 *
 *    paraeq_calib_loopback --list
 *    paraeq_calib_loopback --out <n|name> --in <n|name> [--wav file.wav]
 *        [--sec 60] [--lead 8] [--room 320,4,8] [--room2 750,4,6.5] [--level -12]
 *
 *  Windows only. Not part of the shipped component.
 * ======================================== */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "paraeq_calibrate.h"
#include "paraeq_core.h"
#include "wasapi_capture.h"
#include "wav_io.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace paraeq_calib;

namespace {

const double kPi = 3.14159265358979323846;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    double uni() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; }
};

//! Notes with harmonics and a little noise; see paraeq_calibrate_verify.
std::vector<float> makeMusic(double fs, double sec)
{
    Rng rng(1);
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
                for (int h = 1; h <= 10 && f0 * h < fs * 0.45; ++h) s += sin(2.0 * kPi * f0 * h * tt) / h;
                out[i] += (float)(amp * env * s);
            }
            t += dur * (0.4 + rng.uni());
        }
    }
    for (size_t i = 0; i < n; ++i) out[i] += (float)(0.01 * (2.0 * rng.uni() - 1.0));
    return out;
}

std::vector<float> loadWav(const std::string & path, double fs)
{
    wavio::Audio a;
    std::string err;
    if (!wavio::read(path, a, err)) { fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); exit(1); }
    const size_t frames = a.frames();
    std::vector<float> mono(frames);
    for (size_t f = 0; f < frames; ++f) {
        double s = 0.0;
        for (unsigned c = 0; c < a.channels; ++c) s += a.samples[f * a.channels + c];
        mono[f] = (float)(s / a.channels);
    }
    // Linear interpolation onto the device rate: good enough below 3.4 kHz.
    const double step = (double)a.sampleRate / fs;
    std::vector<float> out;
    for (double p = 0.0; p + 1.0 < (double)frames; p += step) {
        const size_t i = (size_t)p;
        const double t = p - (double)i;
        out.push_back((float)(mono[i] * (1.0 - t) + mono[i + 1] * t));
    }
    return out;
}

std::wstring pick(const std::vector<wasapi_capture::Device> & devs, const std::string & key)
{
    char * end = nullptr;
    const long idx = strtol(key.c_str(), &end, 10);
    if (end && *end == 0 && idx >= 0 && idx < (long)devs.size()) return devs[(size_t)idx].id;
    const std::wstring wkey(key.begin(), key.end());
    for (size_t i = 0; i < devs.size(); ++i)
        if (devs[i].name.find(wkey) != std::wstring::npos) return devs[i].id;
    fprintf(stderr, "no device matches '%s'\n", key.c_str());
    exit(1);
}

void list(const char * title, const std::vector<wasapi_capture::Device> & devs, const std::wstring & def)
{
    printf("%s:\n", title);
    for (size_t i = 0; i < devs.size(); ++i)
        printf("  %zu%s %ls\n", i, devs[i].id == def ? "*" : " ", devs[i].name.c_str());
}

} // anonymous namespace

int main(int argc, char ** argv)
{
    std::string outKey, inKey, wav;
    double sec = 60.0, lead = 8.0, levelDb = -12.0;
    double roomF = 0.0, roomQ = 4.0, roomG = 8.0;
    double room2F = 0.0, room2Q = 4.0, room2G = 6.5;
    bool doList = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", a.c_str()); exit(2); } return argv[++i]; };
        if (a == "--list") doList = true;
        else if (a == "--out") outKey = next();
        else if (a == "--in") inKey = next();
        else if (a == "--wav") wav = next();
        else if (a == "--sec") sec = atof(next().c_str());
        else if (a == "--lead") lead = atof(next().c_str());
        else if (a == "--level") levelDb = atof(next().c_str());
        else if (a == "--room") {
            if (sscanf_s(next().c_str(), "%lf,%lf,%lf", &roomF, &roomQ, &roomG) != 3) { fprintf(stderr, "--room F,Q,G\n"); return 2; }
        } else if (a == "--room2") {
            if (sscanf_s(next().c_str(), "%lf,%lf,%lf", &room2F, &room2Q, &room2G) != 3) { fprintf(stderr, "--room2 F,Q,G\n"); return 2; }
        } else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<wasapi_capture::Device> outs, ins;
    std::wstring defOut, defIn;
    wasapi_capture::listDevices(outs, defOut, true);
    wasapi_capture::listDevices(ins, defIn, false);
    if (doList || outKey.empty() || inKey.empty()) {
        list("Playback", outs, defOut);
        list("Recording", ins, defIn);
        return doList ? 0 : 2;
    }
    const std::wstring outId = pick(outs, outKey), inId = pick(ins, inKey);

    // Playback: the endpoint's own mix format, float.
    IMMDeviceEnumerator * en = nullptr;
    IMMDevice * dev = nullptr;
    IAudioClient * client = nullptr;
    IAudioRenderClient * render = nullptr;
    WAVEFORMATEX * fmt = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en)) ||
        FAILED(en->GetDevice(outId.c_str(), &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client)) ||
        FAILED(client->GetMixFormat(&fmt))) {
        fprintf(stderr, "cannot open the playback device\n");
        return 1;
    }
    WORD tag = fmt->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) tag = (WORD)((WAVEFORMATEXTENSIBLE *)fmt)->SubFormat.Data1;
    if (tag != WAVE_FORMAT_IEEE_FLOAT || fmt->wBitsPerSample != 32) { fprintf(stderr, "playback mix format is not float32\n"); return 1; }
    const double fs = fmt->nSamplesPerSec;
    const unsigned och = fmt->nChannels;
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, fmt, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), (void **)&render))) {
        fprintf(stderr, "cannot start the playback device\n");
        return 1;
    }
    UINT32 bufFrames = 0;
    client->GetBufferSize(&bufFrames);

    // The reference is what the equaliser would send; what is played is that
    // through the simulated room.
    std::vector<float> ref = wav.empty() ? makeMusic(fs, sec + lead + 2.0) : loadWav(wav, fs);
    const size_t need = (size_t)((sec + lead + 2.0) * fs);
    if (ref.size() < need) { const size_t n0 = ref.size(); ref.resize(need); for (size_t i = n0; i < need; ++i) ref[i] = ref[i % n0]; }
    float peak = 0.0f;
    for (float v : ref) peak = std::max(peak, fabsf(v));
    const float gain = (float)(pow(10.0, levelDb / 20.0) / std::max(peak, 1e-6f));
    for (float & v : ref) v *= gain;

    std::vector<float> played(ref);
    const double rooms[2][3] = { { roomF, roomQ, roomG }, { room2F, room2Q, room2G } };
    for (int k = 0; k < 2; ++k) {
        if (!(rooms[k][0] > 0.0)) continue;
        const paraeq::Biquad b = paraeq::design(paraeq::kPeaking, rooms[k][0], rooms[k][1], rooms[k][2], fs);
        double z1 = 0, z2 = 0;
        for (float & v : played) {
            const double in = v, out = b.b0 * in + z1;
            z1 = b.b1 * in - b.a1 * out + z2;
            z2 = b.b2 * in - b.a2 * out;
            v = (float)out;
        }
    }
    {
        float pk = 0.0f;
        for (float v : played) pk = std::max(pk, fabsf(v));
        if (pk > 0.99f) for (float & v : played) v *= 0.99f / pk;
    }

    wasapi_capture::Capture cap;
    std::wstring err;
    if (!cap.open(inId, err)) { fprintf(stderr, "recording device: %ls\n", err.c_str()); return 1; }
    printf("playing %.0f Hz %u ch, recording %.0f Hz %u ch, lead %.1f s, room %s\n",
           fs, och, cap.rate(), cap.channels(), lead, roomF > 0 ? "on" : "flat");
    if (roomF > 0) printf("room: %.0f Hz, Q %.1f, %+.1f dB\n", roomF, roomQ, roomG);
    if (room2F > 0) printf("room: %.0f Hz, Q %.1f, %+.1f dB\n", room2F, room2Q, room2G);

    Analyzer an;
    an.setMinLag(lead - 1.0);   // --lead stands in for foobar2000's buffer
    const size_t leadFrames = (size_t)(lead * fs);
    an.feedOutput(ref.data(), leadFrames, fs);
    size_t pos = 0;
    const size_t total = (size_t)(sec * fs);
    std::vector<float> mic;
    client->Start();
    int printed = 0;
    while (pos < total) {
        UINT32 pad = 0;
        client->GetCurrentPadding(&pad);
        const UINT32 avail = std::min<UINT32>(bufFrames - pad, (UINT32)(total - pos));
        if (avail > 0) {
            BYTE * data = nullptr;
            if (SUCCEEDED(render->GetBuffer(avail, &data))) {
                float * f = (float *)data;
                for (UINT32 i = 0; i < avail; ++i)
                    for (unsigned c = 0; c < och; ++c) f[i * och + c] = played[pos + i];
                render->ReleaseBuffer(avail, 0);
                an.feedOutput(ref.data() + pos + leadFrames, avail, fs);
                pos += avail;
            }
        }
        mic.clear();
        if (!cap.read(mic)) { fprintf(stderr, "recording device stopped\n"); break; }
        if (!mic.empty() && an.feedMic(mic.data(), mic.size(), cap.rate())) {
            const Result & r = an.result();
            while (printed < r.blocks) {
                ++printed;
                printf("block %3d  matched %3d  delay %6.3f s  conf %5.1f  mic %6.1f dBFS%s",
                       r.blocks, r.matched, r.lagSec, r.confidence, r.micDb, r.micClipped ? " CLIP" : "");
                if (r.peaks > 0) printf("  -> %.0f Hz Q %.1f %.1f dB", r.peak[0].frequency, r.peak[0].q, r.peak[0].gainDb);
                printf("\n");
            }
        }
        Sleep(10);
    }
    client->Stop();
    cap.close();

    const Result & r = an.result();
    printf("\n%d of %d blocks matched\n", r.matched, r.blocks);
    for (int i = 0; i < r.peaks; ++i)
        printf("peak %d: %.0f Hz  Q %.1f  gain %.1f dB  (height %.1f dB)\n",
               i, r.peak[i].frequency, r.peak[i].q, r.peak[i].gainDb, r.peak[i].heightDb);
    printf("ready: %s, %s\n", r.ready() ? "yes" : "no", r.sure ? "sure" : "unsure");
    for (int i = 0; i < r.options; ++i)
        printf("option %d: %.0f Hz  Q %.1f  gain %.1f dB\n", i + 1, r.option[i].frequency, r.option[i].q, r.option[i].gainDb);
    printf("\n   Hz   response  trend\n");
    for (size_t j = 0; j < r.frequency.size(); j += kGridPerOctave / 6)
        printf("%6.0f  %7.2f  %7.2f\n", r.frequency[j], r.responseDb[j], r.trendDb[j]);
    return 0;
}
