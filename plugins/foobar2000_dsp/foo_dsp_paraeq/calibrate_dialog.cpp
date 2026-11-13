/* ========================================
 *  foo_dsp_paraeq - room calibration window
 *
 *  Opened from the configuration dialog's Calibrate button. For as long as it
 *  is open the equaliser's output tap is open and a worker thread keeps the
 *  last 20 to 30 s of it (foobar2000's buffer plus 8 s, see historyFor()); Start opens the chosen microphone on the same
 *  thread and every two seconds of microphone signal is looked for in that
 *  history and, when found, added to the measurement. See paraeq_calibrate.h
 *  for the measurement itself.
 *
 *  The window polls the worker five times a second for a snapshot rather than
 *  being posted to, so the worker never waits on the UI and closing the
 *  window never races a message in flight.
 * ======================================== */

#include "stdafx.h"

#include "paraeq_calibrate.h"
#include "paraeq_core.h"
#include "paraeq_preset.h"
#include "resource.h"
#include "wasapi_capture.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using paraeq_calib::Peak;
using paraeq_calib::Result;

namespace {

//! The microphone last used, so the phone's virtual device is preselected
//! next time.
const GUID guid_cfg_device = { 0x7c3e9a51, 0x2d84, 0x4f6b, { 0x9b, 0x1e, 0x5a, 0x0c, 0x8d, 0x47, 0xe2, 0xf3 } };
cfg_string cfg_device(guid_cfg_device, "");

const UINT_PTR kTimer = 1;

//! The delay is taken to be at least foobar2000's buffer less this, which
//! leaves room for a buffer that is not quite full and loses at most a block
//! or two after a seek, while it refills.
const double kSeekMarginSec = 1.0;

class Worker {
public:
    struct Snapshot {
        Result       result;
        double       historySec = 0.0;
        unsigned     outRate = 0;
        bool         capturing = false;
        double       micRate = 0.0;
        unsigned     micChannels = 0;
        std::wstring error;
        unsigned     serial = 0;    //!< bumps when the result changes
    };

    //! `bufferSec` is foobar2000's output buffer, which sets how much output
    //! is kept to search.
    explicit Worker(double bufferSec)
        : m_historySec(paraeq_calib::historyFor(bufferSec)),
          m_minLagSec(bufferSec - kSeekMarginSec)
    {
        paraeq_calib::tap::open();
        m_thread = std::thread([this] { run(); });
    }

    ~Worker()
    {
        {
            std::lock_guard<std::mutex> lock(m_lock);
            m_quit = true;
        }
        m_wake.notify_all();
        m_thread.join();
        paraeq_calib::tap::close();
    }

    void start(const std::wstring & id)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_deviceId = id;
        m_want = true;
        ++m_request;
        m_snap.error.clear();
        m_wake.notify_all();
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(m_lock);
        m_want = false;
        ++m_request;
        m_wake.notify_all();
    }

    void snapshot(Snapshot & out)
    {
        std::lock_guard<std::mutex> lock(m_lock);
        out = m_snap;
    }

private:
    void run()
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // Created here rather than as a member: it holds several megabytes of
        // transform buffers, and only this thread touches it.
        std::unique_ptr<paraeq_calib::Analyzer> an(new paraeq_calib::Analyzer(m_historySec));
        an->setMinLag(m_minLagSec);
        wasapi_capture::Capture cap;
        std::vector<paraeq_calib::tap::Segment> segs;
        std::vector<float> mic;
        unsigned seen = 0;
        unsigned outRate = 0;

        for (;;) {
            bool want;
            unsigned request;
            std::wstring id;
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait_for(lock, std::chrono::milliseconds(20),
                                [this, seen] { return m_quit || m_request != seen; });
                if (m_quit) break;
                want = m_want;
                request = m_request;
                id = m_deviceId;
            }

            std::wstring error;
            bool changed = false;
            if (request != seen) {
                seen = request;
                cap.close();
                if (want) {
                    an->resetMeasurement();
                    changed = true;
                    if (!cap.open(id, error)) cap.close();
                }
            }

            segs.clear();
            paraeq_calib::tap::drain(segs);
            for (size_t i = 0; i < segs.size(); ++i) {
                an->feedOutput(segs[i].samples.data(), segs[i].samples.size(), segs[i].rate);
                outRate = segs[i].rate;
            }

            if (cap.isOpen()) {
                mic.clear();
                if (!cap.read(mic)) {
                    error = L"The microphone stopped delivering audio (unplugged or disabled?).";
                    cap.close();
                } else if (!mic.empty() && an->feedMic(mic.data(), mic.size(), cap.rate())) {
                    changed = true;
                }
            }

