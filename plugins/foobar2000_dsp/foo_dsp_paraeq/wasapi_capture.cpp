/* ========================================
 *  foo_dsp_paraeq - microphone input for the room calibration
 *
 *  See wasapi_capture.h.
 * ======================================== */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propidl.h>

#include <stdint.h>
#include <string.h>

#include "wasapi_capture.h"

namespace wasapi_capture {

namespace {

//! PKEY_Device_FriendlyName, spelled out so that nothing needs INITGUID or a
//! property-system import library.
const PROPERTYKEY kFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

template<typename T> void release(T *& p) { if (p) { p->Release(); p = nullptr; } }

std::wstring hresultText(const wchar_t * what, HRESULT hr)
{
    wchar_t buf[160];
    const wchar_t * reason = L"";
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:   reason = L" (device unplugged or disabled)"; break;
    case AUDCLNT_E_DEVICE_IN_USE:        reason = L" (device in exclusive use)"; break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:   reason = L" (format not supported)"; break;
    case E_ACCESSDENIED:                 reason = L" (microphone access denied in Windows privacy settings)"; break;
    default: break;
    }
    wsprintfW(buf, L"%s failed, 0x%08X%s", what, (unsigned)hr, reason);
    return buf;
}

} // anonymous namespace

bool listDevices(std::vector<Device> & out, std::wstring & defaultId, bool render)
{
    out.clear();
    defaultId.clear();
    IMMDeviceEnumerator * en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void **)&en)))
        return false;

    IMMDevice * def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(render ? eRender : eCapture, eConsole, &def))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) { defaultId = id; CoTaskMemFree(id); }
        release(def);
    }

    IMMDeviceCollection * col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(render ? eRender : eCapture, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice * dev = nullptr;
            if (FAILED(col->Item(i, &dev))) continue;
            Device d;
            LPWSTR id = nullptr;
            if (SUCCEEDED(dev->GetId(&id))) { d.id = id; CoTaskMemFree(id); }
            IPropertyStore * props = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                    d.name = v.pwszVal;
                PropVariantClear(&v);
                release(props);
            }
            if (d.name.empty()) d.name = d.id;
            if (!d.id.empty()) out.push_back(d);
            release(dev);
        }
        release(col);
    }
    release(en);
    return true;
}

bool Capture::open(const std::wstring & id, std::wstring & error)
{
    close();
    IMMDeviceEnumerator * en = nullptr;
    IMMDevice * dev = nullptr;
    WAVEFORMATEX * fmt = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void **)&en);
    if (FAILED(hr)) { error = hresultText(L"Creating the device enumerator", hr); return false; }

    hr = en->GetDevice(id.c_str(), &dev);
    release(en);
    if (FAILED(hr)) { error = hresultText(L"Opening the device", hr); return false; }

    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&m_client);
    release(dev);
    if (FAILED(hr)) { error = hresultText(L"Activating the device", hr); return false; }

    hr = m_client->GetMixFormat(&fmt);
    if (FAILED(hr)) { error = hresultText(L"Reading the device format", hr); close(); return false; }

    // The mix format is float in practice; integer PCM is handled in case a
    // driver says otherwise. Extensible formats carry the real tag in the
    // first field of their subformat GUID.
    WORD tag = fmt->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22)
        tag = (WORD)((WAVEFORMATEXTENSIBLE *)fmt)->SubFormat.Data1;
    m_float = (tag == WAVE_FORMAT_IEEE_FLOAT);
    m_bytesPerSample = fmt->wBitsPerSample / 8;
    m_channels = fmt->nChannels;
    m_rate = (double)fmt->nSamplesPerSec;
    const bool ok = m_channels > 0 &&
        ((m_float && m_bytesPerSample == 4) ||
         (tag == WAVE_FORMAT_PCM && (m_bytesPerSample == 2 || m_bytesPerSample == 3 || m_bytesPerSample == 4)));
    if (!ok) {
        CoTaskMemFree(fmt);
        error = L"The device's format is not one this reads (float or 16/24/32-bit PCM)";
        close();
        return false;
    }

    // One second of buffer: the reader polls every 20 ms but the analysis can
    // hold it up for tens.
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, fmt, nullptr);
    CoTaskMemFree(fmt);
    if (FAILED(hr)) { error = hresultText(L"Initialising the device", hr); close(); return false; }

    hr = m_client->GetService(__uuidof(IAudioCaptureClient), (void **)&m_capture);
    if (FAILED(hr)) { error = hresultText(L"Getting the capture service", hr); close(); return false; }

    hr = m_client->Start();
    if (FAILED(hr)) { error = hresultText(L"Starting the device", hr); close(); return false; }
    return true;
}

void Capture::close()
{
    if (m_client) m_client->Stop();
    release(m_capture);
    release(m_client);
    m_rate = 0.0;
    m_channels = 0;
}

bool Capture::read(std::vector<float> & out)
{
    if (!m_capture) return false;
    for (;;) {
        UINT32 packet = 0;
        HRESULT hr = m_capture->GetNextPacketSize(&packet);
        if (FAILED(hr)) return false;
        if (packet == 0) return true;

        BYTE * data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        hr = m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(hr)) return false;

        const size_t base = out.size();
        out.resize(base + frames, 0.0f);
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
            const float scale = 1.0f / (float)m_channels;
            const unsigned stride = m_channels * m_bytesPerSample;
            for (UINT32 f = 0; f < frames; ++f) {
                const BYTE * p = data + (size_t)f * stride;
                float s = 0.0f;
                for (unsigned c = 0; c < m_channels; ++c, p += m_bytesPerSample) {
                    if (m_float) {
                        float v;
                        memcpy(&v, p, 4);
                        s += v;
                    } else if (m_bytesPerSample == 2) {
                        s += (float)(int16_t)(p[0] | (p[1] << 8)) * (1.0f / 32768.0f);
                    } else if (m_bytesPerSample == 3) {
                        const int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
                        s += (float)v * (1.0f / 2147483648.0f);
                    } else {
                        int32_t v;
                        memcpy(&v, p, 4);
                        s += (float)v * (1.0f / 2147483648.0f);
                    }
                }
                out[base + f] = s * scale;
            }
        }
        m_capture->ReleaseBuffer(frames);
    }
}

} // namespace wasapi_capture
