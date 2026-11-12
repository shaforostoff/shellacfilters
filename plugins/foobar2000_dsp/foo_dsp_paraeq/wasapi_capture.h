/* ========================================
 *  foo_dsp_paraeq - microphone input for the room calibration
 *
 *  WASAPI shared mode, polled, in the device's own mix format. Shared mode
 *  because the device may be a virtual one fed by a phone app, which will not
 *  give up exclusive access, and the mix format because it is the one format
 *  shared mode is guaranteed to accept: the analyser takes any rate and the
 *  channels are averaged here. Polled because the analyser runs on the same
 *  thread every 20 ms anyway and a one-second device buffer leaves it plenty
 *  of slack.
 *
 *  COM must be initialised on the calling thread. Windows Vista and later,
 *  nothing newer than Windows 7 used. No foobar2000 dependency.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_WASAPI_CAPTURE_H
#define FOO_DSP_PARAEQ_WASAPI_CAPTURE_H

#include <string>
#include <vector>

struct IAudioClient;
struct IAudioCaptureClient;

namespace wasapi_capture {

struct Device {
    std::wstring id;
    std::wstring name;
};

//! Active capture endpoints, and which of them is the default for recording.
//! Returns false if the enumerator could not be created.
//! `render` lists playback endpoints instead, for the loopback test tool.
bool listDevices(std::vector<Device> & out, std::wstring & defaultId, bool render = false);

class Capture {
public:
    Capture() {}
    ~Capture() { close(); }
    Capture(const Capture &) = delete;
    void operator=(const Capture &) = delete;

    //! Opens and starts the endpoint. On failure, `error` says why.
    bool open(const std::wstring & id, std::wstring & error);
    void close();
    bool isOpen() const { return m_capture != nullptr; }

    double   rate() const { return m_rate; }
    unsigned channels() const { return m_channels; }

    //! Everything captured since the last call, averaged to mono, appended to
    //! `out`. False if the device went away.
    bool read(std::vector<float> & out);

private:
    IAudioClient *        m_client  = nullptr;
    IAudioCaptureClient * m_capture = nullptr;
    double   m_rate = 0.0;
    unsigned m_channels = 0;
    unsigned m_bytesPerSample = 0;
    bool     m_float = false;
};

} // namespace wasapi_capture

#endif // FOO_DSP_PARAEQ_WASAPI_CAPTURE_H
