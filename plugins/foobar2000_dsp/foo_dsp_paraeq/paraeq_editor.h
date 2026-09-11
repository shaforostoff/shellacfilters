/* ========================================
 *  foo_dsp_paraeq - the curve editor, shared by both places it appears
 *
 *  One child window, owner-drawn, used unchanged by the modal dialog behind
 *  Preferences -> DSP -> Configure and by the Default UI element you drop into
 *  a layout. The two hosts differ in where a change goes - one to a
 *  dsp_preset_edit_callback, the other to the live DSP chain - and in nothing
 *  else, so that difference is the whole of the Host interface below.
 *
 *  Why owner-drawn rather than a dialog template full of trackbars, which is
 *  what the sibling components use. Three reasons, in order of weight:
 *
 *  1. The element has to live in a layout at whatever size it is dragged to, in
 *     light mode or dark. Standard controls do not follow foobar2000's dark
 *     mode without the SDK's DarkMode helpers, and those live in helpers/ and
 *     libPPUI/, which this project deliberately does not build - see the
 *     README. Painting it ourselves means asking the host for two colours and
 *     being correct in both themes, and in whatever a later one does.
 *
 *  2. Sixteen controls is too many sliders for a panel and exactly right for a
 *     curve with five handles on it, which is also how the work is thought
 *     about: nobody decides to set a peaking filter to 1 kHz, they pull the
 *     boxiness out of wherever it turns out to be.
 *
 *  3. A trackbar per parameter cannot show what the parameters add up to, and
 *     the sum is the thing being listened for.
 *
 *  Everything is reachable three ways, because the mouse is not always the
 *  right instrument and is never the accessible one:
 *
 *    Mouse     drag a handle for frequency and gain, wheel for Q or slope,
 *              right-click for the shape switches, double-click to zero a band.
 *              The readout cells under the curve drag vertically too, which is
 *              how to set a value one can name rather than one one can see.
 *    Keyboard  left and right move the selected band's frequency, up and down
 *              its gain, page up and down its Q, and ctrl with left and right
 *              moves the selection along the strip. See onKey() for the table
 *              and for why the selection is the thing on the modifier.
 *    Readout   every value is written out under the curve, so what a drag did
 *              is legible without moving the pointer off it.
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_EDITOR_H
#define FOO_DSP_PARAEQ_EDITOR_H

#include "paraeq_core.h"

#include <vector>

namespace paraeq_editor {

//! What the editor needs from whoever put it on screen. Both implementations
//! are a dozen lines; everything else the editor does for itself.
class Host {
public:
    //! The user moved something. Already sanitised. Main thread, throttled to
    //! one call per kPushIntervalMs during a drag plus one when it ends, so an
    //! implementation may treat it as expensive.
    virtual void editorParamsChanged(const paraeq::Params & p) = 0;

    //! False when the equaliser is not in the DSP chain at all, which the
    //! editor draws dimmed and says so in the footer. The modal dialog is
    //! always engaged - it is editing a preset that is in the chain by
    //! construction - so it takes the default.
    virtual bool editorEngaged() { return true; }

    //! The user asked for it to be put into the chain, through the footer
    //! button that only appears while editorEngaged() is false.
    virtual void editorEngage() {}

    //! A foobar2000 theme colour, e.g. ui_color_background. False to fall back
    //! to the system colours, which is what the modal dialog does.
    virtual bool editorColor(const GUID & what, COLORREF & out) {
        (void)what; (void)out; return false;
    }

    //! The font to draw with. NULL for the system default.
    virtual HFONT editorFont() { return NULL; }

    //! True while the host has layout editing mode switched on. The editor
    //! stops taking mouse and keyboard input then: in that mode a click on an
    //! element means move it or replace it, and an equaliser that changed its
    //! own settings while the layout was being rearranged would be a trap.
    virtual bool editorLayoutEditMode() { return false; }

protected:
    ~Host() {}
};

//! Registers the window class. Idempotent; main thread.
bool registerClass();

//! One editor and its window. The window is created by create() and destroyed
//! by the destructor if the host has not destroyed it already, which is the
//! contract ui_element_instance asks for.
class Editor {
public:
    explicit Editor(Host & host);
    ~Editor();

    Editor(const Editor &) = delete;
    void operator=(const Editor &) = delete;

    //! Creates the child window. Returns NULL on failure.
    HWND create(HWND parent, const RECT & rc, UINT id, const paraeq::Params & initial);

    HWND wnd() const { return m_wnd; }
    const paraeq::Params & params() const { return m_params; }

    //! Settings arrived from somewhere else - the chain changed under us, or a
    //! second copy of this editor is open. Ignored while the user is dragging,
    //! because a push of our own coming back round must not yank the handle out
    //! from under the pointer.
    void setParams(const paraeq::Params & p);

    //! The host's colours or font changed; drop whatever was derived from them.
    void refreshTheme();

    //! Whether the equaliser is in the chain has changed.
    void refreshEngaged();

    //! Rate to draw the curve for. Only the top of the plot can tell the
    //! difference, but that is where the hiss shelf lives.
    void setSampleRate(double rate);

    //! The window procedure, public only because registerClass() has to take
    //! its address. Nothing else should call it.
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Layout {
        RECT curve;     //!< the graph
        RECT strip;     //!< the readouts, empty when there is no room for them
        RECT foot;      //!< the buttons, empty when there is no room for them
        int  cellW;     //!< strip column width
        int  rowH;      //!< strip row height

        //! One per footer button, empty for a button that is not shown or does
        //! not fit. Measured in layout() rather than where they are painted,
        //! because hit testing needs the same rectangles and doing it there
        //! would mean a device context and a text measurement on every mouse
        //! move just to decide whether the pointer is over one.
        RECT button[4];

        //! How wide each band's two names come out in the host's font. Measured
        //! in layout() for the same reason the button rectangles are: the curve
        //! is repainted on every step of a drag and placing the labels needs
        //! these on every one of them. Indexed by band; 6 is the band count,
        //! which lives in the .cpp because nothing out here has any business
        //! knowing what the bands are.
        int  nameW[6];
        int  abbrW[6];

        //! True when a readout column is wide enough for the longest of the
        //! long names, so the header row can use them. Decided for the row
        //! rather than per band: one reading Bass / LMF / Brilliance would look
        //! like a mistake rather than like a fit.
        bool longNames;
    };

    // -- painting ----------------------------------------------------------
    void   layout();
    void   rebuildTable();      //!< the log frequency axis and its trig table
    void   markCurveStale();
    void   ensureCurve();
    void   paint(HDC dc, const RECT & client);
    void   paintCurve(HDC dc);
    void   paintStrip(HDC dc);
    void   paintFoot(HDC dc);
    void   theme();             //!< resolve colours and font from the host
    void   releaseBuffer();

    int    xOfHz(double hz) const;
    double hzOfX(int x) const;
    int    yOfDb(double db) const;
    double dbOfY(int y) const;
    int    handleX(int band) const;
    int    handleY(int band) const;
    int    handleRadius() const;

    // -- interaction -------------------------------------------------------
    int  hitHandle(POINT pt) const;             //!< band index, or -1
    int  hitStripCell(POINT pt, int * row) const;
    int  hitFootButton(POINT pt) const;

    void onMouseDown(POINT pt, bool doubleClick);
    void onMouseMove(POINT pt);
    void onMouseUp();
    void onWheel(POINT screenPt, int delta);
    void onContextMenu(POINT screenPt);
    bool onKey(WPARAM key);

    void select(int band);
    void nudgeGain(int band, double db);
    void nudgeFreq(int band, double semitones);
    void nudgeQ(int band, int steps);
    void toggleShape(int band);
    void resetBand(int band);
    void commit();              //!< sanitise, redraw, schedule a push
    void schedulePush();
    void flushPush();
    bool editable() const;       //!< false while the host is in layout edit mode

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);

    Host &         m_host;
    HWND           m_wnd = NULL;
    paraeq::Params m_params;

    Layout m_layout = {};
    int    m_dpi    = 96;
    double m_sampleRate = 44100.0;

    // Colours resolved from the host, so that dark mode needs no code of its
    // own, plus the font to draw them with.
    COLORREF m_colBack = 0, m_colText = 0, m_colGrid = 0, m_colDim = 0,
             m_colCurve = 0, m_colSel = 0, m_colFill = 0;
    HFONT    m_font = NULL;

    // The frequency axis, its trig table, and the curve read off them. All
    // three depend on the width and not on the parameters - see paraeq_core.h.
    std::vector<double> m_hz;
    std::vector<double> m_trig;
    std::vector<float>  m_db;       //!< the cascade
    std::vector<float>  m_dbBand;   //!< the band under the pointer, if any
    std::vector<float>  m_dbBand2;  //!< the high-pass's second section
    std::vector<POINT>  m_poly;
    std::vector<POINT>  m_polyBand;
    bool m_curveStale = true;

    // Double buffer, kept between paints and resized with the window: the
    // alternative is creating and destroying a screen-sized bitmap on every
    // frame of a drag.
    HDC     m_memDC  = NULL;
    HBITMAP m_memBmp = NULL;
    HBITMAP m_memOld = NULL;
    int     m_memW = 0, m_memH = 0;

    int   m_selected = -1;      //!< keyboard and readout selection
    int   m_hover    = -1;      //!< handle under the pointer
    int   m_dragBand = -1;
    int   m_dragRow  = -1;      //!< strip row being dragged, -1 for a handle
    POINT m_dragFrom = {};
    //! Every drag works from where it started rather than from where the last
    //! mouse message left things, so that a handle stays exactly under the
    //! pointer and holding ctrl can scale the whole delta rather than switching
    //! into a different mode part way through.
    paraeq::Params m_dragStart;
    bool  m_dragging = false;
    bool  m_tracking = false;   //!< TrackMouseEvent is armed
    bool  m_engaged  = true;
    int   m_footHot  = -1;

    bool  m_pushDirty = false;
    bool  m_pushTimer = false;
    DWORD m_lastPush  = 0;
};

} // namespace paraeq_editor

#endif // FOO_DSP_PARAEQ_EDITOR_H
