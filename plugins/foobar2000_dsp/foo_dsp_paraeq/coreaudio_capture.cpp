/* ========================================
 *  foo_dsp_paraeq - microphone input for the room calibration, macOS
 *
 *  See coreaudio_capture.h.
 * ======================================== */

#include "coreaudio_capture.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

namespace coreaudio_capture {

namespace {

AudioObjectPropertyAddress address(AudioObjectPropertySelector sel,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    // Element 0 is kAudioObjectPropertyElementMain, which is macOS 12 and
    // later by name; the deployment target is 11.
    AudioObjectPropertyAddress a = { sel, scope, 0 };
    return a;
}

std::string utf8(CFStringRef s)
{
    if (!s) return std::string();
    const CFIndex len = CFStringGetLength(s);
    const CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::vector<char> buf((size_t)max);
    if (!CFStringGetCString(s, buf.data(), max, kCFStringEncodingUTF8)) return std::string();
    return std::string(buf.data());
}

//! A device's string property, UTF-8; empty if it has none.
std::string stringProperty(AudioObjectID dev, AudioObjectPropertySelector sel)
{
    const AudioObjectPropertyAddress a = address(sel);
    CFStringRef s = nullptr;
    UInt32 size = sizeof(s);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &s) != noErr || !s) return std::string();
    const std::string r = utf8(s);
    CFRelease(s);
    return r;
}

//! Input channels, over all the device's input streams.
unsigned inputChannels(AudioObjectID dev)
{
    const AudioObjectPropertyAddress a = address(kAudioDevicePropertyStreamConfiguration,
                                                 kAudioObjectPropertyScopeInput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> buf(size);
    AudioBufferList * list = (AudioBufferList *)buf.data();
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
    unsigned n = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) n += list->mBuffers[i].mNumberChannels;
    return n;
}

double nominalRate(AudioObjectID dev)
{
    const AudioObjectPropertyAddress a = address(kAudioDevicePropertyNominalSampleRate);
    Float64 r = 0.0;
    UInt32 size = sizeof(r);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &r) != noErr) return 0.0;
    return r;
}

bool allDevices(std::vector<AudioObjectID> & out)
{
    const AudioObjectPropertyAddress a = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return false;
    out.resize(size / sizeof(AudioObjectID));
    if (out.empty()) return true;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, out.data()) != noErr)
        return false;
    out.resize(size / sizeof(AudioObjectID));
    return true;
}

//! Whether every input stream hands the HAL's side 32-bit native floats, which
//! is what deliver() reads. It is the HAL's virtual format on every device
//! seen; anything else is refused rather than misread.
bool inputIsFloat(AudioObjectID dev)
{
    const AudioObjectPropertyAddress a = address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr) return false;
    std::vector<AudioStreamID> streams(size / sizeof(AudioStreamID));
    if (streams.empty()) return false;
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, streams.data()) != noErr) return false;
    for (AudioStreamID s : streams) {
        const AudioObjectPropertyAddress f = address(kAudioStreamPropertyVirtualFormat);
        AudioStreamBasicDescription d = {};
        UInt32 dsize = sizeof(d);
        if (AudioObjectGetPropertyData(s, &f, 0, nullptr, &dsize, &d) != noErr) return false;
        if (d.mFormatID != kAudioFormatLinearPCM || !(d.mFormatFlags & kAudioFormatFlagIsFloat)
            || d.mBitsPerChannel != 32 || (d.mFormatFlags & kAudioFormatFlagIsBigEndian))
            return false;
    }
    return true;
}

OSStatus ioProc(AudioObjectID, const AudioTimeStamp *, const AudioBufferList * input,
                const AudioTimeStamp *, AudioBufferList *, const AudioTimeStamp *, void * client)
{
    if (input) static_cast<Capture *>(client)->deliver(input);
    return noErr;
}

} // anonymous namespace

bool listDevices(std::vector<Device> & out, std::string & defaultId)
{
    out.clear();
    defaultId.clear();
    std::vector<AudioObjectID> ids;
    if (!allDevices(ids)) return false;
    for (AudioObjectID dev : ids) {
        if (inputChannels(dev) == 0) continue;
        Device d;
        d.id = stringProperty(dev, kAudioDevicePropertyDeviceUID);
        d.name = stringProperty(dev, kAudioObjectPropertyName);
        if (d.id.empty()) continue;
        if (d.name.empty()) d.name = d.id;
        out.push_back(d);
    }
    const AudioObjectPropertyAddress a = address(kAudioHardwarePropertyDefaultInputDevice);
    AudioObjectID def = kAudioObjectUnknown;
    UInt32 size = sizeof(def);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &def) == noErr
        && def != kAudioObjectUnknown)
        defaultId = stringProperty(def, kAudioDevicePropertyDeviceUID);
    return true;
}

