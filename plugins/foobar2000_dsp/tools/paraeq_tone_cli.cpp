// paraeq_tone_cli <rate> <channels>
//
// Reads interleaved 32-bit float PCM from stdin (ffmpeg -f f32le -) and prints
// the track's tone profile: "valid bass brilliance hiss frames", then the
// third-octave spectrum (25 values, 40 Hz-10 kHz) when there is one. Used to
// build the library reference in paraeq_tone.cpp; see paraeq_tone.h.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS   // scanf in the fit mode
#endif

#include "paraeq_tone.h"
#ifdef TONE_DEHUM
#include "../foo_dsp_dehum/dehum_core.h"
#include <memory>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char ** argv)
{
    if (argc >= 2 && strcmp(argv[1], "fit") == 0) {
        // paraeq_tone_cli fit < lines of the track minus the library, dB: the
        // three bands, then the 25 third-octave steps. For the default
        // equaliser at 44.1 kHz, as the component would, prints the suggested
        // "LF f g HMF f g HF f g", which dots show now, and which would still
        // show with every suggestion applied.
        paraeq_tone::Deviation d;
        d.valid = d.specValid = true;
        for (;;) {
            int k = 0;
            for (; k < paraeq_tone::kMeasures; k++) if (scanf("%f", &d.db[k]) != 1) break;
            if (k < paraeq_tone::kMeasures) break;
            for (k = 0; k < paraeq_tone::kSpecBands; k++) if (scanf("%f", &d.spec[k]) != 1) break;
            if (k < paraeq_tone::kSpecBands) break;
            paraeq::Params p = paraeq::Params::defaults();
            const paraeq_tone::Suggestion s = paraeq_tone::suggest(d, p, 44100.0);
            p.lfFrequency = s.freqHz[0];  p.lfGain = s.gainDb[0];
            p.hmfFrequency = s.freqHz[1]; p.hmfGain = s.gainDb[1];
            p.hfFrequency = s.freqHz[2];  p.hfGain = s.gainDb[2];
            const paraeq_tone::Suggestion after = paraeq_tone::suggest(d, p, 44100.0);
            printf("%.1f %.2f %.1f %.2f %.1f %.2f %d%d%d %d%d%d\n",
                   s.freqHz[0], s.gainDb[0], s.freqHz[1], s.gainDb[1], s.freqHz[2], s.gainDb[2],
                   s.show[0], s.show[1], s.show[2], after.show[0], after.show[1], after.show[2]);
        }
        return 0;
    }
    if (argc < 3) { fprintf(stderr, "usage: paraeq_tone_cli rate channels < f32le\n"); return 2; }
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    const double rate = atof(argv[1]);
    const unsigned ch = (unsigned)atoi(argv[2]);
    paraeq_tone::Analyzer a;
    a.start(rate, ch);
    std::vector<float> buf(4096 * ch);
    size_t got;
#ifdef TONE_DEHUM
    // Optional pre-stage: measure what the equaliser sees after Dehum in the
    // chain, so hum at 50/60 Hz and its harmonics does not read as bass.
    dehum::Config dcfg;
    dcfg.compute(dehum::Params::defaults(), rate);
    std::vector<std::unique_ptr<dehum::Channel> > dh;
    for (unsigned c = 0; c < ch; c++) { dh.emplace_back(new dehum::Channel()); dh.back()->configure(dcfg); }
    dehum::scoped_flush_denormals ftz;
#endif
    while ((got = fread(buf.data(), sizeof(float) * ch, 4096, stdin)) > 0) {
#ifdef TONE_DEHUM
        for (unsigned c = 0; c < ch; c++) dh[c]->process(buf.data() + c, got, ch);
#endif
        a.feed(buf.data(), got);
    }
    const paraeq_tone::Profile p = a.finish();
    printf("%d %.3f %.3f %.3f %d", p.valid ? 1 : 0, p.db[0], p.db[1], p.db[2], p.frames);
    if (p.specValid)
        for (int k = 0; k < paraeq_tone::kSpecBands; k++) printf(" %.3f", p.spec[k]);
    printf("\n");
    return 0;
}
