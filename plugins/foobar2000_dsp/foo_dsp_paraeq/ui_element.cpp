/* ========================================
 *  foo_dsp_paraeq - the Default UI element
 *
 *  What "Enable layout editing mode" offers under Add new UI element -> DSP,
 *  and what the View menu opens as a window of its own. Same editor as the
 *  modal dialog; what differs is where a move goes and where one comes from.
 *
 *  The dialog edits a dsp_preset that foobar2000 hands it and hands back. This
 *  edits the live DSP chain, and that turns three questions into design:
 *
 *  1. Where the settings live. Not here: the element holds no configuration of
 *     its own, and get_configuration() returns an empty block. The equaliser is
 *     a property of the playback chain rather than of a panel, so two copies of
 *     this element in one layout show one equaliser, and a layout saved and
 *     restored comes back pointed at whatever the chain holds then. A per-panel
 *     copy of the settings would be a second source of truth for something
 *     there is only one of.
 *
 *  2. How the copies stay level. Through the chain itself, not through each
 *     other: every instance registers with the watcher below, which is a
 *     dsp_config_callback, so a change made in one panel, in the Preferences
 *     dialog, or by any other component reaches all of them the same way and
 *     by the same route. The editor ignores what arrives mid-drag - see
 *     Editor::setParams.
 *
 *  3. What to do when the equaliser is not in the chain. Draw it anyway, dimmed,
 *     with the settings the chain last held and a button to put it back. An
 *     element that showed nothing until the DSP was enabled somewhere else
 *     would be a panel that is blank exactly when a user is looking for the
 *     thing to press.
 * ======================================== */

#include "stdafx.h"

#include "paraeq_editor.h"
#include "paraeq_preset.h"

#include <memory>
#include <vector>

using paraeq::Params;

