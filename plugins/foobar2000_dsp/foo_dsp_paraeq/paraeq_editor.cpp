/* ========================================
 *  foo_dsp_paraeq - the curve editor
 * ======================================== */

#include "stdafx.h"

#include "paraeq_editor.h"

#include <math.h>
#include <stdio.h>

using namespace paraeq;

namespace paraeq_editor {

namespace {

// ---------------------------------------------------------------------------
// The bands, as the interface sees them
// ---------------------------------------------------------------------------

enum { kHP = 0, kLF, kLMF, kHMF, kHF, kOut, kBandCount };

//! Rows of the readout strip. A band that has nothing to say on a row writes a
//! dash there rather than the row being missing, so the columns line up and a
//! glance down one of them compares like with like.
enum { kRowFreq = 0, kRowGain, kRowShape, kRowCount };

struct BandSpec {
    const wchar_t * name;
    float fMin, fMax;       //!< both zero when the band has no frequency
    bool  hasGain;
    bool  hasQ;
    bool  hasShelf;         //!< shelf that can be switched to a bell
};

const BandSpec kSpec[kBandCount] = {
    { L"HP",  kHpFreqMin,  kHpFreqMax,  false, false, false },
    { L"LF",  kLfFreqMin,  kLfFreqMax,  true,  false, true  },
    { L"LMF", kLmfFreqMin, kLmfFreqMax, true,  true,  false },
    { L"HMF", kHmfFreqMin, kHmfFreqMax, true,  true,  false },
    { L"HF",  kHfFreqMin,  kHfFreqMax,  true,  false, true  },
    { L"Out", 0.0f,        0.0f,        true,  false, false },
};

const wchar_t * const kRowName[kRowCount] = { L"Freq", L"Gain", L"Shape" };

double bandFreq(const Params & p, int b) {
    switch (b) {
    case kHP:  return p.hpFrequency;
    case kLF:  return p.lfFrequency;
    case kLMF: return p.lmfFrequency;
    case kHMF: return p.hmfFrequency;
    case kHF:  return p.hfFrequency;
    default:   return 0.0;
    }
}

void setBandFreq(Params & p, int b, double hz) {
    switch (b) {
    case kHP:  p.hpFrequency  = (float)hz; break;
    case kLF:  p.lfFrequency  = (float)hz; break;
    case kLMF: p.lmfFrequency = (float)hz; break;
    case kHMF: p.hmfFrequency = (float)hz; break;
    case kHF:  p.hfFrequency  = (float)hz; break;
    default: break;
    }
}

double bandGain(const Params & p, int b) {
    switch (b) {
    case kLF:  return p.lfGain;
    case kLMF: return p.lmfGain;
    case kHMF: return p.hmfGain;
    case kHF:  return p.hfGain;
    case kOut: return p.outputGain;
    default:   return 0.0;
    }
}

void setBandGain(Params & p, int b, double db) {
    switch (b) {
    case kLF:  p.lfGain     = (float)db; break;
    case kLMF: p.lmfGain    = (float)db; break;
    case kHMF: p.hmfGain    = (float)db; break;
    case kHF:  p.hfGain     = (float)db; break;
    case kOut: p.outputGain = (float)db; break;
    default: break;
    }
}

double bandQ(const Params & p, int b) {
    if (b == kLMF) return p.lmfQ;
    if (b == kHMF) return p.hmfQ;
    return 0.0;
}

void setBandQ(Params & p, int b, double q) {
    if (b == kLMF) p.lmfQ = (float)q;
    if (b == kHMF) p.hmfQ = (float)q;
}

//! The section this band contributes, for the faint overlay drawn under the
//! pointer. kUnity for the output trim, which is a scalar rather than a filter.
Biquad bandSection(const Config & cfg, int b) {
    switch (b) {
    // The high-pass is two sections at 24 dB/oct; the overlay wants the pair,
    // and a cascade of two is not a Biquad, so the caller draws it as two.
    case kHP:  return cfg.stage[kStageHighPass1];
    case kLF:  return cfg.stage[kStageLowShelf];
    case kLMF: return cfg.stage[kStageLowMid];
    case kHMF: return cfg.stage[kStageHighMid];
    case kHF:  return cfg.stage[kStageHighShelf];
    default:   return Biquad();
    }
}

// ---------------------------------------------------------------------------
// Plot geometry
// ---------------------------------------------------------------------------

//! The axis. Not 20 Hz to 20 kHz, which is the familiar one and is wrong here
//! by a little at each end: the high-pass reaches down to 16 Hz and the high
//! shelf up to 16 kHz, and a control at the end of its travel has to be visible
//! at the end of its travel or it cannot be got back. So the plot starts below
//! the lowest thing on it and ends above the highest, and the decade grid
//! inside it is unchanged.
const double kAxisLoHz = 14.0;
const double kAxisHiHz = 22000.0;

//! Half the vertical range. The controls stop at 20 dB, so 24 leaves a band at
//! full boost visibly short of the ceiling. Bands do sum past this - four at
//! once can - and a curve that leaves the box is clipped, which reads correctly
//! as having gone too far.
const double kAxisDb = 24.0;

const double kGridHz[] = { 20, 30, 50, 100, 200, 300, 500, 1000,
                           2000, 3000, 5000, 10000, 20000 };
const int    kGridHzCount = (int)(sizeof(kGridHz) / sizeof(kGridHz[0]));

//! Labelled lines. The decades only: a grid with a number on every line stops
//! being a grid.
const double kLabelHz[]  = { 100, 1000, 10000 };
const wchar_t * const kLabelHzText[] = { L"100", L"1k", L"10k" };
const int    kLabelHzCount = 3;

const double kGridDb[] = { -18, -12, -6, 0, 6, 12, 18 };
const int    kGridDbCount = (int)(sizeof(kGridDb) / sizeof(kGridDb[0]));

//! A control move is pushed at most this often while one is in progress, with
//! one more push when it ends. Every push rewrites the whole DSP chain and
//! wakes every dsp_config_callback in the process, so a drag reporting each of
//! its several hundred mouse messages would be wasteful; 40 a second is faster
//! than the 20 ms the audio takes to glide there anyway.
const UINT kPushIntervalMs = 25;

enum { kTimerPush = 1 };

// Footer buttons, left to right. The engage button only appears when the
// equaliser is not in the DSP chain, and then it comes first.
enum { kBtnEngage = 0, kBtnBypass, kBtnFlatten, kBtnReset, kBtnCount };

const wchar_t * const kBtnText[kBtnCount] = {
    L"Add to DSP chain", L"Bypass", L"Flatten", L"Reset"
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

inline double clampd(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

//! `t` of the way from a to b, per channel. Every derived colour comes from
//! here, which is what keeps the whole editor correct in dark mode without
//! knowing that dark mode exists: the host hands over a background and a text
//! colour and everything else is between them.
COLORREF blend(COLORREF a, COLORREF b, double t) {
    const double u = 1.0 - t;
    const int r = (int)(GetRValue(a) * u + GetRValue(b) * t + 0.5);
    const int g = (int)(GetGValue(a) * u + GetGValue(b) * t + 0.5);
    const int bl = (int)(GetBValue(a) * u + GetBValue(b) * t + 0.5);
    return RGB(clampi(r, 0, 255), clampi(g, 0, 255), clampi(bl, 0, 255));
}

void formatHz(wchar_t * out, size_t n, double hz) {
    if (hz >= 10000.0)     _snwprintf_s(out, n, _TRUNCATE, L"%.1fk", hz / 1000.0);
    else if (hz >= 1000.0) _snwprintf_s(out, n, _TRUNCATE, L"%.2fk", hz / 1000.0);
    else                   _snwprintf_s(out, n, _TRUNCATE, L"%.0f",  hz);
}

void drawText(HDC dc, const RECT & rc, const wchar_t * text, UINT flags, COLORREF col) {
    RECT r = rc;
    SetTextColor(dc, col);
    DrawTextW(dc, text, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

void fillRect(HDC dc, const RECT & rc, COLORREF col) {
    const HBRUSH br = CreateSolidBrush(col);
    if (br == NULL) return;
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

void frameRect(HDC dc, const RECT & rc, COLORREF col) {
    const HBRUSH br = CreateSolidBrush(col);
    if (br == NULL) return;
    FrameRect(dc, &rc, br);
    DeleteObject(br);
}

const wchar_t * const kClassName = L"foo_dsp_paraeq_editor";
bool g_classRegistered = false;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

bool registerClass()
{
    // Registered once and never unregistered, which is safe because
    // foobar2000 does not unload component DLLs during a session - removing a
    // component takes a restart. A class whose window procedure had gone with
    // an unloaded module would be the thing to worry about otherwise.
    if (g_classRegistered) return true;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    // CS_DBLCLKS for the double-click that zeroes a band; no background brush
    // because every pixel is painted from the double buffer and letting the
    // class erase first would flicker.
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = &Editor::wndProc;
    wc.hInstance     = core_api::get_my_instance();
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = kClassName;

    g_classRegistered = (RegisterClassExW(&wc) != 0)
                     || (GetLastError() == ERROR_CLASS_ALREADY_EXISTS);
    return g_classRegistered;
}


Editor::Editor(Host & host)
    : m_host(host)
{
    m_params    = Params::defaults();
    m_dragStart = m_params;
    m_colBack = GetSysColor(COLOR_WINDOW);
    m_colText = GetSysColor(COLOR_WINDOWTEXT);
}


Editor::~Editor()
{
    if (m_wnd != NULL) {
        // Getting here means the host released the instance without destroying
        // the window first, which ui_element_instance explicitly allows. A
        // pending push is dropped rather than sent: an Editor is a member of
        // whatever implements Host, so by the time this runs the host may be
        // part way through its own destructor and in no state to be called.
        // The window-first path - WM_DESTROY below - still flushes, and it is
        // the one a panel being closed takes.
        if (m_pushTimer) KillTimer(m_wnd, kTimerPush);
        m_pushTimer = false;
        m_pushDirty = false;

        // Dropping the back pointer first: DestroyWindow sends WM_DESTROY
        // synchronously and the handler must not reach a half-destroyed object.
        SetWindowLongPtr(m_wnd, GWLP_USERDATA, 0);
        const HWND wnd = m_wnd;
        m_wnd = NULL;
        DestroyWindow(wnd);
    }
    releaseBuffer();
}


HWND Editor::create(HWND parent, const RECT & rc, UINT id, const Params & initial)
{
    if (!registerClass()) return NULL;

    m_params = initial;
    m_params.sanitize();
    m_engaged = m_host.editorEngaged();

    m_wnd = CreateWindowExW(
        0, kClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        parent, (HMENU)(UINT_PTR)id, core_api::get_my_instance(), this);

    return m_wnd;
}


void Editor::setParams(const Params & p)
{
    // A push of ours comes back round through the host a moment after it went
    // out. Taking it mid-drag would put the handle back where the last push
    // left it rather than where the pointer is now.
    if (m_dragging) return;

    Params next = p;
    next.sanitize();
    if (next == m_params) return;

    m_params = next;
    markCurveStale();
    if (m_wnd) InvalidateRect(m_wnd, NULL, FALSE);
}


void Editor::refreshTheme()
{
    theme();
    if (m_wnd) {
        layout();
        rebuildTable();
        InvalidateRect(m_wnd, NULL, FALSE);
    }
}


void Editor::refreshEngaged()
{
    const bool now = m_host.editorEngaged();
    if (now == m_engaged) return;
    m_engaged = now;
    // The engage button comes and goes with this, and the ones after it shift
    // along, so the footer has to be laid out again rather than just redrawn.
    layout();
    if (m_wnd) InvalidateRect(m_wnd, NULL, FALSE);
}


void Editor::setSampleRate(double rate)
{
    if (!(rate > 8000.0) || !(rate < 1e7)) rate = 44100.0;
    if (rate == m_sampleRate) return;
    m_sampleRate = rate;
    rebuildTable();
    if (m_wnd) InvalidateRect(m_wnd, NULL, FALSE);
}


bool Editor::editable() const
{
    return !m_host.editorLayoutEditMode();
}

// ---------------------------------------------------------------------------
// Theme and geometry
// ---------------------------------------------------------------------------

void Editor::theme()
{
    if (!m_host.editorColor(ui_color_background, m_colBack)) {
        m_colBack = GetSysColor(COLOR_WINDOW);
    }
    if (!m_host.editorColor(ui_color_text, m_colText)) {
        m_colText = GetSysColor(COLOR_WINDOWTEXT);
    }

    // Derived rather than asked for, so the set stays coherent whatever the two
    // it is derived from turn out to be.
    m_colGrid  = blend(m_colBack, m_colText, 0.18);
    m_colDim   = blend(m_colBack, m_colText, 0.50);
    m_colFill  = blend(m_colBack, m_colText, 0.10);
    m_colCurve = m_colText;

    if (!m_host.editorColor(ui_color_selection, m_colSel)) {
        m_colSel = GetSysColor(COLOR_HIGHLIGHT);
    }
    // A host may hand back a selection colour equal to the background - some
    // themes do, for selections drawn as an outline - and a ring in it would be
    // invisible.
    if (m_colSel == m_colBack) m_colSel = m_colText;

    m_font = m_host.editorFont();
    if (m_font == NULL) m_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}


void Editor::layout()
{
    if (m_wnd == NULL) return;

    RECT client;
    GetClientRect(m_wnd, &client);

    const int pad = MulDiv(4, m_dpi, 96);

    // One device context for both measurements below. The host's font is
    // whatever the user set it to, so the row height is measured rather than
    // assumed, and so are the footer buttons.
    const HDC dc = GetDC(m_wnd);
    const HFONT oldFont = (dc != NULL) ? (HFONT)SelectObject(dc, m_font) : NULL;

    int textH = MulDiv(13, m_dpi, 96);
    if (dc != NULL) {
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm)) textH = (int)tm.tmHeight;
    }

    m_layout.rowH  = textH + MulDiv(2, m_dpi, 96);
    m_layout.cellW = 0;
    SetRectEmpty(&m_layout.strip);
    SetRectEmpty(&m_layout.foot);
    for (int b = 0; b < kBtnCount; b++) SetRectEmpty(&m_layout.button[b]);

    RECT rest = client;
    InflateRect(&rest, -pad, -pad);
    if (rest.right <= rest.left || rest.bottom <= rest.top) {
        m_layout.curve = client;
        if (dc != NULL) { SelectObject(dc, oldFont); ReleaseDC(m_wnd, dc); }
        return;
    }

    const int footH    = m_layout.rowH + MulDiv(6, m_dpi, 96);
    const int stripH   = m_layout.rowH * (kRowCount + 1);   // a header row on top
    const int curveMin = MulDiv(60, m_dpi, 96);

    // Shed the chrome from the bottom up as the panel gets shorter: the curve
    // is the part that still says something at forty pixels tall, the footer
    // buttons have keyboard and context-menu equivalents, and the readouts are
    // a convenience. A panel too small for any of it still draws a curve.
    int height = rest.bottom - rest.top;

    if (height >= curveMin + stripH + footH + pad * 2) {
        m_layout.foot = rest;
        m_layout.foot.top = rest.bottom - footH;
        rest.bottom = m_layout.foot.top - pad;
        height = rest.bottom - rest.top;
    }
    if (height >= curveMin + stripH + pad) {
        m_layout.strip = rest;
        m_layout.strip.top = rest.bottom - stripH;

        // How wide a cell has to be is a question about the font rather than a
        // number, so it is measured: the widest thing a cell ever holds is a Q,
        // and the widest row name is "Shape". A strip whose columns are
        // narrower than that would be a row of ellipses, which is worse than no
        // strip at all - so it goes, and the curve takes the height back.
        SIZE cellProbe = { MulDiv(34, m_dpi, 96), 0 };
        SIZE nameProbe = { MulDiv(28, m_dpi, 96), 0 };
        if (dc != NULL) {
            GetTextExtentPoint32W(dc, L"Q 0.00", 6, &cellProbe);
            GetTextExtentPoint32W(dc, L"Shape",  5, &nameProbe);
        }
        const int cellMin = cellProbe.cx + MulDiv(8, m_dpi, 96);
        const int nameMin = nameProbe.cx + MulDiv(6, m_dpi, 96);

        const int stripW = m_layout.strip.right - m_layout.strip.left;
        const int labelW = clampi(stripW / 9, nameMin, nameMin * 2);
        m_layout.cellW = (stripW - labelW) / kBandCount;

        if (m_layout.cellW < cellMin) {
            SetRectEmpty(&m_layout.strip);
            m_layout.cellW = 0;
        } else {
            rest.bottom = m_layout.strip.top - pad;
        }
    }

    m_layout.curve = rest;

    if (dc != NULL && !IsRectEmpty(&m_layout.foot)) {
        const int padX = MulDiv(8, m_dpi, 96);
        const int gap  = MulDiv(4, m_dpi, 96);
        int x = m_layout.foot.left;

        for (int b = 0; b < kBtnCount; b++) {
            if (b == kBtnEngage && m_engaged) continue;

            SIZE sz = { 0, 0 };
            GetTextExtentPoint32W(dc, kBtnText[b], (int)wcslen(kBtnText[b]), &sz);
            const int w = sz.cx + padX * 2;
            // Out of room: the rest are left empty rather than squeezed.
            // Everything down here has a key and a context menu item too.
            if (x + w > m_layout.foot.right) break;

            SetRect(&m_layout.button[b], x, m_layout.foot.top,
                    x + w, m_layout.foot.bottom);
            x += w + gap;
        }
    }

    if (dc != NULL) { SelectObject(dc, oldFont); ReleaseDC(m_wnd, dc); }
}


void Editor::rebuildTable()
{
    const int w = m_layout.curve.right - m_layout.curve.left;
    if (w <= 1) {
        m_hz.clear(); m_trig.clear(); m_db.clear(); m_dbBand.clear();
        m_poly.clear(); m_polyBand.clear();
        return;
    }

    const size_t n = (size_t)w;
    m_hz.resize(n);
    m_trig.resize(n * (size_t)kCurveTrigStride);
    m_db.resize(n);
    m_dbBand.resize(n);
    m_poly.resize(n);
    m_polyBand.resize(n);

    // One point per pixel column, spaced logarithmically, which is the spacing
    // the axis is drawn on.
    const double ratio = kAxisHiHz / kAxisLoHz;
    for (size_t i = 0; i < n; i++) {
        m_hz[i] = kAxisLoHz * pow(ratio, (double)i / (double)(n - 1));
    }

    // The expensive half, and the reason it is here rather than in ensureCurve:
    // it depends on the width and the sample rate and on nothing a control can
    // move, so a drag never touches it. See paraeq_core.h.
    curveTrig(m_hz.data(), n, m_sampleRate, m_trig.data());

    m_curveStale = true;
}


void Editor::markCurveStale()
{
    m_curveStale = true;
}


void Editor::ensureCurve()
{
    if (!m_curveStale || m_hz.empty()) return;
    m_curveStale = false;

    Config cfg;
    cfg.compute(m_params, m_sampleRate);

    const size_t n = m_hz.size();
    curveDb(cfg, m_trig.data(), n, m_db.data());

    const int left = m_layout.curve.left;
    // Coordinates well outside the rectangle are clipped anyway; holding them
    // near it keeps the numbers small, which older GDI cared about and the
    // clipping region does not mind.
    const int lo = m_layout.curve.top - 4;
    const int hi = m_layout.curve.bottom + 4;

    for (size_t i = 0; i < n; i++) {
        m_poly[i].x = left + (int)i;
        m_poly[i].y = clampi(yOfDb(m_db[i]), lo, hi);
    }

    // The faint overlay for the band being pointed at or dragged. Only one
    // band is ever highlighted, so this is one more pass and not six.
    const int band = (m_dragBand >= 0) ? m_dragBand : m_hover;
    if (band >= 0 && band != kOut) {
        curveDb(bandSection(cfg, band), m_trig.data(), n, m_dbBand.data());

        if (band == kHP && m_params.hpSlope == 2) {
            // 24 dB/oct is two sections, and the overlay should show what the
            // pair does rather than half of it. The scratch buffer is a member
            // so that hovering the high-pass does not allocate once a frame.
            m_dbBand2.resize(n);
            curveDb(cfg.stage[kStageHighPass2], m_trig.data(), n, m_dbBand2.data());
            for (size_t i = 0; i < n; i++) m_dbBand[i] += m_dbBand2[i];
        }

        for (size_t i = 0; i < n; i++) {
            m_polyBand[i].x = left + (int)i;
            m_polyBand[i].y = clampi(yOfDb(m_dbBand[i]), lo, hi);
        }
    }
}


int Editor::xOfHz(double hz) const
{
    const int w = m_layout.curve.right - m_layout.curve.left;
    if (w <= 1) return m_layout.curve.left;
    const double t = log(clampd(hz, 1.0, 1e6) / kAxisLoHz) / log(kAxisHiHz / kAxisLoHz);
    return m_layout.curve.left + (int)(clampd(t, -0.2, 1.2) * (w - 1) + 0.5);
}


double Editor::hzOfX(int x) const
{
    const int w = m_layout.curve.right - m_layout.curve.left;
    if (w <= 1) return kAxisLoHz;
    const double t = (double)(x - m_layout.curve.left) / (double)(w - 1);
    return kAxisLoHz * pow(kAxisHiHz / kAxisLoHz, t);
}


int Editor::yOfDb(double db) const
{
    const int h = m_layout.curve.bottom - m_layout.curve.top;
    if (h <= 1) return m_layout.curve.top;
    const double t = (kAxisDb - db) / (2.0 * kAxisDb);
    return m_layout.curve.top + (int)(clampd(t, -0.5, 1.5) * (h - 1) + 0.5);
}


double Editor::dbOfY(int y) const
{
    const int h = m_layout.curve.bottom - m_layout.curve.top;
    if (h <= 1) return 0.0;
    const double t = (double)(y - m_layout.curve.top) / (double)(h - 1);
    return kAxisDb - t * 2.0 * kAxisDb;
}


//! Where a handle is drawn, and therefore where it can be grabbed - the two
//! have to be the same function or a handle at the edge of the plot would be
//! drawn in one place and picked up in another. Held a radius inside the frame
//! so that a band at either end of its travel is still a whole circle.
int Editor::handleX(int band) const
{
    if (band == kOut) return m_layout.curve.right - handleRadius() - 1;

    const int r = handleRadius();
    return clampi(xOfHz(bandFreq(m_params, band)),
                  m_layout.curve.left + r, m_layout.curve.right - r - 1);
}


int Editor::handleRadius() const
{
    return MulDiv(6, m_dpi, 96);
}


int Editor::handleY(int band) const
{
    if (band == kOut) return yOfDb(m_params.outputGain);

    if (band == kHP) {
        // The high-pass has no gain of its own, so its handle rides the curve
        // at its corner - which is where the curve is 3 dB down when it is on
        // and on the zero line when it is off. Both are the truth about it.
        const int x = handleX(kHP) - m_layout.curve.left;
        if (x >= 0 && (size_t)x < m_db.size()) {
            return clampi(yOfDb(m_db[(size_t)x]),
                          m_layout.curve.top, m_layout.curve.bottom - 1);
        }
        return yOfDb(0.0);
    }

    const int r = handleRadius();
    return clampi(yOfDb(bandGain(m_params, band)),
                  m_layout.curve.top + r, m_layout.curve.bottom - r - 1);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void Editor::releaseBuffer()
{
    if (m_memDC != NULL) {
        if (m_memOld != NULL) SelectObject(m_memDC, m_memOld);
        DeleteDC(m_memDC);
    }
    if (m_memBmp != NULL) DeleteObject(m_memBmp);
    m_memDC = NULL; m_memBmp = NULL; m_memOld = NULL;
    m_memW = 0; m_memH = 0;
}


void Editor::paint(HDC dc, const RECT & client)
{
    const int w = client.right - client.left;
    const int h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return;

    // Everything is drawn into a bitmap and blitted in one go: a curve, a grid
    // and forty pieces of text drawn straight onto the window would flicker on
    // every frame of a drag. The bitmap is kept between paints and only rebuilt
    // when the window grows.
    if (m_memDC == NULL || m_memW < w || m_memH < h) {
        releaseBuffer();
        m_memDC = CreateCompatibleDC(dc);
        if (m_memDC == NULL) return;
        m_memBmp = CreateCompatibleBitmap(dc, w, h);
        if (m_memBmp == NULL) { releaseBuffer(); return; }
        m_memOld = (HBITMAP)SelectObject(m_memDC, m_memBmp);
        m_memW = w; m_memH = h;
    }

    const HDC mem = m_memDC;
    const HFONT oldFont = (HFONT)SelectObject(mem, m_font);
    SetBkMode(mem, TRANSPARENT);

    fillRect(mem, client, m_colBack);

    ensureCurve();
    paintCurve(mem);
    paintStrip(mem);
    paintFoot(mem);

    // The focus ring goes round everything rather than round the selected band,
    // because it answers a different question: not which band is selected, but
    // whether the arrow keys are going to reach this panel at all.
    if (GetFocus() == m_wnd && editable()) {
        frameRect(mem, client, blend(m_colBack, m_colSel, 0.55));
    }

    SelectObject(mem, oldFont);
    BitBlt(dc, client.left, client.top, w, h, mem, 0, 0, SRCCOPY);
}


void Editor::paintCurve(HDC dc)
{
    const RECT & rc = m_layout.curve;
    if (IsRectEmpty(&rc)) return;

    // A disengaged equaliser is drawn in the dim colour throughout: the curve
    // is still the truth about the settings, it is just not the truth about
    // what is coming out of the speakers.
    const bool live = m_engaged && !m_params.bypass;

    const HPEN gridPen = CreatePen(PS_SOLID, 1, m_colGrid);
    const HPEN zeroPen = CreatePen(PS_SOLID, 1, blend(m_colBack, m_colText, 0.34));
    const HPEN oldPen  = (HPEN)SelectObject(dc, gridPen);

    for (int i = 0; i < kGridHzCount; i++) {
        const int x = xOfHz(kGridHz[i]);
        if (x < rc.left || x >= rc.right) continue;
        MoveToEx(dc, x, rc.top, NULL);
        LineTo(dc, x, rc.bottom);
    }
    for (int i = 0; i < kGridDbCount; i++) {
        const int y = yOfDb(kGridDb[i]);
        if (y < rc.top || y >= rc.bottom) continue;
        SelectObject(dc, kGridDb[i] == 0.0 ? zeroPen : gridPen);
        MoveToEx(dc, rc.left, y, NULL);
        LineTo(dc, rc.right, y);
    }

    // The axis labels sit inside the plot in the dim colour rather than in
    // gutters of their own, which would cost the curve a fifth of the width in
    // a panel this small.
    const int labelPad = MulDiv(2, m_dpi, 96);
    for (int i = 0; i < kLabelHzCount; i++) {
        const int x = xOfHz(kLabelHz[i]);
        if (x < rc.left || x >= rc.right - MulDiv(18, m_dpi, 96)) continue;
        RECT r = { x + labelPad, rc.bottom - m_layout.rowH, x + MulDiv(40, m_dpi, 96), rc.bottom };
        drawText(dc, r, kLabelHzText[i], DT_LEFT | DT_BOTTOM, m_colDim);
    }
    {
        const int step = (int)kAxisDb / 2;
        wchar_t text[16];
        for (int s = -1; s <= 1; s += 2) {
            _snwprintf_s(text, _countof(text), _TRUNCATE, L"%+d", s * step);
            const int y = yOfDb(s * (double)step);
            RECT r = { rc.left + labelPad, y - m_layout.rowH / 2,
                       rc.left + MulDiv(30, m_dpi, 96), y + m_layout.rowH / 2 };
            drawText(dc, r, text, DT_LEFT | DT_VCENTER, m_colDim);
        }
    }

    // Everything from here is clipped to the plot, so a band pushed off the top
    // stops at the frame instead of drawing over the readouts.
    const int savedDC = SaveDC(dc);
    IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);

    const int band = (m_dragBand >= 0) ? m_dragBand : m_hover;
    if (band >= 0 && band != kOut && !m_polyBand.empty()) {
        const HPEN bandPen = CreatePen(PS_SOLID, 1, blend(m_colBack, m_colSel, 0.55));
        if (bandPen != NULL) {
            SelectObject(dc, bandPen);
            Polyline(dc, m_polyBand.data(), (int)m_polyBand.size());
            SelectObject(dc, gridPen);
            DeleteObject(bandPen);
        }
    }

    // The output trim as a dashed line where flat now sits. It is the one
    // control with no frequency, so it gets the whole width instead of a point
    // on it, and it can be grabbed anywhere along that width.
    if (m_params.outputGain != 0.0f) {
        const HPEN trimPen = CreatePen(PS_DOT, 1, m_colDim);
        if (trimPen != NULL) {
            SelectObject(dc, trimPen);
            const int y = yOfDb(m_params.outputGain);
            MoveToEx(dc, rc.left, y, NULL);
            LineTo(dc, rc.right, y);
            SelectObject(dc, gridPen);
            DeleteObject(trimPen);
        }
    }

    if (!m_poly.empty()) {
        const int width = MulDiv(2, m_dpi, 96);
        const HPEN curvePen = CreatePen(PS_SOLID, width < 1 ? 1 : width,
                                        live ? m_colCurve : m_colDim);
        if (curvePen != NULL) {
            SelectObject(dc, curvePen);
            Polyline(dc, m_poly.data(), (int)m_poly.size());
            SelectObject(dc, gridPen);
            DeleteObject(curvePen);
        }
    }

    // Handles last, on top of the curve they describe.
    const int r  = MulDiv(4, m_dpi, 96);
    const int rs = handleRadius();
    for (int b = 0; b < kBandCount; b++) {
        if (b == kOut) continue;

        const int x = handleX(b);
        const int y = handleY(b);
        const bool sel = (b == m_selected);
        const bool hot = (b == m_hover) || (b == m_dragBand);
        const int  rad = (sel || hot) ? rs : r;

        const COLORREF edge = sel ? m_colSel : (live ? m_colText : m_colDim);
        const HPEN pen = CreatePen(PS_SOLID, sel ? MulDiv(2, m_dpi, 96) : 1, edge);
        const HBRUSH br = CreateSolidBrush(hot || sel ? edge : m_colBack);
        if (pen != NULL && br != NULL) {
            const HPEN po = (HPEN)SelectObject(dc, pen);
            const HBRUSH bo = (HBRUSH)SelectObject(dc, br);
            Ellipse(dc, x - rad, y - rad, x + rad + 1, y + rad + 1);
            SelectObject(dc, po);
            SelectObject(dc, bo);
        }
        if (pen != NULL) DeleteObject(pen);
        if (br != NULL) DeleteObject(br);
    }

    // Then the names beside them, so the curve can be read without looking down
    // at the strip to work out which bump is which.
    //
    // Narrow the panel far enough and the handles crowd together, at which point
    // five names in a row become one smear. So they are placed rather than
    // simply drawn: the band being worked on goes first and keeps its name
    // whatever the width, because that is the one whose identity is in question,
    // and each of the rest is dropped if it would land on a name already there.
    {
        const int nameW = MulDiv(26, m_dpi, 96);
        RECT claimed[kBandCount];
        int  claimedCount = 0;

        int order[kBandCount];
        int n = 0;
        if (m_selected >= 0 && m_selected != kOut) order[n++] = m_selected;
        if (m_hover >= 0 && m_hover != kOut && m_hover != m_selected) order[n++] = m_hover;
        for (int b = 0; b < kBandCount; b++) {
            if (b == kOut || b == m_selected || b == m_hover) continue;
            order[n++] = b;
        }

        for (int i = 0; i < n; i++) {
            const int b = order[i];
            const bool sel = (b == m_selected);
            const int  rad = (sel || b == m_hover || b == m_dragBand) ? rs : r;
            const int  x = handleX(b);
            const int  y = handleY(b);

            // On the left when there is no room on the right, which is what
            // happens to the high shelf at the top of its travel.
            const bool onLeft = (x + rad + labelPad + nameW > rc.right);
            RECT lr = onLeft
                ? RECT{ x - rad - labelPad - nameW, y - m_layout.rowH, x - rad - labelPad, y }
                : RECT{ x + rad + labelPad, y - m_layout.rowH, x + rad + labelPad + nameW, y };

            bool clear = true;
            for (int c = 0; c < claimedCount && clear; c++) {
                RECT ignored;
                if (IntersectRect(&ignored, &lr, &claimed[c])) clear = false;
            }
            if (!clear && i > 0) continue;          // the first one always gets drawn

            claimed[claimedCount++] = lr;
            drawText(dc, lr, kSpec[b].name,
                     (onLeft ? DT_RIGHT : DT_LEFT) | DT_BOTTOM,
                     sel ? m_colSel : m_colDim);
        }
    }

    RestoreDC(dc, savedDC);
    SelectObject(dc, oldPen);
    DeleteObject(gridPen);
    DeleteObject(zeroPen);

    frameRect(dc, rc, m_colGrid);
}


void Editor::paintStrip(HDC dc)
{
    const RECT & rc = m_layout.strip;
    if (IsRectEmpty(&rc) || m_layout.cellW <= 0) return;

    const int rowH   = m_layout.rowH;
    const int cellW  = m_layout.cellW;
    const int labelW = (rc.right - rc.left) - cellW * kBandCount;
    const int pad    = MulDiv(3, m_dpi, 96);

    // The selected band's column, marked behind the text rather than round it:
    // a box inside a strip this dense turns into another grid line.
    if (m_selected >= 0) {
        RECT col = { rc.left + labelW + cellW * m_selected, rc.top,
                     rc.left + labelW + cellW * (m_selected + 1), rc.bottom };
        fillRect(dc, col, m_colFill);
    }

    for (int b = 0; b < kBandCount; b++) {
        RECT r = { rc.left + labelW + cellW * b, rc.top,
                   rc.left + labelW + cellW * (b + 1), rc.top + rowH };
        InflateRect(&r, -pad, 0);
        drawText(dc, r, kSpec[b].name, DT_RIGHT | DT_VCENTER,
                 b == m_selected ? m_colText : m_colDim);
    }

    for (int row = 0; row < kRowCount; row++) {
        RECT lr = { rc.left, rc.top + rowH * (row + 1),
                    rc.left + labelW, rc.top + rowH * (row + 2) };
        InflateRect(&lr, -pad, 0);
        drawText(dc, lr, kRowName[row], DT_LEFT | DT_VCENTER, m_colDim);

        for (int b = 0; b < kBandCount; b++) {
            wchar_t text[24];
            text[0] = 0;

            switch (row) {
            case kRowFreq:
                if (kSpec[b].fMax > 0.0f) formatHz(text, _countof(text), bandFreq(m_params, b));
                else                      wcscpy_s(text, _countof(text), L"-");
                break;

            case kRowGain:
                if (kSpec[b].hasGain) {
                    _snwprintf_s(text, _countof(text), _TRUNCATE, L"%+.1f",
                                 bandGain(m_params, b));
                } else {
                    wcscpy_s(text, _countof(text), L"-");
                }
                break;

            default:
                if (b == kHP) {
                    const int s = clampi(m_params.hpSlope, 0, 2);
                    wcscpy_s(text, _countof(text),
                             s == 0 ? L"off" : (s == 1 ? L"12dB" : L"24dB"));
                } else if (kSpec[b].hasShelf) {
                    const bool bell = (b == kLF) ? m_params.lfBell : m_params.hfBell;
                    wcscpy_s(text, _countof(text), bell ? L"bell" : L"shelf");
                } else if (kSpec[b].hasQ) {
                    _snwprintf_s(text, _countof(text), _TRUNCATE, L"Q %.2f",
                                 bandQ(m_params, b));
                } else {
                    wcscpy_s(text, _countof(text), L"-");
                }
                break;
            }

            RECT r = { rc.left + labelW + cellW * b, rc.top + rowH * (row + 1),
                       rc.left + labelW + cellW * (b + 1), rc.top + rowH * (row + 2) };
            InflateRect(&r, -pad, 0);

            const bool dash = (text[0] == L'-' && text[1] == 0);
            drawText(dc, r, text, DT_RIGHT | DT_VCENTER,
                     dash ? m_colGrid : (m_engaged ? m_colText : m_colDim));
        }
    }
}


void Editor::paintFoot(HDC dc)
{
    const RECT & rc = m_layout.foot;
    if (IsRectEmpty(&rc)) return;

    int rightmost = rc.left;

    for (int b = 0; b < kBtnCount; b++) {
        const RECT & r = m_layout.button[b];
        if (IsRectEmpty(&r)) continue;

        const bool on  = (b == kBtnBypass && m_params.bypass);
        const bool hot = (b == m_footHot);
        if (on)       fillRect(dc, r, blend(m_colBack, m_colSel, 0.35));
        else if (hot) fillRect(dc, r, m_colFill);

        frameRect(dc, r, on ? m_colSel : m_colGrid);
        drawText(dc, r, kBtnText[b], DT_CENTER | DT_VCENTER, m_colText);

        if (r.right > rightmost) rightmost = r.right;
    }

    if (!m_engaged) {
        RECT r = { rightmost + MulDiv(8, m_dpi, 96), rc.top, rc.right, rc.bottom };
        if (r.right > r.left) {
            drawText(dc, r, L"not in the DSP chain", DT_LEFT | DT_VCENTER, m_colDim);
        }
    }
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

int Editor::hitHandle(POINT pt) const
{
    if (!PtInRect(&m_layout.curve, pt)) return -1;

    const int reach = MulDiv(9, m_dpi, 96);
    int best = -1;
    long bestDist = reach * reach + 1;

    for (int b = 0; b < kBandCount; b++) {
        if (b == kOut) continue;
        const long dx = pt.x - handleX(b);
        const long dy = pt.y - handleY(b);
        const long d  = dx * dx + dy * dy;
        if (d < bestDist) { bestDist = d; best = b; }
    }
    if (best >= 0) return best;

    // The trim line has no handle, so it is grabbed by its height anywhere
    // across the plot - but only after every real handle has had its chance,
    // or a band sitting on the trim line would be unreachable.
    if (labs(pt.y - yOfDb(m_params.outputGain)) <= MulDiv(4, m_dpi, 96)) return kOut;

    return -1;
}


int Editor::hitStripCell(POINT pt, int * row) const
{
    if (IsRectEmpty(&m_layout.strip) || !PtInRect(&m_layout.strip, pt)) return -1;
    if (m_layout.cellW <= 0 || m_layout.rowH <= 0) return -1;

    const int labelW = (m_layout.strip.right - m_layout.strip.left)
                     - m_layout.cellW * kBandCount;
    const int b = (pt.x - m_layout.strip.left - labelW) / m_layout.cellW;
    if (b < 0 || b >= kBandCount) return -1;

    const int r = (pt.y - m_layout.strip.top) / m_layout.rowH - 1;   // header
    if (row != NULL) *row = clampi(r, -1, kRowCount - 1);
    return b;
}


int Editor::hitFootButton(POINT pt) const
{
    for (int b = 0; b < kBtnCount; b++) {
        if (PtInRect(&m_layout.button[b], pt)) return b;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void Editor::select(int band)
{
    if (band == m_selected) return;
    m_selected = band;
    markCurveStale();
    if (m_wnd) InvalidateRect(m_wnd, NULL, FALSE);
}


void Editor::commit()
{
    m_params.sanitize();
    markCurveStale();
    if (m_wnd) InvalidateRect(m_wnd, NULL, FALSE);
    schedulePush();
}


void Editor::schedulePush()
{
    m_pushDirty = true;
    if (m_wnd == NULL) return;

    const DWORD now = GetTickCount();
    const DWORD since = now - m_lastPush;      // wraps correctly, being unsigned

    if (since >= kPushIntervalMs) {
        flushPush();
    } else if (!m_pushTimer) {
        m_pushTimer = SetTimer(m_wnd, kTimerPush, kPushIntervalMs - since, NULL) != 0;
        if (!m_pushTimer) flushPush();         // no timer to be had; take the cost
    }
}


void Editor::flushPush()
{
    if (m_pushTimer && m_wnd != NULL) {
        KillTimer(m_wnd, kTimerPush);
        m_pushTimer = false;
    }
    if (!m_pushDirty) return;

    m_pushDirty = false;
    m_lastPush  = GetTickCount();
    m_host.editorParamsChanged(m_params);
}


void Editor::nudgeGain(int band, double db)
{
    if (band < 0 || band >= kBandCount) return;

    if (band == kHP) {
        // The high-pass has a slope where the others have a gain, and up should
        // mean more of it.
        m_params.hpSlope = clampi(m_params.hpSlope + (db > 0 ? 1 : -1), 0, 2);
        return;
    }
    if (!kSpec[band].hasGain) return;

    const double limit = (band == kOut) ? kOutputMaxDb : kGainMaxDb;
    setBandGain(m_params, band, clampd(bandGain(m_params, band) + db, -limit, limit));
}


void Editor::nudgeFreq(int band, double semitones)
{
    if (band < 0 || band >= kBandCount) return;
    if (!(kSpec[band].fMax > 0.0f)) return;

    // Geometric, because that is how a frequency control has to move: a step
    // that is an octave at 8 kHz is the whole of the low shelf at 30 Hz.
    const double hz = bandFreq(m_params, band) * pow(2.0, semitones / 12.0);
    setBandFreq(m_params, band, clampd(hz, kSpec[band].fMin, kSpec[band].fMax));
}


void Editor::nudgeQ(int band, int steps)
{
    if (band < 0 || band >= kBandCount) return;

    if (band == kHP) {
        m_params.hpSlope = clampi(m_params.hpSlope + steps, 0, 2);
        return;
    }
    if (!kSpec[band].hasQ) {
        if (kSpec[band].hasShelf && steps != 0) toggleShape(band);
        return;
    }

    // Sixth of an octave per step: fine enough to place a notch on a ring,
    // coarse enough that 0.5 to 8 is twenty-four presses rather than a hundred.
    const double q = bandQ(m_params, band) * pow(2.0, steps / 6.0);
    setBandQ(m_params, band, clampd(q, kQMin, kQMax));
}


void Editor::toggleShape(int band)
{
    if (band == kHP) {
        m_params.hpSlope = (m_params.hpSlope + 1) % 3;
    } else if (band == kLF) {
        m_params.lfBell = !m_params.lfBell;
    } else if (band == kHF) {
        m_params.hfBell = !m_params.hfBell;
    }
}


void Editor::resetBand(int band)
{
    const Params d = Params::defaults();
    switch (band) {
    case kHP:  m_params.hpFrequency = d.hpFrequency; m_params.hpSlope = d.hpSlope; break;
    case kLF:  m_params.lfGain = d.lfGain; m_params.lfFrequency = d.lfFrequency;
               m_params.lfBell = d.lfBell; break;
    case kLMF: m_params.lmfGain = d.lmfGain; m_params.lmfFrequency = d.lmfFrequency;
               m_params.lmfQ = d.lmfQ; break;
    case kHMF: m_params.hmfGain = d.hmfGain; m_params.hmfFrequency = d.hmfFrequency;
               m_params.hmfQ = d.hmfQ; break;
    case kHF:  m_params.hfGain = d.hfGain; m_params.hfFrequency = d.hfFrequency;
               m_params.hfBell = d.hfBell; break;
    case kOut: m_params.outputGain = d.outputGain; break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void Editor::onMouseDown(POINT pt, bool doubleClick)
{
    if (!editable()) return;
    SetFocus(m_wnd);

    const int btn = hitFootButton(pt);
    if (btn >= 0) {
        switch (btn) {
        case kBtnEngage:
            // Before, not after: a move made a few milliseconds ago may still
            // be sitting in the throttle, and the host builds what it inserts
            // out of the last settings it was told about.
            flushPush();
            m_host.editorEngage();
            refreshEngaged();
            return;
        case kBtnBypass:
            m_params.bypass = !m_params.bypass;
            break;
        case kBtnFlatten:
            // Gains to zero and nothing else. Where the bands sit and how wide
            // they are is a setting for the box of records; what they are doing
            // is a setting for this one.
            m_params.lfGain = m_params.lmfGain = 0.0f;
            m_params.hmfGain = m_params.hfGain = 0.0f;
            m_params.outputGain = 0.0f;
            break;
        default:
            m_params = Params::defaults();
            break;
        }
        commit();
        flushPush();
        return;
    }

    int row = -1;
    const int cell = hitStripCell(pt, &row);
    if (cell >= 0) {
        select(cell);
        if (row < 0) return;                    // the header row only selects

        if (doubleClick) { resetBand(cell); commit(); flushPush(); return; }

        // A row that names a state rather than a quantity toggles on click;
        // there is nothing to drag along.
        if (row == kRowShape && (cell == kHP || kSpec[cell].hasShelf)) {
            toggleShape(cell);
            commit();
            flushPush();
            return;
        }

        m_dragBand  = cell;
        m_dragRow   = row;
        m_dragFrom  = pt;
        m_dragStart = m_params;
        m_dragging  = true;
        SetCapture(m_wnd);
        return;
    }

    if (!PtInRect(&m_layout.curve, pt)) return;

    const int band = hitHandle(pt);
    if (band < 0) {
        // Empty plot. Select the nearest band by frequency so that the arrow
        // keys have something to work on, but move nothing: a click that
        // teleported a band to where it landed would be unforgivable.
        int best = -1;
        long bestDx = MulDiv(10000, m_dpi, 96);
        for (int b = 0; b < kBandCount; b++) {
            if (b == kOut) continue;
            const long dx = labs(pt.x - handleX(b));
            if (dx < bestDx) { bestDx = dx; best = b; }
        }
        select(best);
        return;
    }

    select(band);

    if (doubleClick) {
        if (band == kHP)      m_params.hpSlope = (m_params.hpSlope == 0) ? 1 : 0;
        else                  setBandGain(m_params, band, 0.0);
        commit();
        flushPush();
        return;
    }

    m_dragBand  = band;
    m_dragRow   = -1;
    m_dragFrom  = pt;
    m_dragStart = m_params;
    m_dragging  = true;
    SetCapture(m_wnd);
}


void Editor::onMouseMove(POINT pt)
{
    if (!editable()) return;

    if (!m_dragging) {
        const int hover = hitHandle(pt);
        const int foot  = hitFootButton(pt);
        if (hover != m_hover || foot != m_footHot) {
            m_hover   = hover;
            m_footHot = foot;
            markCurveStale();
            InvalidateRect(m_wnd, NULL, FALSE);
        }
        return;
    }

    if (m_dragBand < 0) return;

    // Held ctrl scales the whole displacement rather than switching to a
    // different mapping, so a drag that starts coarse and turns fine does not
    // jump when the key goes down.
    const double fine = (GetKeyState(VK_CONTROL) & 0x8000) ? 0.25 : 1.0;
    const double dx = (pt.x - m_dragFrom.x) * fine;
    const double dy = (m_dragFrom.y - pt.y) * fine;

    // A handle drag is one to one with the axes and so is already the same
    // gesture at any scaling. A readout cell has no axis to be one to one with,
    // so its rates are quoted per 96-dpi pixel and converted here, or the same
    // movement of the hand would mean different things on different displays.
    const double dyLogical = dy * 96.0 / (double)m_dpi;

    const int b = m_dragBand;

    if (m_dragRow < 0) {
        // Dragging the handle itself. One to one with the axes it is drawn on,
        // so the handle stays under the pointer.
        if (kSpec[b].fMax > 0.0f) {
            const int w = m_layout.curve.right - m_layout.curve.left;
            if (w > 1) {
                const double octaves = dx / (double)(w - 1)
                                     * (log(kAxisHiHz / kAxisLoHz) / log(2.0));
                const double hz = bandFreq(m_dragStart, b) * pow(2.0, octaves);
                setBandFreq(m_params, b, clampd(hz, kSpec[b].fMin, kSpec[b].fMax));
            }
        }
        if (kSpec[b].hasGain) {
            const int h = m_layout.curve.bottom - m_layout.curve.top;
            if (h > 1) {
                const double limit = (b == kOut) ? kOutputMaxDb : kGainMaxDb;
                const double db = bandGain(m_dragStart, b)
                                + dy * (2.0 * kAxisDb) / (double)(h - 1);
                setBandGain(m_params, b, clampd(db, -limit, limit));
            }
        }
        // The high-pass handle drags sideways and nothing else. Putting the
        // slope on vertical travel was the first attempt and it is a trap: the
        // handle rides the curve, so a frequency drag is already a diagonal
        // one, and the slope has six other ways to reach it - the wheel, page
        // up and down, the arrow keys, space, the context menu and its own
        // readout cell.
    } else {
        // Dragging a readout cell. Vertical only, at a rate that crosses the
        // control's whole range in about the height of the plot.
        switch (m_dragRow) {
        case kRowFreq:
            // An octave per 48 pixels, which is about what dragging the
            // handle across a plot of the usual width comes to.
            if (kSpec[b].fMax > 0.0f) {
                const double hz = bandFreq(m_dragStart, b) * pow(2.0, dyLogical / 48.0);
                setBandFreq(m_params, b, clampd(hz, kSpec[b].fMin, kSpec[b].fMax));
            }
            break;
        case kRowGain:
            // A quarter of a dB per pixel, likewise: 48 dB over a 200-pixel
            // plot is the rate the handle moves at.
            if (kSpec[b].hasGain) {
                const double limit = (b == kOut) ? kOutputMaxDb : kGainMaxDb;
                setBandGain(m_params, b,
                            clampd(bandGain(m_dragStart, b) + dyLogical * 0.25,
                                   -limit, limit));
            } else if (b == kHP) {
                m_params.hpSlope =
                    clampi(m_dragStart.hpSlope + (int)(dyLogical / 24.0), 0, 2);
            }
            break;
        default:
            if (kSpec[b].hasQ) {
                setBandQ(m_params, b,
                         clampd(bandQ(m_dragStart, b) * pow(2.0, dyLogical / 48.0),
                                kQMin, kQMax));
            }
            break;
        }
    }

    commit();
}


void Editor::onMouseUp()
{
    if (!m_dragging) return;
    m_dragging = false;
    m_dragBand = -1;
    m_dragRow  = -1;
    if (GetCapture() == m_wnd) ReleaseCapture();
    markCurveStale();
    InvalidateRect(m_wnd, NULL, FALSE);
    flushPush();
}


void Editor::onWheel(POINT screenPt, int delta)
{
    if (!editable() || delta == 0) return;

    POINT pt = screenPt;
    ScreenToClient(m_wnd, &pt);

    int band = hitHandle(pt);
    if (band < 0) {
        int row = -1;
        band = hitStripCell(pt, &row);
    }
    if (band < 0) band = m_selected;
    if (band < 0) return;

    select(band);

    const int steps = delta / WHEEL_DELTA;
    const int n = (steps == 0) ? (delta > 0 ? 1 : -1) : steps;

    if (GetKeyState(VK_SHIFT) & 0x8000) {
        nudgeQ(band, n);
    } else {
        const double step = (GetKeyState(VK_CONTROL) & 0x8000) ? 0.1 : 0.5;
        nudgeGain(band, n * step);
    }
    commit();
}


void Editor::onContextMenu(POINT screenPt)
{
    if (!editable()) return;

    POINT pt = screenPt;
    ScreenToClient(m_wnd, &pt);

    int band = hitHandle(pt);
    if (band < 0) {
        int row = -1;
        band = hitStripCell(pt, &row);
    }
    if (band < 0) band = m_selected;
    if (band < 0) band = kLMF;
    select(band);

    enum { kIdOff = 100, kId12, kId24, kIdShelf, kIdBell,
           kIdResetBand = 200, kIdFlatten, kIdResetAll, kIdBypass };

    const HMENU menu = CreatePopupMenu();
    if (menu == NULL) return;

    wchar_t header[64];
    _snwprintf_s(header, _countof(header), _TRUNCATE, L"%s band", kSpec[band].name);
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, header);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    if (band == kHP) {
        const int s = clampi(m_params.hpSlope, 0, 2);
        AppendMenuW(menu, MF_STRING | (s == 0 ? MF_CHECKED : 0), kIdOff, L"Off");
        AppendMenuW(menu, MF_STRING | (s == 1 ? MF_CHECKED : 0), kId12, L"12 dB/oct");
        AppendMenuW(menu, MF_STRING | (s == 2 ? MF_CHECKED : 0), kId24, L"24 dB/oct");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    } else if (kSpec[band].hasShelf) {
        const bool bell = (band == kLF) ? m_params.lfBell : m_params.hfBell;
        AppendMenuW(menu, MF_STRING | (!bell ? MF_CHECKED : 0), kIdShelf, L"Shelf");
        AppendMenuW(menu, MF_STRING | ( bell ? MF_CHECKED : 0), kIdBell,  L"Bell");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }

    AppendMenuW(menu, MF_STRING, kIdResetBand, L"Reset this band");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (m_params.bypass ? MF_CHECKED : 0), kIdBypass, L"Bypass");
    AppendMenuW(menu, MF_STRING, kIdFlatten,  L"Flatten all gains");
    AppendMenuW(menu, MF_STRING, kIdResetAll, L"Reset everything");

    const int cmd = (int)TrackPopupMenu(menu,
                                        TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN
                                        | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                        screenPt.x, screenPt.y, 0, m_wnd, NULL);
    DestroyMenu(menu);
    if (cmd == 0) return;

    switch (cmd) {
    case kIdOff:   m_params.hpSlope = 0; break;
    case kId12:    m_params.hpSlope = 1; break;
    case kId24:    m_params.hpSlope = 2; break;
    case kIdShelf: if (band == kLF) m_params.lfBell = false; else m_params.hfBell = false; break;
    case kIdBell:  if (band == kLF) m_params.lfBell = true;  else m_params.hfBell = true;  break;
    case kIdResetBand: resetBand(band); break;
    case kIdBypass:    m_params.bypass = !m_params.bypass; break;
    case kIdFlatten:
        m_params.lfGain = m_params.lmfGain = 0.0f;
        m_params.hmfGain = m_params.hfGain = 0.0f;
        m_params.outputGain = 0.0f;
        break;
    case kIdResetAll:  m_params = Params::defaults(); break;
    default: return;
    }

    commit();
    flushPush();
}

// ---------------------------------------------------------------------------
// Keyboard
//
//   left / right          select the previous or next band, wrapping
//   up / down             its gain, 0.5 dB a press; on the high-pass, its slope
//   shift + up / down     the same, 2 dB a press
//   shift + left / right  its frequency, a semitone a press
//   page up / page down   its Q; on the high-pass, its slope; on a shelf, the
//                         shelf/bell switch, which is the nearest thing it has
//   space                 shelf to bell, or the next high-pass slope
//   home                  reset the selected band; ctrl + home resets all of them
//   delete / backspace    its gain to zero
//   ctrl + anything       a quarter of the step, for placing rather than finding
//
// Shift carries frequency rather than ctrl because ctrl with an arrow key is
// claimed by hosts often enough that it cannot be relied on to arrive, and the
// fine modifier is the one that can afford to be the second choice.
// ---------------------------------------------------------------------------

bool Editor::onKey(WPARAM key)
{
    if (!editable()) return false;

    const bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const double fine = ctrl ? 0.25 : 1.0;

    if (m_selected < 0 && key != VK_LEFT && key != VK_RIGHT) {
        // Nothing selected yet and a key that acts on a band: take the one the
        // knob metaphor starts at rather than refusing.
        select(kLF);
    }

    switch (key) {
    case VK_LEFT:
        if (shift) { nudgeFreq(m_selected, -1.0 * fine); break; }
        select(m_selected <= 0 ? kBandCount - 1 : m_selected - 1);
        return true;

    case VK_RIGHT:
        if (shift) { nudgeFreq(m_selected, 1.0 * fine); break; }
        select(m_selected < 0 || m_selected >= kBandCount - 1 ? 0 : m_selected + 1);
        return true;

    case VK_UP:    nudgeGain(m_selected,  0.5 * (shift ? 4.0 : 1.0) * fine); break;
    case VK_DOWN:  nudgeGain(m_selected, -0.5 * (shift ? 4.0 : 1.0) * fine); break;

    case VK_PRIOR: nudgeQ(m_selected,  1); break;
    case VK_NEXT:  nudgeQ(m_selected, -1); break;

    case VK_SPACE: toggleShape(m_selected); break;

    case VK_HOME:
        if (ctrl) m_params = Params::defaults();
        else      resetBand(m_selected);
        break;

    case VK_DELETE:
    case VK_BACK:
        if (m_selected == kHP) m_params.hpSlope = 0;
        else                   setBandGain(m_params, m_selected, 0.0);
        break;

    default:
        return false;
    }

    commit();
    return true;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK Editor::wndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        const CREATESTRUCT * cs = (const CREATESTRUCT *)lp;
        SetWindowLongPtr(wnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        Editor * self = (Editor *)cs->lpCreateParams;
        if (self != NULL) self->m_wnd = wnd;
        return DefWindowProc(wnd, msg, wp, lp);
    }

    Editor * self = (Editor *)GetWindowLongPtr(wnd, GWLP_USERDATA);
    if (self == NULL) return DefWindowProc(wnd, msg, wp, lp);

    return self->handle(msg, wp, lp);
}


LRESULT Editor::handle(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        m_dpi = 96;
        const HDC dc = GetDC(m_wnd);
        if (dc != NULL) {
            const int y = GetDeviceCaps(dc, LOGPIXELSY);
            if (y > 0) m_dpi = y;
            ReleaseDC(m_wnd, dc);
        }
        theme();
        layout();
        rebuildTable();
        return 0;
    }

    case WM_DESTROY:
        if (m_pushTimer) { KillTimer(m_wnd, kTimerPush); m_pushTimer = false; }
        // Anything the user did that has not gone out yet goes out now: a panel
        // closing is not a reason to lose the last move made in it.
        if (m_pushDirty) {
            m_pushDirty = false;
            m_host.editorParamsChanged(m_params);
        }
        releaseBuffer();

        // Drop the back pointer here rather than leaving it for the destructor.
        // WM_NCDESTROY still has to arrive, and every message after this one
        // would otherwise reach handle() with m_wnd already NULL - which ends
        // in DefWindowProc(NULL, ...). Clearing it sends the rest straight to
        // DefWindowProc in wndProc, where the real window handle still is.
        SetWindowLongPtr(m_wnd, GWLP_USERDATA, 0);
        m_wnd = NULL;
        return 0;

    case WM_SIZE:
        layout();
        rebuildTable();
        InvalidateRect(m_wnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;               // every pixel comes from the double buffer

    case WM_PAINT: {
        PAINTSTRUCT ps;
        const HDC dc = BeginPaint(m_wnd, &ps);
        if (dc != NULL) {
            RECT client;
            GetClientRect(m_wnd, &client);
            paint(dc, client);
            EndPaint(m_wnd, &ps);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kTimerPush) { m_pushTimer = false; flushPush(); return 0; }
        break;

    // Without this the dialog manager eats the arrows for tab navigation and
    // the keyboard model above never sees them.
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(m_wnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        onMouseDown(pt, msg == WM_LBUTTONDBLCLK);
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (!m_tracking) {
            TRACKMOUSEEVENT tme;
            memset(&tme, 0, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = m_wnd;
            m_tracking = TrackMouseEvent(&tme) != FALSE;
        }
        onMouseMove(pt);
        return 0;
    }

    case WM_MOUSELEAVE:
        m_tracking = false;
        if (m_hover >= 0 || m_footHot >= 0) {
            m_hover = -1;
            m_footHot = -1;
            markCurveStale();
            InvalidateRect(m_wnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        onMouseUp();
        return 0;

    case WM_CAPTURECHANGED:
        // Capture can be taken away - a menu, an alt-tab - and the drag has to
        // end cleanly when it is, or setParams() stays locked out for good.
        if (m_dragging) {
            m_dragging = false;
            m_dragBand = -1;
            m_dragRow  = -1;
            markCurveStale();
            InvalidateRect(m_wnd, NULL, FALSE);
            flushPush();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };   // screen coordinates
        onWheel(pt, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }

    case WM_CONTEXTMENU: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (pt.x == -1 && pt.y == -1) {          // from the keyboard
            RECT rc;
            GetWindowRect(m_wnd, &rc);
            pt.x = rc.left + (rc.right - rc.left) / 2;
            pt.y = rc.top + (rc.bottom - rc.top) / 2;
        }
        onContextMenu(pt);
        return 0;
    }

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && editable()) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(m_wnd, &pt);
            const int band = hitHandle(pt);
            int row = -1;
            if (band == kOut || (band < 0 && hitStripCell(pt, &row) >= 0 && row >= 0)) {
                SetCursor(LoadCursor(NULL, IDC_SIZENS));
                return TRUE;
            }
            if (band >= 0) {
                SetCursor(LoadCursor(NULL, IDC_SIZEALL));
                return TRUE;
            }
        }
        break;

    case WM_KEYDOWN:
        if (onKey(wp)) return 0;
        break;

    case WM_KEYUP:
        // The last press of a held arrow key gets out without waiting for the
        // throttle, the same as the end of a drag does.
        flushPush();
        break;

    default:
        break;
    }

    return DefWindowProc(m_wnd, msg, wp, lp);
}

} // namespace paraeq_editor
