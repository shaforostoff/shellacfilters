/* ========================================
 *  Minimal WAV reader / writer for the offline tools.
 *  Handles PCM 8/16/24/32 and IEEE float 32/64, mono or multichannel.
 * ======================================== */

#ifndef DECRACKLE_WAV_IO_H
#define DECRACKLE_WAV_IO_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

namespace wavio {

struct Audio {
    std::vector<double> samples;   //!< interleaved
    unsigned channels = 0;
    unsigned sampleRate = 0;
    unsigned sourceBits = 0;       //!< bit depth as found in the file
    bool     sourceFloat = false;

    size_t frames() const { return channels ? samples.size() / channels : 0; }
};

namespace detail {

inline uint32_t rd32(const unsigned char * p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const unsigned char * p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
inline void wr32(unsigned char * p, uint32_t v) {
    p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
inline void wr16(unsigned char * p, uint16_t v) {
    p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
}

//! fopen_s where MSVC would otherwise warn about fopen, fopen everywhere else -
//! fopen_s is Annex K, which no other C library ships.
inline FILE * open(const std::string & path, const char * mode) {
#ifdef _MSC_VER
    FILE * f = NULL;
    return fopen_s(&f, path.c_str(), mode) == 0 ? f : NULL;
#else
    return fopen(path.c_str(), mode);
#endif
}

} // namespace detail

//! Returns false and fills `err` on failure.
inline bool read(const std::string & path, Audio & out, std::string & err) {
    FILE * f = detail::open(path, "rb");
    if (f == NULL) {
        err = "cannot open " + path;
        return false;
    }
    std::vector<unsigned char> buf;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 44) { fclose(f); err = "too short to be a WAV"; return false; }
    buf.resize((size_t)len);
    size_t got = fread(&buf[0], 1, buf.size(), f);
    fclose(f);
    if (got != buf.size()) { err = "short read"; return false; }

    if (memcmp(&buf[0], "RIFF", 4) != 0 || memcmp(&buf[8], "WAVE", 4) != 0) {
        err = "not a RIFF/WAVE file";
        return false;
    }

    unsigned format = 0, channels = 0, bits = 0, rate = 0;
    const unsigned char * data = NULL;
    size_t dataBytes = 0;

    size_t pos = 12;
    while (pos + 8 <= buf.size()) {
        const char * id = (const char *)&buf[pos];
        uint32_t sz = detail::rd32(&buf[pos + 4]);
        size_t body = pos + 8;
        if (body + sz > buf.size()) sz = (uint32_t)(buf.size() - body);
        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            format   = detail::rd16(&buf[body + 0]);
            channels = detail::rd16(&buf[body + 2]);
            rate     = detail::rd32(&buf[body + 4]);
            bits     = detail::rd16(&buf[body + 14]);
            if (format == 0xFFFE && sz >= 40) {     // WAVE_FORMAT_EXTENSIBLE
                format = detail::rd16(&buf[body + 24]);
            }
        } else if (memcmp(id, "data", 4) == 0) {
            data = &buf[body];
            dataBytes = sz;
        }
        pos = body + sz + (sz & 1);
    }

    if (channels == 0 || rate == 0 || data == NULL) { err = "missing fmt or data chunk"; return false; }

    const bool isFloat = (format == 3);
    if (format != 1 && format != 3) { err = "unsupported WAV format tag"; return false; }

    const size_t bytesPer = bits / 8;
    if (bytesPer == 0) { err = "bad bit depth"; return false; }
    const size_t count = dataBytes / bytesPer;

    out.channels = channels;
    out.sampleRate = rate;
    out.sourceBits = bits;
    out.sourceFloat = isFloat;
    out.samples.resize(count);

    for (size_t i = 0; i < count; ++i) {
        const unsigned char * p = data + i * bytesPer;
        double v = 0.0;
        if (isFloat && bits == 32) {
            float t; memcpy(&t, p, 4); v = (double)t;
        } else if (isFloat && bits == 64) {
            double t; memcpy(&t, p, 8); v = t;
        } else if (bits == 8) {
            v = ((double)p[0] - 128.0) / 128.0;
        } else if (bits == 16) {
            v = (double)(int16_t)detail::rd16(p) / 32768.0;
        } else if (bits == 24) {
            int32_t t = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (t & 0x800000) t -= 0x1000000;
            v = (double)t / 8388608.0;
        } else if (bits == 32) {
            v = (double)(int32_t)detail::rd32(p) / 2147483648.0;
        } else {
            err = "unsupported bit depth";
            return false;
        }
        out.samples[i] = v;
    }
    return true;
}

//! Always writes 32 bit float, so comparisons are not clouded by requantisation.
inline bool writeFloat32(const std::string & path, const Audio & a, std::string & err) {
    FILE * f = detail::open(path, "wb");
    if (f == NULL) {
        err = "cannot create " + path;
        return false;
    }
    const uint32_t dataBytes = (uint32_t)(a.samples.size() * 4);
    unsigned char h[68];
    memcpy(h + 0, "RIFF", 4);
    detail::wr32(h + 4, 60 + dataBytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    detail::wr32(h + 16, 18);
    detail::wr16(h + 20, 3);                                  // IEEE float
    detail::wr16(h + 22, (uint16_t)a.channels);
    detail::wr32(h + 24, a.sampleRate);
    detail::wr32(h + 28, a.sampleRate * a.channels * 4);       // byte rate
    detail::wr16(h + 32, (uint16_t)(a.channels * 4));          // block align
    detail::wr16(h + 34, 32);                                  // bits
    detail::wr16(h + 36, 0);                                   // cbSize
    memcpy(h + 38, "fact", 4);
    detail::wr32(h + 42, 4);
    detail::wr32(h + 46, (uint32_t)(a.channels ? a.samples.size() / a.channels : 0));
    memcpy(h + 50, "data", 4);
    detail::wr32(h + 54, dataBytes);
    fwrite(h, 1, 58, f);

    std::vector<float> chunk;
    chunk.reserve(65536);
    for (size_t i = 0; i < a.samples.size(); ++i) {
        chunk.push_back((float)a.samples[i]);
        if (chunk.size() == 65536) {
            fwrite(&chunk[0], 4, chunk.size(), f);
            chunk.clear();
        }
    }
    if (!chunk.empty()) fwrite(&chunk[0], 4, chunk.size(), f);
    fclose(f);
    return true;
}

} // namespace wavio

#endif // DECRACKLE_WAV_IO_H