bool Capture::open(const std::string & id, std::string & error)
{
    close();
    std::vector<AudioObjectID> ids;
    if (!allDevices(ids)) { error = "Could not list the audio devices."; return false; }
    AudioObjectID dev = kAudioObjectUnknown;
    for (AudioObjectID d : ids)
        if (stringProperty(d, kAudioDevicePropertyDeviceUID) == id) { dev = d; break; }
    if (dev == kAudioObjectUnknown) { error = "The microphone is no longer connected."; return false; }

    m_channels = inputChannels(dev);
    m_rate = nominalRate(dev);
    if (m_channels == 0 || !(m_rate > 0.0)) { error = "The device has no input."; return false; }
    if (!inputIsFloat(dev)) { error = "The device's input format is not supported."; return false; }

    m_ring.assign((size_t)1 << kRingOrder, 0.0f);
    m_write.store(0, std::memory_order_relaxed);
    m_read = 0;
    m_device = dev;

    AudioDeviceIOProcID proc = nullptr;
    OSStatus st = AudioDeviceCreateIOProcID(dev, ioProc, this, &proc);
    if (st != noErr || !proc) {
        error = "Could not open the microphone (error " + std::to_string((int)st) + ").";
        return false;
    }
    m_proc = (void *)proc;
    st = AudioDeviceStart(dev, proc);
    if (st != noErr) {
        error = "Could not start the microphone (error " + std::to_string((int)st) + ").";
        close();
        return false;
    }
    return true;
}

void Capture::close()
{
    if (m_proc) {
        AudioDeviceIOProcID proc = (AudioDeviceIOProcID)m_proc;
        // Stop waits for an IOProc in flight, so `this` is safe to drop after.
        AudioDeviceStop(m_device, proc);
        AudioDeviceDestroyIOProcID(m_device, proc);
        m_proc = nullptr;
    }
    m_device = 0;
    m_rate = 0.0;
    m_channels = 0;
}

void Capture::deliver(const void * bufferList)
{
    const AudioBufferList * list = (const AudioBufferList *)bufferList;
    if (list->mNumberBuffers == 0) return;
    unsigned chans = 0;
    for (UInt32 b = 0; b < list->mNumberBuffers; ++b) chans += list->mBuffers[b].mNumberChannels;
    if (chans == 0 || list->mBuffers[0].mNumberChannels == 0) return;
    const size_t frames = list->mBuffers[0].mDataByteSize / (sizeof(float) * list->mBuffers[0].mNumberChannels);
    const float scale = 1.0f / (float)chans;
    const size_t mask = m_ring.size() - 1;
    uint64_t w = m_write.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames; ++i) {
        float s = 0.0f;
        for (UInt32 b = 0; b < list->mNumberBuffers; ++b) {
            const AudioBuffer & buf = list->mBuffers[b];
            const unsigned nc = buf.mNumberChannels;
            if (nc == 0 || !buf.mData || (i + 1) * nc * sizeof(float) > buf.mDataByteSize) continue;
            const float * p = (const float *)buf.mData + i * nc;
            for (unsigned c = 0; c < nc; ++c) s += p[c];
        }
        m_ring[(size_t)w & mask] = s * scale;
        ++w;
    }
    m_write.store(w, std::memory_order_release);
}

bool Capture::read(std::vector<float> & out)
{
    if (!m_proc) return false;
    {
        const AudioObjectPropertyAddress a = address(kAudioDevicePropertyDeviceIsAlive);
        UInt32 alive = 1, size = sizeof(alive);
        if (AudioObjectGetPropertyData(m_device, &a, 0, nullptr, &size, &alive) != noErr || !alive)
            return false;
    }
    const uint64_t w = m_write.load(std::memory_order_acquire);
    const size_t cap = m_ring.size(), mask = cap - 1;
    // Fallen a whole ring behind: what was overwritten is lost, the rest is
    // still in order.
    if (w - m_read > cap) m_read = w - cap;
    for (; m_read < w; ++m_read) out.push_back(m_ring[(size_t)m_read & mask]);

    // A rate change is taken from the next read on; the analyser restarts its
    // resampler for it.
    const double r = nominalRate(m_device);
    if (r > 0.0) m_rate = r;
    return true;
}

} // namespace coreaudio_capture
