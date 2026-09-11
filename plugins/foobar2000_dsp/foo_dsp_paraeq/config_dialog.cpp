/* ========================================
 *  foo_dsp_paraeq - configuration dialog
 *
 *  What Preferences -> DSP -> Configure opens. Plain Win32, same as the other
 *  three components: no ATL, no WTL, no libPPUI, so the component builds from
 *  nothing but the foobar2000 SDK archive.
 *
 *  It is a thin frame round paraeq_editor: a placeholder control in the
 *  template says where the editor goes, three buttons say what to do when it is
 *  finished, and everything between those is the editor's. The UI element in
 *  ui_element.cpp is the same editor with a different fifteen lines round it.
 * ======================================== */

#include "stdafx.h"

#include "paraeq_editor.h"
#include "paraeq_preset.h"
#include "resource.h"

#include <memory>

using paraeq::Params;

namespace {

class dialog_host : public paraeq_editor::Host {
public:
    dialog_host(dsp_preset_edit_callback & callback, HWND dlg)
        : m_callback(callback), m_dlg(dlg) {}

    void editorParamsChanged(const Params & p) override {
        dsp_preset_impl preset;
        paraeq_preset::make(p, preset);
        m_callback.on_preset_changed(preset);
    }

    //! The dialog's own font, so the editor's readouts match its buttons. The
    //! colours are deliberately not answered: a modal dialog is drawn in system
    //! colours, and the editor falls back to those when asked and refused.
    HFONT editorFont() override {
        return (HFONT)SendMessage(m_dlg, WM_GETFONT, 0, 0);
    }

private:
    dsp_preset_edit_callback & m_callback;
    HWND                       m_dlg;
};

struct dialog_state {
    dsp_preset_edit_callback *              callback;
    Params                                  params;
    std::unique_ptr<dialog_host>            host;
    std::unique_ptr<paraeq_editor::Editor>  editor;
};


//! Puts the editor over the placeholder control and hides it. Keeping the
//! rectangle in the template rather than in code means the layout is visible in
//! one place - the .rc - instead of split between two.
bool build_editor(HWND dlg, dialog_state * st)
{
    const HWND placeholder = GetDlgItem(dlg, IDC_EDITOR_PLACEHOLDER);
    if (placeholder == NULL) return false;

    RECT rc;
    GetWindowRect(placeholder, &rc);
    MapWindowPoints(NULL, dlg, (POINT *)&rc, 2);
    ShowWindow(placeholder, SW_HIDE);

    st->host.reset(new dialog_host(*st->callback, dlg));
    st->editor.reset(new paraeq_editor::Editor(*st->host));

    if (st->editor->create(dlg, rc, IDC_EDITOR, st->params) == NULL) {
        st->editor.reset();
        st->host.reset();
        return false;
    }
    return true;
}


INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    dialog_state * st = (dialog_state *)GetWindowLongPtr(dlg, GWLP_USERDATA);

    switch (msg) {
    case WM_INITDIALOG:
        st = (dialog_state *)lp;
        SetWindowLongPtr(dlg, GWLP_USERDATA, (LONG_PTR)st);
        if (build_editor(dlg, st) && st->editor->wnd() != NULL) {
            SetFocus(st->editor->wnd());
            return FALSE;       // focus placed by hand, so do not place it again
        }
        return TRUE;

    case WM_COMMAND:
        if (st == NULL) break;
        switch (LOWORD(wp)) {
        case IDC_DEFAULTS:
            if (HIWORD(wp) == BN_CLICKED && st->editor) {
                st->editor->setParams(Params::defaults());
                dsp_preset_impl preset;
                paraeq_preset::make(Params::defaults(), preset);
                st->callback->on_preset_changed(preset);
                SetFocus(st->editor->wnd());
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

    case WM_DESTROY:
        // Before the dialog_state on the caller's stack goes out of scope, and
        // while the window tree is still standing.
        if (st != NULL) { st->editor.reset(); st->host.reset(); }
        return FALSE;
    }
    return FALSE;
}

} // anonymous namespace


void paraeq_config_popup(const dsp_preset & data, HWND parent,
                         dsp_preset_edit_callback & callback)
{
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    dialog_state st;
    st.callback = &callback;
    st.params = paraeq_preset::parse(data);

    const INT_PTR result = DialogBoxParam(core_api::get_my_instance(),
                                          MAKEINTRESOURCE(IDD_PARAEQ),
                                          parent, dialog_proc, (LPARAM)&st);
    if (result != IDOK) {
        callback.on_preset_changed(data);
    }
}
