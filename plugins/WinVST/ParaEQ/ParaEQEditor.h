/* ========================================
 *  ParaEQ - ParaEQEditor.h
 *
 *  The VST2 editor: a curve with five handles on it, rather than sixteen
 *  sliders in a host's generic list. It is the same editor the foobar2000
 *  component draws - paraeq_editor.{h,cpp}, mirrored into this folder by
 *  scripts/sync_cores.ps1 - because that file is plain Win32 with no SDK in it
 *  and there was never a second one worth writing.
 *
 *  So this class is only the join between two interfaces, and is deliberately
 *  thin:
 *
 *    AEffEditor          what the host asks - how big, here is a parent, take
 *                        it away, and a periodic ping while it is up.
 *    paraeq_editor::Host what the editor asks - where a change goes, what
 *                        colours to draw in, what font.
 *
 *  Everything either side does for itself. What is left in the middle is the
 *  one thing neither can know: that a VST parameter is a float in 0..1 and a
 *  paraeq::Params is not, so every move has to cross the mapping in ParaEQ.cpp
 *  in one direction or the other. Crossing it in both directions is also the
 *  only real hazard here - see m_synced.
 * ======================================== */

#ifndef __ParaEQEditor_H
#define __ParaEQEditor_H

#ifndef __audioeffect__
#include "audioeffectx.h"
#endif

#include "paraeq_core.h"
#include "paraeq_editor.h"

class ParaEQ;

class ParaEQEditor : public AEffEditor, public paraeq_editor::Host
{
public:
    explicit ParaEQEditor(ParaEQ & plugin);
    ~ParaEQEditor();

    ParaEQEditor(const ParaEQEditor &) = delete;
    void operator=(const ParaEQEditor &) = delete;

    // -- AEffEditor ---------------------------------------------------------

    bool getRect(ERect ** rect);
    bool open(void * ptr);
    void close();
    void idle();
    bool isOpen();

    // -- paraeq_editor::Host ------------------------------------------------

    void editorParamsChanged(const paraeq::Params & p);

    //! A VST is in the chain by construction - the host would not be showing
    //! this window otherwise - so the footer's "Add to DSP chain" button never
    //! appears and Bypass, Flatten and Reset are the whole of it. That button
    //! is the one thing in the editor that is foobar2000-shaped, and taking the
    //! default is how it goes away.
    bool editorEngaged() { return true; }

    //! System colours. A VST2 host has no theme to ask about: there is no
    //! equivalent of ui_color_background in the ABI, and a host that skins its
    //! own windows has no way to tell a plug-in about it. Answering false gets
    //! GetSysColor(), which follows the user's Windows theme and is the best
    //! answer available. Everything else the editor draws is blended from these
    //! three, so it is coherent in light and dark without either side knowing
    //! which it is in.
    bool editorColor(paraeq_editor::Color what, COLORREF & out)
    { (void)what; (void)out; return false; }

    //! NULL, which the editor reads as the stock GUI font. A host font would be
    //! better and there is no way to ask for one.
    HFONT editorFont() { return NULL; }

    //! There is no layout editing mode in a VST window; it is always live.
    bool editorLayoutEditMode() { return false; }

    //! What the open editor is currently showing, or NULL when it is closed.
    //! The settings it draws should track the plug-in's sliders, and this is the
    //! only way to see from outside that idle() is keeping them in step - which
    //! paraeq_vst_verify checks, because the alternative is finding out in a
    //! host that a curve stopped following its own automation.
    const paraeq::Params * shownParams() const;

private:
    //! Copies the plug-in's sixteen sliders into m_synced.
    void snapshotControls();

    //! True when the plug-in's sliders have moved away from m_synced, i.e. by
    //! some route other than this editor - automation, a preset load, a generic
    //! UI. Compared as slider floats rather than as Params, so the comparison is
    //! exact and a round trip through the mapping cannot register as a change.
    bool controlsMovedElsewhere() const;

    ParaEQ & m_plugin;

    //! Created by open() and destroyed by close(), because a host opens and
    //! closes the window repeatedly over one plug-in's life and the window is
    //! the only thing that is per-opening. The settings are not: they live in
    //! the plug-in, which is where a VST's state belongs.
    paraeq_editor::Editor * m_editor;

    //! Storage for getRect(). The host reads through the pointer and does not
    //! free it, so it has to outlive the call and belong to us.
    ERect m_rect;

public:
    //! One float per parameter. Sixteen is a literal because this header cannot
    //! see kNumParameters - ParaEQ.h includes this one, not the other way about
    //! - so ParaEQEditor.cpp static_asserts the two against each other where
    //! both are in scope, and this typedef is what lets it.
    typedef float synced_type[16];

private:
    //! The sliders as both sides last agreed on them. Written when the editor
    //! pushes a change out and when idle() pulls one in, so a value that has
    //! just made the round trip does not read as a fresh change and bounce back.
    synced_type m_synced;

    double m_syncedRate;
};

#endif