            std::lock_guard<std::mutex> lock(m_lock);
            if (changed) {
                m_snap.result = an->result();
                ++m_snap.serial;
            }
            m_snap.historySec  = an->historySec();
            m_snap.outRate     = outRate;
            m_snap.capturing   = cap.isOpen();
            m_snap.micRate     = cap.rate();
            m_snap.micChannels = cap.channels();
            if (!error.empty()) { m_snap.error = error; m_want = false; }
        }

        cap.close();
        an.reset();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    const double            m_historySec;
    const double            m_minLagSec;
    std::thread             m_thread;
    std::mutex              m_lock;
    std::condition_variable m_wake;
    bool         m_quit = false;
    bool         m_want = false;
    unsigned     m_request = 0;
    std::wstring m_deviceId;
    Snapshot     m_snap;
};

//! foobar2000's configured output buffer, in seconds; 0 if it cannot say.
//! That is the most the DSP chain runs ahead of what is heard, not how far
//! ahead it is now - after a seek or at the start of playback it is less -
//! so it sizes the history and is shown, and the search still covers all of it.
double output_buffer_sec()
{
    try {
        const double b = output_manager::get()->getCoreConfig().m_buffer_length;
        return (b > 0.0 && b < 60.0) ? b : 0.0;
    } catch (...) {
        return 0.0;
    }
}

struct cal_state {
    double                 bufferSec = 0.0;
    paraeq_calibrate_apply apply;
    void *                 ctx;
    std::unique_ptr<Worker> worker;
    std::vector<wasapi_capture::Device> devices;
    Worker::Snapshot snap;
    unsigned drawnSerial = ~0u;
    bool running = false;
    std::vector<std::wstring> choices;   //!< what the options list holds
};

std::wstring format(const wchar_t * fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring hz(double f)
{
    return f >= 1000.0 ? format(L"%.2f kHz", f / 1000.0) : format(L"%.0f Hz", f);
}

void fill_devices(HWND dlg, cal_state * st)
{
    const HWND combo = GetDlgItem(dlg, IDC_CAL_DEVICE);
    std::wstring defaultId;
    wasapi_capture::listDevices(st->devices, defaultId);

    const std::wstring remembered = pfc::stringcvt::string_wide_from_utf8(cfg_device.get()).get_ptr();
    int sel = -1, def = -1;
    for (size_t i = 0; i < st->devices.size(); ++i) {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)st->devices[i].name.c_str());
        if (st->devices[i].id == remembered) sel = (int)i;
        if (st->devices[i].id == defaultId) def = (int)i;
    }
    if (sel < 0) sel = def;
    if (sel < 0 && !st->devices.empty()) sel = 0;
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
    EnableWindow(GetDlgItem(dlg, IDC_CAL_START), !st->devices.empty());
}

void set_running(HWND dlg, cal_state * st, bool on)
{
    st->running = on;
    SetDlgItemTextW(dlg, IDC_CAL_START, on ? L"&Stop" : L"&Start");
    EnableWindow(GetDlgItem(dlg, IDC_CAL_DEVICE), !on);
}

