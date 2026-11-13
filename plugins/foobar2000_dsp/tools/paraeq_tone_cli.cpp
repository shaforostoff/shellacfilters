// paraeq_tone_cli <rate> <channels>
//
// Reads interleaved 32-bit float PCM from stdin (ffmpeg -f f32le -) and prints
// the track's tone profile: "valid bass brilliance hiss frames". Used to build
// the library reference in paraeq_tone.cpp; see paraeq_tone.h.

#include "paraeq_tone.h"
#ifdef TONE_DEHUM
#include "../foo_dsp_dehum/dehum_core.h"
#include <memory>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char ** argv)
{
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
    printf("%d %.3f %.3f %.3f %d\n", p.valid ? 1 : 0, p.db[0], p.db[1], p.db[2], p.frames);
    return 0;
}