namespace {

class paraeq_element_instance;

//! Identity of the element in a saved layout. Distinct from the DSP's own GUID:
//! one names a panel, the other names a link in the playback chain, and a
//! layout referring to the DSP by its DSP identity would be a category error.
GUID element_guid() {
    // {9833DE78-90A8-4700-8BCD-033E32FAB171}
    static const GUID g =
        { 0x9833de78, 0x90a8, 0x4700, { 0x8b, 0xcd, 0x03, 0x3e, 0x32, 0xfa, 0xb1, 0x71 } };
    return g;
}

//! Every live element, main thread only - which is where ui_element_instance,
//! dsp_config_callback and play_callback all live, so no lock is needed and one
//! would be a claim that they do not.
std::vector<paraeq_element_instance *> g_live;

double g_sampleRate = 44100.0;   //!< what the curve is drawn for; see below

//! The rate the equaliser is most likely running at. Only the top of the plot
//! can tell 44.1 from 48 - the shelves are placed by the bilinear transform,
//! which warps most near Nyquist - but the hiss shelf is exactly what lives up
//! there, so it is worth being right about.
double rate_of(const metadb_handle_ptr & track) {
    if (track.is_valid()) {
        metadb_info_container::ptr info;
        if (track->get_info_ref(info) && info.is_valid()) {
            const t_int64 sr = info->info().info_get_int("samplerate");
            if (sr > 8000 && sr < 10000000) return (double)sr;
        }
    }
    return 44100.0;
}

// ---------------------------------------------------------------------------
// The element instance
// ---------------------------------------------------------------------------

class paraeq_element_instance : public ui_element_instance,
                                public paraeq_editor::Host {
public:
    paraeq_element_instance(ui_element_config::ptr, ui_element_instance_callback_ptr cb)
        : m_callback(cb), m_editor(*this)
    {
        m_engaged = paraeq_preset::core_read(m_params);
    }

    //! Split out of the constructor because the window has to exist by the time
    //! instantiate() returns, and a service_impl_t is not fully built until its
    //! constructor has.
    void initialize_window(HWND parent) {
        // Zero sized: the host positions the window the moment it has it, and
        // giving it a size here would only make it flash at that one.
        RECT rc = { 0, 0, 0, 0 };
        m_editor.create(parent, rc, 0, m_params);
        m_editor.setSampleRate(g_sampleRate);
        g_live.push_back(this);
    }

    ~paraeq_element_instance() {
        for (size_t i = 0; i < g_live.size(); ++i) {
            if (g_live[i] == this) { g_live.erase(g_live.begin() + i); break; }
        }
    }

    // -- ui_element_instance -------------------------------------------------

    fb2k::hwnd_t get_wnd() override { return m_editor.wnd(); }

    //! Nothing to store - see the note at the top of the file.
    void set_configuration(ui_element_config::ptr) override {}
    ui_element_config::ptr get_configuration() override {
        return ui_element_config::g_create_empty(get_guid());
    }

    GUID get_guid() override { return element_guid(); }
    GUID get_subclass() override { return ui_element_subclass_dsp; }

    ui_element_min_max_info get_min_max_info() override {
        // Below this the curve stops being readable rather than merely small,
        // and the layout editor should refuse the drag instead of producing a
        // panel with nothing in it. The maximum is left alone: a full-height
        // equaliser is a reasonable thing to want.
        const int dpi = dpiY();
        ui_element_min_max_info info;
        info.m_min_width  = (t_uint32)MulDiv(200, dpi, 96);
        info.m_min_height = (t_uint32)MulDiv(90,  dpi, 96);
        return info;
    }

    void notify(const GUID & what, t_size, const void *, t_size) override {
        if (what == ui_element_notify_colors_changed ||
            what == ui_element_notify_font_changed)
        {
            m_editor.refreshTheme();
        } else if (what == ui_element_notify_edit_mode_changed) {
            // The editor stops taking input in edit mode, and says so by
            // redrawing without the focus ring.
            if (m_editor.wnd()) InvalidateRect(m_editor.wnd(), NULL, FALSE);
        }
    }

    // -- paraeq_editor::Host -------------------------------------------------

    void editorParamsChanged(const Params & p) override {
        m_params = p;
        // insertIfAbsent false: moving a control on a disengaged equaliser sets
        // it up for later rather than silently switching it into the chain. The
        // footer button is the one thing that does that, and it says so.
        paraeq_preset::core_write(p, false);
    }

    bool editorEngaged() override { return m_engaged; }

    void editorEngage() override {
        paraeq_preset::core_write(m_params, true);
        m_engaged = true;
    }

    //! The editor names the three colours it wants in its own terms, because
    //! it has a host with no ui_color_* to answer with as well as this one.
    //! Mapping them onto the SDK's GUIDs is a foobar2000 problem and so lives
    //! on this side of the interface.
    bool editorColor(paraeq_editor::Color what, COLORREF & out) override {
        if (!m_callback.is_valid()) return false;

        GUID which;
        switch (what) {
            case paraeq_editor::kColorBackground: which = ui_color_background; break;
            case paraeq_editor::kColorText:       which = ui_color_text;       break;
            case paraeq_editor::kColorSelection:  which = ui_color_selection;  break;
            default: return false;
        }

        t_ui_color col = 0;
        if (!m_callback->query_color(which, col)) return false;
        out = (COLORREF)col;
        return true;
    }

    HFONT editorFont() override {
        if (!m_callback.is_valid()) return NULL;
        return (HFONT)m_callback->query_font_ex(ui_font_default);
    }

    bool editorLayoutEditMode() override {
        return m_callback.is_valid() && m_callback->is_edit_mode_enabled();
    }

    // -- told from outside ---------------------------------------------------

    void onChainChanged(const Params & p, bool engaged) {
        m_params  = p;
        m_engaged = engaged;
        m_editor.setParams(p);
        m_editor.refreshEngaged();
    }

    void onSampleRate(double rate) { m_editor.setSampleRate(rate); }

    void bumpFocus() {
        const HWND wnd = m_editor.wnd();
        if (wnd != NULL) SetFocus(wnd);
    }

private:
    int dpiY() const {
        int dpi = 96;
        const HDC dc = GetDC(NULL);
        if (dc != NULL) {
            const int y = GetDeviceCaps(dc, LOGPIXELSY);
            if (y > 0) dpi = y;
            ReleaseDC(NULL, dc);
        }
        return dpi;
    }

    ui_element_instance_callback_ptr m_callback;
    paraeq_editor::Editor            m_editor;
    Params                           m_params = Params::defaults();
    bool                             m_engaged = false;
};

// ---------------------------------------------------------------------------
// The element
// ---------------------------------------------------------------------------

class paraeq_element : public ui_element_v2 {
public:
    GUID get_guid() override { return element_guid(); }