void update_text(HWND dlg, cal_state * st)
{
    const Worker::Snapshot & s = st->snap;
    const Result & r = s.result;
    std::wstring status;

    if (s.historySec < 0.5)
        status = L"No output from Parametric EQ yet - start playback with it in the active DSP chain.";
    else
        status = format(L"Output: %.0f s kept, %u Hz", s.historySec, s.outRate);
    if (st->bufferSec > 0.0) status += format(L"%sfoobar2000 buffer %.1f s.", s.historySec < 0.5 ? L" " : L"; ", st->bufferSec);
    else if (s.historySec >= 0.5) status += L".";

    if (!s.error.empty()) {
        status += L"\r\n" + s.error;
    } else if (s.capturing) {
        status += format(L" Microphone: %.0f Hz, %u ch", s.micRate, s.micChannels);
        if (r.blocks > 0) status += format(L", %.0f dBFS%s.", r.micDb, r.micClipped ? L" - CLIPPING, turn it down" : L"");
        else status += L".";
        // First whether the microphone is lined up with the output at all;
        // nothing else means anything until it is.
        if (r.lockedLagSec >= 0.0)
            status += format(L"\r\nAligned: delay %.2f s, %d recent blocks agree.", r.lockedLagSec, r.agreeing);
        else if (r.matched > 0)
            status += L"\r\nAligning: a match, waiting for more to agree.";
        else if (r.blocks >= 8)
            status += L"\r\nNOT ALIGNED: the microphone does not hear this output. Is foobar2000 the one "
                      L"playing, through Parametric EQ, and loud enough at the microphone?";
        else if (r.blocks > 0)
            status += L"\r\nAligning: listening for the output in the microphone...";
        status += format(L"\r\nMeasured: %d of %d blocks (%.0f s)", r.matched, r.blocks,
                         r.matched * (double)paraeq_calib::kBlock / paraeq_calib::kRate);
        if (r.lagSec < 0.0 && r.blocks > 0 && r.lockedLagSec >= 0.0)
            status += L"; the last one was not - music too quiet, talking, or a pause?";
        else status += L".";
    } else if (r.blocks > 0) {
        status += format(L"\r\nStopped. Measured %d of %d blocks.", r.matched, r.blocks);
    }
    SetDlgItemTextW(dlg, IDC_CAL_STATUS, status.c_str());

    std::wstring result;
    if (r.ready() && r.sure) {
        const Peak & p = r.option[0];
        result = format(L"Suggested Reverb cut: %s, Q %.1f, %.1f dB - the room's main bump, %.1f dB tall.",
                        hz(p.frequency).c_str(), p.q, p.gainDb, p.heightDb);
        if (r.peaks > 1) {
            result += L"\r\nSmaller bumps:";
            for (int i = 1; i < r.peaks; ++i)
                result += format(L"%s %s +%.1f dB", i > 1 ? L"," : L"", hz(r.peak[i].frequency).c_str(), r.peak[i].heightDb);
        }
    } else if (r.ready()) {
        result = format(L"Not sure which bump is the room's main one: %d candidates, tallest first. "
                        L"Try each by ear - pick one, Apply, listen.", r.options);
    } else if (r.matched >= paraeq_calib::kMinBlocks) {
        result = L"No bump between 200 Hz and 3 kHz stands out: this room needs no Reverb cut.";
    } else if (s.capturing) {
        const int need = (int)((paraeq_calib::kMinBlocks - r.matched) * (double)paraeq_calib::kBlock / paraeq_calib::kRate + 0.5);
        result = format(L"Keep walking: about %d s more of matched music before a suggestion.", need);
    }
    SetDlgItemTextW(dlg, IDC_CAL_RESULT, result.c_str());

    // The options list. Refilled only when it changes, keeping the user's
    // pick by position, so a choice is not lost every time a block lands.
    std::vector<std::wstring> items;
    if (r.ready())
        for (int i = 0; i < r.options; ++i)
            items.push_back(format(L"%d: %s, Q %.1f, %.1f dB", i + 1, hz(r.option[i].frequency).c_str(),
                                   r.option[i].q, r.option[i].gainDb));
    const HWND combo = GetDlgItem(dlg, IDC_CAL_CHOICE);
    if (items != st->choices) {
        int sel = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < items.size(); ++i) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)items[i].c_str());
        if (sel < 0 || sel >= (int)items.size()) sel = 0;
        if (!items.empty()) SendMessageW(combo, CB_SETCURSEL, (WPARAM)sel, 0);
        st->choices.swap(items);
        InvalidateRect(GetDlgItem(dlg, IDC_CAL_PLOT), nullptr, FALSE);
    }
    EnableWindow(combo, r.ready());
    EnableWindow(GetDlgItem(dlg, IDC_CAL_APPLY), r.ready());
}

//! The option the list has selected, or null.
const Peak * chosen(HWND dlg, const cal_state * st)
{
    const Result & r = st->snap.result;
    if (!r.ready()) return nullptr;
    int sel = (int)SendDlgItemMessageW(dlg, IDC_CAL_CHOICE, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= r.options) sel = 0;
    return &r.option[sel];
}

// ---------------------------------------------------------------------------
// The plot
// ---------------------------------------------------------------------------

const double kPlotLoHz = paraeq_calib::kGridLoHz;
const double kPlotHiHz = paraeq_calib::kGridHiHz;
const double kPlotDb = 15.0;

