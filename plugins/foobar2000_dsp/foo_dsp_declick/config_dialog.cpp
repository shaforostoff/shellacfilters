/* ========================================
 *  foo_dsp_declick - configuration dialog
 *
 *  Plain Win32, same as foo_dsp_decrackle: no ATL, no WTL, no libPPUI, so the
 *  component builds from nothing but the foobar2000 SDK archive.
 * ======================================== */

#include "stdafx.h"

#include "declick_preset.h"
#include "resource.h"

#include <math.h>
#include <stdio.h>

using declick::Params;

namespace {

//! Each slider works in integer steps and maps onto its parameter through
//! `lo + pos * step`, or, where `choices` is set, picks choices[pos]. Keeping
//! the mapping in a table lets one handler drive all of them.
//!
//! Only the controls Params::restrictToControls() keeps are here: the stages
//! after the AR repair were tuned with the rest at their defaults.
//!
//! `help` is the hover text, shared word for word with the macOS view; it is
//! condensed from the rationale in declick_core.h, which has the measurements
//! behind it.
struct slider_def {
    int    sliderId;
    int    labelId;
    int    valueId;
    int    steps;        // slider range is 0..steps
    double lo, hi;       // parameter range
    int    decimals;     // for the readout
    const wchar_t * suffix;
    const double *  choices;   // steps + 1 values, or NULL for a linear slider
    const wchar_t * help;
};

const double k_orders[] = { 64.0, 128.0, 256.0 };

const slider_def k_sliders[] = {
    { IDC_SLIDER_SENSITIVITY, IDC_LABEL_SENSITIVITY, IDC_VALUE_SENSITIVITY, 1000,  0.0,  1.0, 3, L"", NULL,
      L"How readily a sample is called a click. The default, 0.9, puts the trigger at 2.85 sigma "
      L"above the local noise estimate. Detections near the trigger are only partly repaired - "
      L"they are as likely noise or music as clicks - and full repair is kept for clear ones. "
      L"Past the default, music starts being flagged as damage faster than crackle is found. On "
      L"clean records 0.7-0.8 takes less still." },

    { IDC_SLIDER_ORDER,       IDC_LABEL_ORDER,       IDC_VALUE_ORDER,          2, 64.0, 256.0, 0, L"", k_orders,
      L"Taps in the autoregressive model that predicts what the waveform should have been. 128 is "
      L"the balance. 256 models the music better and changes clean records least, but misses more "
      L"of the short clicks on heavily crackled transfers. 64 costs about 0.6x the CPU of 128 and "
      L"gives up surprisingly little - the one to pick on a slow machine; 256 costs about 2.4x." },

    { IDC_SLIDER_DRYWET,      IDC_LABEL_DRYWET,      IDC_VALUE_DRYWET,      1000,  0.0,  1.0, 3, L"", NULL,
      L"Blend of the repaired signal against the original. 0 passes the input through "
      L"untouched." },
};

const size_t k_count = sizeof(k_sliders) / sizeof(k_sliders[0]);

const wchar_t * const k_cautious_help =
    L"Repairs only the clicks the detector flags, and turns off every stage that works "
    L"on the audio around them: the click-shape fit, the crackle body, top-band and mid "
    L"stages, the flank repair and the crackle floor. Those take the carpet of crackle "
    L"under the clicks, and on some records a little of the music with it. With this on "
    L"the music is left as it is, and more crackle stays. Uses less CPU.";

const wchar_t * const k_delta_help =
    L"Outputs what the repair takes out - the input minus the output - instead of the "
    L"repaired audio. For testing: it should be crackle and clicks and nothing else, and "
    L"any music heard in it is being damaged. Saved with the preset; untick it when done.";

double get_param(const Params & p, size_t i) {
    switch (i) {
    case 0: return p.sensitivity;
    case 1: return (double)p.order;
    default: return p.dryWet;
    }
}

void set_param(Params & p, size_t i, double v) {
    switch (i) {
    case 0: p.sensitivity = (float)v; break;
    case 1: p.order = (int)(v + 0.5); break;
    default: p.dryWet = (float)v; break;
    }
}

double pos_to_value(const slider_def & s, int pos) {
    if (s.steps <= 0) return s.lo;
    if (s.choices) {
        if (pos < 0) pos = 0;
        if (pos > s.steps) pos = s.steps;
        return s.choices[pos];
    }
    return s.lo + (s.hi - s.lo) * ((double)pos / (double)s.steps);
}

int value_to_pos(const slider_def & s, double v) {
    if (s.choices) {
        int best = 0;
        for (int k = 1; k <= s.steps; ++k) {
            if (fabs(s.choices[k] - v) < fabs(s.choices[best] - v)) best = k;
        }
        return best;
    }
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
    CheckDlgButton(dlg, IDC_CAUTIOUS, st->params.cautious ? BST_CHECKED : BST_UNCHECKED);
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
    add_tooltip(tips, dlg, IDC_CAUTIOUS, k_cautious_help);
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
        case IDC_CAUTIOUS:
            if (HIWORD(wp) == BN_CLICKED) {
                st->params.cautious = (IsDlgButtonChecked(dlg, IDC_CAUTIOUS) == BST_CHECKED);
                push_preset(st);
                return TRUE;
            }
            break;
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