    GUID get_subclass() override { return ui_element_subclass_dsp; }

    void get_name(pfc::string_base & out) override { out = "Parametric EQ"; }

    bool get_description(pfc::string_base & out) override {
        out = "Curve editor for the Parametric EQ DSP: high-pass, low shelf, "
              "two peaking bands, high shelf and output trim. Edits the DSP "
              "chain directly, so it shows and changes the same settings as "
              "Preferences / Playback / DSP Manager.";
        return true;
    }

    ui_element_instance_ptr instantiate(fb2k::hwnd_t parent,
                                        ui_element_config::ptr cfg,
                                        ui_element_instance_callback_ptr cb) override
    {
        service_ptr_t<paraeq_element_instance> item =
            new service_impl_t<paraeq_element_instance>(cfg, cb);
        item->initialize_window(parent);
        return item;
    }

    ui_element_config::ptr get_default_configuration() override {
        return ui_element_config::g_create_empty(get_guid());
    }

    //! Not a container.
    ui_element_children_enumerator_ptr enumerate_children(ui_element_config::ptr) override {
        return NULL;
    }

    t_uint32 get_flags() override {
        // A menu command in View, and bump() to bring an existing panel forward
        // rather than opening a second window onto the same equaliser.
        return KFlagHavePopupCommand | KFlagSupportsBump;
    }

    bool bump() override {
        if (g_live.empty()) return false;
        g_live.back()->bumpFocus();
        return true;
    }

    bool get_popup_specs(ui_size & defSize, pfc::string_base & title) override {
        defSize.cx = 460;
        defSize.cy = 300;
        title = "Parametric EQ";
        return true;
    }
};

static service_factory_single_t<paraeq_element> g_paraeq_element_factory;

// ---------------------------------------------------------------------------
// Keeping every panel level with the chain
// ---------------------------------------------------------------------------

class paraeq_chain_watch : public dsp_config_callback {
public:
    void on_core_settings_change(const dsp_chain_config & chain) override {
        if (g_live.empty()) return;

        // Read straight out of what arrived rather than asking
        // dsp_config_manager again: the callback is explicitly not a place to
        // touch the DSP settings, and this way there is nothing to be reentrant
        // about.
        Params p = Params::defaults();
        bool engaged = false;
        for (t_size i = 0; i < chain.get_count(); ++i) {
            if (chain.get_item(i).get_owner() == paraeq_preset::guid()) {
                p = paraeq_preset::parse(chain.get_item(i));
                engaged = true;
                break;
            }
        }

        // A copy, because an element's editor could in principle change the
        // chain from under the loop.
        std::vector<paraeq_element_instance *> live = g_live;
        for (size_t i = 0; i < live.size(); ++i) live[i]->onChainChanged(p, engaged);
    }
};

static service_factory_single_t<paraeq_chain_watch> g_paraeq_chain_watch_factory;

// ---------------------------------------------------------------------------
// Keeping the curve honest about the sample rate
// ---------------------------------------------------------------------------

class paraeq_rate_watch : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_stop;
    }

    void on_playback_new_track(metadb_handle_ptr track) override {
        set_rate(rate_of(track));
    }

    void on_playback_stop(play_control::t_stop_reason) override {
        set_rate(44100.0);
    }

    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info &) override {}
    void on_playback_dynamic_info_track(const file_info &) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}

private:
    static void set_rate(double rate) {
        if (rate == g_sampleRate) return;
        g_sampleRate = rate;
        for (size_t i = 0; i < g_live.size(); ++i) g_live[i]->onSampleRate(rate);
    }
};

static play_callback_static_factory_t<paraeq_rate_watch> g_paraeq_rate_watch_factory;

} // anonymous namespace