void draw_plot(HWND dlg, const DRAWITEMSTRUCT * di, cal_state * st, HFONT font)
{
    const RECT & full = di->rcItem;
    const int w = full.right - full.left, h = full.bottom - full.top;
    if (w <= 0 || h <= 0) return;

    HDC dc = CreateCompatibleDC(di->hDC);
    HBITMAP bmp = CreateCompatibleBitmap(di->hDC, w, h);
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    HGDIOBJ oldFont = SelectObject(dc, font);

    const COLORREF bg = GetSysColor(COLOR_WINDOW);
    const COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
    const COLORREF grid = RGB((GetRValue(bg) * 3 + GetRValue(fg)) / 4,
                              (GetGValue(bg) * 3 + GetGValue(fg)) / 4,
                              (GetBValue(bg) * 3 + GetBValue(fg)) / 4);
    HBRUSH bgBrush = CreateSolidBrush(bg);
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, bgBrush);
    DeleteObject(bgBrush);
    SetBkMode(dc, TRANSPARENT);

    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int padL = tm.tmAveCharWidth * 5, padB = tm.tmHeight + 2, padT = 4, padR = 6;
    const RECT pr = { padL, padT, w - padR, h - padB };
    const int pw = pr.right - pr.left, ph = pr.bottom - pr.top;

    auto xOf = [&](double f) {
        return pr.left + (int)(pw * log(f / kPlotLoHz) / log(kPlotHiHz / kPlotLoHz) + 0.5);
    };
    auto yOf = [&](double db) {
        db = std::max(-kPlotDb, std::min(kPlotDb, db));
        return pr.top + (int)(ph * (kPlotDb - db) / (2.0 * kPlotDb) + 0.5);
    };

    // The band's range, shaded.
    {
        // A tenth of a blue over the background, which reads in light and
        // dark system colours alike.
        const COLORREF shade = RGB((GetRValue(bg) * 9 + 70) / 10,
                                   (GetGValue(bg) * 9 + 130) / 10,
                                   (GetBValue(bg) * 9 + 220) / 10);
        HBRUSH b = CreateSolidBrush(shade);
        RECT band = { xOf(paraeq_calib::kSearchLoHz), pr.top, xOf(paraeq_calib::kSearchHiHz), pr.bottom };
        FillRect(dc, &band, b);
        DeleteObject(b);
    }

    HPEN gridPen = CreatePen(PS_SOLID, 1, grid);
    HGDIOBJ oldPen = SelectObject(dc, gridPen);
    SetTextColor(dc, fg);
    const double ticks[] = { 100, 200, 300, 500, 1000, 2000, 3000 };
    for (double f : ticks) {
        const int x = xOf(f);
        MoveToEx(dc, x, pr.top, nullptr);
        LineTo(dc, x, pr.bottom);
        const wchar_t * label = f >= 1000 ? (f == 1000 ? L"1k" : f == 2000 ? L"2k" : L"3k")
                                          : (f == 100 ? L"100" : f == 200 ? L"200" : f == 300 ? L"300" : L"500");
        RECT tr = { x - 20, pr.bottom + 1, x + 20, h };
        DrawTextW(dc, label, -1, &tr, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
    for (int db = -10; db <= 10; db += 5) {
        const int y = yOf(db);
        MoveToEx(dc, pr.left, y, nullptr);
        LineTo(dc, pr.right, y);
        wchar_t t[16];
        wsprintfW(t, db > 0 ? L"+%d" : L"%d", db);
        RECT tr = { 0, y - tm.tmHeight / 2, pr.left - 3, y + tm.tmHeight };
        DrawTextW(dc, t, -1, &tr, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }

    const Result & r = st->snap.result;
    const size_t n = r.frequency.size();
    if (n > 1) {
        auto curve = [&](COLORREF c, int width, const double * extra, const std::vector<double> & v) {
            HPEN pen = CreatePen(PS_SOLID, width, c);
            HGDIOBJ op = SelectObject(dc, pen);
            for (size_t i = 0; i < n; ++i) {
                const double db = v[i] + (extra ? extra[i] : 0.0);
                const int x = xOf(r.frequency[i]), y = yOf(db);
                if (i == 0) MoveToEx(dc, x, y, nullptr); else LineTo(dc, x, y);
            }
            SelectObject(dc, op);
            DeleteObject(pen);
        };

        curve(RGB(150, 150, 150), 1, nullptr, r.trendDb);
        if (const Peak * sel = chosen(dlg, st)) {
            // What the room would sound like with the selected option applied.
            const double fs = 48000.0;
            const paraeq::Biquad b = paraeq::design(paraeq::kPeaking, sel->frequency, sel->q, sel->gainDb, fs);
            std::vector<double> bell(n);
            for (size_t i = 0; i < n; ++i) bell[i] = paraeq::magnitudeDb(b, r.frequency[i], fs);
            curve(RGB(40, 160, 70), 1, bell.data(), r.responseDb);

            // Every option marked, the selected one solid.
            for (int k = 0; k < r.options; ++k) {
                const bool on = &r.option[k] == sel;
                HPEN mark = CreatePen(on ? PS_SOLID : PS_DOT, 1, RGB(200, 40, 40));
                HGDIOBJ op = SelectObject(dc, mark);
                const int x = xOf(r.option[k].frequency);
                MoveToEx(dc, x, pr.top, nullptr);
                LineTo(dc, x, pr.bottom);
                SelectObject(dc, op);
                DeleteObject(mark);
                wchar_t t[4];
                wsprintfW(t, L"%d", k + 1);
                RECT tr = { x + 2, pr.bottom - tm.tmHeight - 2, x + 30, pr.bottom };
                SetTextColor(dc, RGB(200, 40, 40));
                DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        curve(RGB(30, 90, 200), 2, nullptr, r.responseDb);
    } else {
        RECT tr = pr;
        SetTextColor(dc, grid);
        DrawTextW(dc, L"The room's response appears here after a few seconds of matched music.", -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Legend.
    {
        SetTextColor(dc, fg);
        const wchar_t * text = L"blue: room   grey: trend   green: with the cut";
        RECT tr = { pr.left + 4, pr.top + 2, pr.right - 4, pr.top + tm.tmHeight + 2 };
        DrawTextW(dc, text, -1, &tr, DT_RIGHT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }

    HBRUSH frame = CreateSolidBrush(grid);
    RECT fr = pr;
    FrameRect(dc, &fr, frame);
    DeleteObject(frame);

    BitBlt(di->hDC, full.left, full.top, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------

INT_PTR CALLBACK cal_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    cal_state * st = (cal_state *)GetWindowLongPtr(dlg, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG:
        st = (cal_state *)lp;
        SetWindowLongPtr(dlg, GWLP_USERDATA, (LONG_PTR)st);
        st->bufferSec = output_buffer_sec();
        st->worker.reset(new Worker(st->bufferSec));
        fill_devices(dlg, st);
        update_text(dlg, st);
        SetTimer(dlg, kTimer, 200, nullptr);
        return TRUE;

    case WM_TIMER:
        if (st && wp == kTimer && st->worker) {
            st->worker->snapshot(st->snap);
            if (st->running && !st->snap.capturing && !st->snap.error.empty()) set_running(dlg, st, false);
            update_text(dlg, st);
            if (st->snap.serial != st->drawnSerial) {
                st->drawnSerial = st->snap.serial;
                InvalidateRect(GetDlgItem(dlg, IDC_CAL_PLOT), nullptr, FALSE);
            }
            return TRUE;
        }
        break;

    case WM_DRAWITEM:
        if (st && wp == IDC_CAL_PLOT) {
            draw_plot(dlg, (const DRAWITEMSTRUCT *)lp, st, (HFONT)SendMessage(dlg, WM_GETFONT, 0, 0));
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (st == nullptr) break;
        switch (LOWORD(wp)) {
        case IDC_CAL_START:
            if (HIWORD(wp) == BN_CLICKED) {
                if (st->running) {
                    st->worker->stop();
                    set_running(dlg, st, false);
                } else {
                    const int sel = (int)SendDlgItemMessageW(dlg, IDC_CAL_DEVICE, CB_GETCURSEL, 0, 0);
                    if (sel < 0 || sel >= (int)st->devices.size()) return TRUE;
                    cfg_device = pfc::stringcvt::string_utf8_from_wide(st->devices[(size_t)sel].id.c_str()).get_ptr();
                    st->worker->start(st->devices[(size_t)sel].id);
                    set_running(dlg, st, true);
                }
                return TRUE;
            }
            break;
        case IDC_CAL_APPLY:
            if (HIWORD(wp) == BN_CLICKED) {
                if (const Peak * p = chosen(dlg, st)) st->apply(st->ctx, p->frequency, p->q, p->gainDb);
                return TRUE;
            }
            break;
        case IDC_CAL_CHOICE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                InvalidateRect(GetDlgItem(dlg, IDC_CAL_PLOT), nullptr, FALSE);
                return TRUE;
            }
            break;
        case IDCANCEL:
            if (HIWORD(wp) == BN_CLICKED) { EndDialog(dlg, IDCANCEL); return TRUE; }
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;

    case WM_DESTROY:
        if (st) {
            KillTimer(dlg, kTimer);
            st->worker.reset();     // joins the thread and closes the tap
        }
        return FALSE;
    }
    return FALSE;
}

} // anonymous namespace


void paraeq_calibrate_popup(HWND parent, paraeq_calibrate_apply apply, void * ctx)
{
    // The UI thread normally has COM already; this only makes sure of it for
    // the device list.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    cal_state st;
    st.apply = apply;
    st.ctx = ctx;
    DialogBoxParam(core_api::get_my_instance(), MAKEINTRESOURCE(IDD_PARAEQ_CALIBRATE),
                   parent, cal_proc, (LPARAM)&st);

    if (SUCCEEDED(co)) CoUninitialize();
}
