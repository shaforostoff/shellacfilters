/* ========================================
 *  foo_dsp_declick - configuration dialog
 *
 *  Plain Win32, same as foo_dsp_decrackle: no ATL, no WTL, no libPPUI, so the
 *  component builds from nothing but the foobar2000 SDK archive.
 * ======================================== */

#include "stdafx.h"

#include "declick_preset.h"
#include "resource.h"

#include <stdio.h>

using declick::Params;

namespace {

//! Each slider works in integer steps and maps onto its parameter through
//! `lo + pos * step`. Keeping the mapping in a table lets one handler drive
//! all six of them.
//!
//! `help` is the hover text, shared word for word with the AU's parameter
//! tooltips; it is condensed from the rationale in declick_core.h, which has
//! the measurements behind it.
struct slider_def {
    int    sliderId;
    int    labelId;
    int    valueId;
    int    steps;        // slider range is 0..steps
    double lo, hi;       // parameter range
    int    decimals;     // for the readout
    const wchar_t * suffix;
    const wchar_t * help;
};

const slider_def k_sliders[] = {
    { IDC_SLIDER_SENSITIVITY, IDC_LABEL_SENSITIVITY, IDC_VALUE_SENSITIVITY, 1000,  0.0,  1.0, 3, L"",
      L"How readily a sample is called a click. The default, 0.8, puts the trigger at 3.2 sigma "
      L"above the local noise estimate and was chosen by ear on crackly 78s. Past it, music starts "
      L"being flagged as damage faster than crackle is found. On clean records 0.6-0.7 takes less "
      L"and leaves little behind." },

    { IDC_SLIDER_EXTENT,      IDC_LABEL_EXTENT,      IDC_VALUE_EXTENT,      1000,  0.0,  1.0, 3, L"",
      L"How far each detection spreads outwards into the tail of the click. Higher repairs more "
      L"of the decay, but it lowers the floor for every repair, so too high starts replacing good "
      L"audio either side. A strong click whose error rings through zero is repaired whole "
      L"without it. Lower it (0.3) on clean records." },

    { IDC_SLIDER_MAXLEN,      IDC_LABEL_MAXLEN,      IDC_VALUE_MAXLEN,       198,  0.2, 20.0, 1, L" ms",
      L"The longest single stretch that may be reconstructed. Damage longer than this is left "
      L"alone rather than guessed at." },

    { IDC_SLIDER_PASSES,      IDC_LABEL_PASSES,      IDC_VALUE_PASSES,         2,  1.0,  3.0, 0, L"",
      L"How many times the detector sweeps each block. A second pass catches clicks that the "
      L"first pass's repairs uncover." },

    { IDC_SLIDER_ORDER,       IDC_LABEL_ORDER,       IDC_VALUE_ORDER,        124,  8.0, 256.0, 0, L"",
      L"Taps in the autoregressive model that predicts what the waveform should have been. Higher "
      L"orders model the music better, so they change clean records least - 256 is the gentlest - "
      L"but they also miss more of the short clicks on heavily crackled transfers, where 64-128 "
      L"catches more. 128 is the balance. Any value works, not only powers of two; CPU and latency "
      L"grow with it (256 costs about 2.4x what 128 does, 64 about 0.6x)." },

    { IDC_SLIDER_DEPTH,       IDC_LABEL_DEPTH,       IDC_VALUE_DEPTH,       1000,  0.0,  1.0, 3, L"",
      L"How much of the estimated click is subtracted. 1, the default, replaces the damaged "
      L"samples outright. 0 subtracts 45% of each click - the least added error by whole-file "
      L"measure, but it leaves every click no more than 5 dB quieter." },

    { IDC_SLIDER_DRYWET,      IDC_LABEL_DRYWET,      IDC_VALUE_DRYWET,      1000,  0.0,  1.0, 3, L"",
      L"Blend of the repaired signal against the original. 0 passes the input through "
      L"untouched." },
};

const size_t k_count = sizeof(k_sliders) / sizeof(k_sliders[0]);

const wchar_t * const k_delta_help =
    L"Outputs what the repair takes out - the input minus the output - instead of the "
    L"repaired audio. For testing: it should be crackle and clicks and nothing else, and "
    L"any music heard in it is being damaged. Saved with the preset; untick it when done.";

double get_param(const Params & p, size_t i) {
    switch (i) {
    case 0: return p.sensitivity;
    case 1: return p.extent;
    case 2: return p.maxLengthMs;
    case 3: return (double)p.passes;
    case 4: return (double)p.order;
    case 5: return p.depth;
    default: return p.dryWet;
    }
}

void set_param(Params & p, size_t i, double v) {
    switch (i) {
    case 0: p.sensitivity = (float)v; break;
    case 1: p.extent = (float)v; break;
    case 2: p.maxLengthMs = (float)v; break;
    case 3: p.passes = (int)(v + 0.5); break;
    case 4: p.order = (int)(v + 0.5); break;
    case 5: p.depth = (float)v; break;
    default: p.dryWet = (float)v; break;
    }
}

double pos_to_value(const slider_def & s, int pos) {
    if (s.steps <= 0) return s.lo;
    return s.lo + (s.hi - s.lo) * ((double)pos / (double)s.steps);
}

int value_to_pos(const slider_def & s, double v) {
    if (s.hi <= s.lo) return 0;
    double t = (v - s.lo) / (s.hi - s.lo);
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return (int)(t * (double)s.steps + 0.5);
}

struct dialog_state {
    dsp_preset_edit_callback * callback;
    Params                     params;
};

void refresh_label(HWND dlg, const slider_def & s, double v) {
    wchar_t text[48];
    _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.*f%s", s.decimals, v, s.suffix);
    SetDlgItemTextW(dlg, s.valueId, text);
}

void push_preset(dialog_state * st) {
    dsp_preset_impl preset;
    declick_preset::make(st->params, preset);
    st->callback->on_preset_changed(preset);
}

void load_controls(HWND dlg, dialog_state * st) {
    for (size_t i = 0; i < k_count; ++i) {
        const double v = get_param(st->params, i);
        SendDlgItemMessage(dlg, k_sliders[i].sliderId, TBM_SETPOS, TRUE,
                           (LPARAM)value_to_pos(k_sliders[i], v));
        refresh_label(dlg, k_sliders[i], v);
    }
    CheckDlgButton(dlg, IDC_DELTA, st->params.delta ? BST_CHECKED : BST_UNCHECKED);
}

//! One tooltip control owned by the dialog, with the label, the slider and the
//! readout of a row all registered against that row's text, so the hint comes
//! up wherever on the row the pointer lands. TTF_SUBCLASS has the tooltip pick
//! the mouse messages up itself: a modal DialogBoxParam loop has nowhere to
//! hang a TTM_RELAYEVENT forwarder.
void add_tooltip(HWND tips, HWND dlg, int id, const wchar_t * text) {
    const HWND control = GetDlgItem(dlg, id);
    if (control == NULL) return;

    TTTOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize   = TTTOOLINFOW_V2_SIZE;  // accepted by both comctl32 v5 and v6
    ti.uFlags   = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd     = dlg;
    ti.uId      = (UINT_PTR)control;
    ti.lpszText = (LPWSTR)text;
    SendMessage(tips, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

void create_tooltips(HWND dlg) {
    const HWND tips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL,
                                      WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      CW_USEDEFAULT, CW_USEDEFAULT,
                                      dlg, NULL, core_api::get_my_instance(), NULL);
    if (tips == NULL) return;

    // Without a maximum width a tooltip is laid out on a single line, and these
    // run to several sentences. The pop-up timeout goes up to match: the
    // default takes the longest of them away before it can be read.
    SendMessage(tips, TTM_SETMAXTIPWIDTH, 0, 360);
    SendMessage(tips, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELPARAM(30000, 0));

    for (size_t i = 0; i < k_count; ++i) {
        const int row[3] = { k_sliders[i].labelId,
                             k_sliders[i].sliderId,
                             k_sliders[i].valueId };
        for (int k = 0; k < 3; ++k) add_tooltip(tips, dlg, row[k], k_sliders[i].help);
    }
    add_tooltip(tips, dlg, IDC_DELTA, k_delta_help);
}

INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    dialog_state * st = (dialog_state *)GetWindowLongPtr(dlg, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG:
        st = (dialog_state *)lp;
        SetWindowLongPtr(dlg, GWLP_USERDATA, (LONG_PTR)st);
        for (size_t i = 0; i < k_count; ++i) {
            SendDlgItemMessage(dlg, k_sliders[i].sliderId, TBM_SETRANGE, FALSE,
                               MAKELPARAM(0, k_sliders[i].steps));
            SendDlgItemMessage(dlg, k_sliders[i].sliderId, TBM_SETPAGESIZE, 0,
                               (LPARAM)(k_sliders[i].steps > 20
                                        ? k_sliders[i].steps / 20 : 1));
            SendDlgItemMessage(dlg, k_sliders[i].sliderId, TBM_SETLINESIZE, 0, 1);
        }
        load_controls(dlg, st);
        create_tooltips(dlg);
        return TRUE;

    case WM_HSCROLL: {
        if (st == NULL || lp == 0) break;
        const HWND bar = (HWND)lp;
        const int id = GetDlgCtrlID(bar);
        for (size_t i = 0; i < k_count; ++i) {
            if (k_sliders[i].sliderId != id) continue;
            const int pos = (int)SendMessage(bar, TBM_GETPOS, 0, 0);
            const double v = pos_to_value(k_sliders[i], pos);
            if (get_param(st->params, i) != v) {
                set_param(st->params, i, v);
                refresh_label(dlg, k_sliders[i], v);
                push_preset(st);
            }
            break;
        }
        return TRUE;
    }

    case WM_COMMAND:
        if (st == NULL) break;
        switch (LOWORD(wp)) {
        case IDC_DELTA:
            if (HIWORD(wp) == BN_CLICKED) {
                st->params.delta = (IsDlgButtonChecked(dlg, IDC_DELTA) == BST_CHECKED);
                push_preset(st);
                return TRUE;
            }
            break;
        case IDC_DEFAULTS:
            if (HIWORD(wp) == BN_CLICKED) {
                st->params = Params::defaults();
                load_controls(dlg, st);
                push_preset(st);
                return TRUE;
            }
            break;
        case IDOK:
            if (HIWORD(wp) == BN_CLICKED) { EndDialog(dlg, IDOK); return TRUE; }
            break;
        case IDCANCEL:
            if (HIWORD(wp) == BN_CLICKED) { EndDialog(dlg, IDCANCEL); return TRUE; }
            break;
        }
        break;

    case WM_CLOSE:
        EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

} // anonymous namespace

void declick_config_popup(const dsp_preset & data, HWND parent,
                          dsp_preset_edit_callback & callback) {
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    dialog_state st;
    st.callback = &callback;
    st.params = declick_preset::parse(data);

    const INT_PTR result = DialogBoxParam(core_api::get_my_instance(),
                                          MAKEINTRESOURCE(IDD_DECLICK),
                                          parent, dialog_proc, (LPARAM)&st);
    if (result != IDOK) {
        callback.on_preset_changed(data);
    }
}
