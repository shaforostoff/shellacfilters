/* ========================================
 *  foo_dsp_paraeq - microphone input for the room calibration, macOS
 *
 *  The counterpart of wasapi_capture.h. A HAL IOProc on the device itself, in
 *  the device's own format and rate, which on the HAL's side of the device is
 *  always 32-bit float: no converter, no audio unit, nothing between the
 *  device and the analyser that could resample or delay. The IOProc averages
 *  every input channel to mono into a lock-free ring, and read() takes what
 *  is there - polled, as on Windows, because the analyser runs every 20 ms on
 *  the worker anyway and the ring holds several seconds.
 *
 *  A device is named by its UID, which, unlike its AudioDeviceID, survives a
 *  reboot and replugging, so the one last used can be found again. No
 *  foobar2000 dependency. The process needs microphone access (TCC); without
 *  it the device delivers silence, so ask first - see calibrate_window_mac.mm.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_COREAUDIO_CAPTURE_H
#define FOO_DSP_PARAEQ_COREAUDIO_CAPTURE_H

#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

namespace coreaudio_capture {

struct Device {
    std::string id;      //!< the device's UID
    std::string name;    //!< UTF-8
};

//! Devices with at least one input channel, and the UID of the default input
//! device. Returns false if the device list could not be read.
bool listDevices(std::vector<Device> & out, std::string & defaultId);

class Capture {
public:
    Capture() {}
    ~Capture() { close(); }
    Capture(const Capture &) = delete;
    void operator=(const Capture &) = delete;

    //! Opens and starts the device. On failure, `error` says why.
    bool open(const std::string & id, std::string & error);
    void close();
    bool isOpen() const { return m_proc != nullptr; }

    double   rate() const { return m_rate; }
    unsigned channels() const { return m_channels; }

    //! Everything captured since the last call, mono, appended to `out`.
    //! False if the device went away. Picks up a change of the device's rate,
    //! which rate() then reports.
    bool read(std::vector<float> & out);

    //! The IOProc. Public only so the C callback can reach it.
    void deliver(const void * bufferList);

private:
    enum { kRingOrder = 19 };   //!< 2^19 samples: 10.9 s at 48 kHz, 2.7 s at 192
    uint32_t m_device = 0;
    void *   m_proc = nullptr;  //!< AudioDeviceIOProcID
    double   m_rate = 0.0;
    unsigned m_channels = 0;
    std::vector<float>    m_ring;
    std::atomic<uint64_t> m_write { 0 };   //!< written by the IOProc
    uint64_t              m_read = 0;      //!< the reader's own
};

} // namespace coreaudio_capture

#endif // FOO_DSP_PARAEQ_COREAUDIO_CAPTURE_H
