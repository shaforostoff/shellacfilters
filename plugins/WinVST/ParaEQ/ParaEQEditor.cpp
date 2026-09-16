/* ========================================
 *  ParaEQ - ParaEQEditor.cpp
 *  MIT license, as the rest of the tree
 * ======================================== */

#ifndef __ParaEQ_H
#include "ParaEQ.h"
#endif

/*  The window's size, in pixels, and the only number in this file that is a
 *  taste rather than a consequence. For comparison the foobar2000 modal dialog
 *  gives the same editor about 486 x 384, and the element in a layout gets
 *  whatever it is dragged to - the editor drops the readout strip and then the
 *  footer as the room runs out, so there is no size at which it breaks, only
 *  sizes at which it shows less. This is a little wider than the dialog because
 *  a plug-in window has no other reason to be narrow, and the extra width all
 *  goes to the curve.
 *
 *  Fixed, because VST2 window resizing needs audioMasterSizeWindow and a host
 *  that honours it, and the ones that do not leave the plug-in drawing outside
 *  a window the host still thinks is the old size.
 */
static const int kEditorWidth  = 640;
static const int kEditorHeight = 400;

//! The child window's control ID. Nothing looks it up - the editor is found
//! through m_editor, not through GetDlgItem - so any value does; it is passed
//! because create() asks for one.
static const UINT kEditorChildId = 1;

/*  ParaEQEditor.h cannot include ParaEQ.h - the plug-in holds the editor by
 *  value, so the dependency only runs one way - which means m_synced is sized
 *  by a literal rather than by kNumParameters. This is the literal being held
 *  to account, here where both are visible. */
static_assert(sizeof(ParaEQEditor::synced_type) / sizeof(float) == kNumParameters,
              "m_synced in ParaEQEditor.h is no longer one float per parameter");

ParaEQEditor::ParaEQEditor(ParaEQ & plugin)
    : m_plugin(plugin)
    , m_editor(NULL)
    , m_syncedRate(0.0)
{
    memset(&m_rect, 0, sizeof m_rect);
    for (int i = 0; i < kNumParameters; ++i) m_synced[i] = 0.0f;
}

ParaEQEditor::~ParaEQEditor()
{
    //A host is supposed to send effEditClose before the plug-in is destroyed
    //and most do. This is for the ones that do not: the window is parented to
    //something the host is about to destroy, and leaving it to be orphaned is
    //how a plug-in scan ends up with a stray window on screen.
    delete m_editor;
    m_editor = NULL;
}

// ---------------------------------------------------------------------------
// AEffEditor
// ---------------------------------------------------------------------------

bool ParaEQEditor::getRect(ERect ** rect)
{
    //Answered whether or not the window is open: a host asks before it creates
    //the parent, to know how big to make it.
    m_rect.top    = 0;
    m_rect.left   = 0;
    m_rect.bottom = (VstInt16)kEditorHeight;
    m_rect.right  = (VstInt16)kEditorWidth;

    if (rect) *rect = &m_rect;
    return true;
}

bool ParaEQEditor::open(void * ptr)
{
    if (m_editor != NULL) return true;    //already up; a host may ask twice

    HWND parent = (HWND)ptr;
    if (parent == NULL || !IsWindow(parent)) return false;

    if (!paraeq_editor::registerClass()) return false;

    RECT rc;
    rc.left   = 0;
    rc.top    = 0;
    rc.right  = kEditorWidth;
    rc.bottom = kEditorHeight;

    /*  Nothing below is allowed to throw out of this function: the caller is
     *  the host, reached through a C function pointer, and an exception
     *  crossing that boundary is undefined rather than merely rude. The editor
     *  allocates a handful of std::vectors as it lays itself out, so bad_alloc
     *  is the realistic one.
     */
    try {
        m_editor = new paraeq_editor::Editor(*this);
        if (m_editor->create(parent, rc, kEditorChildId,
                             m_plugin.paramsFromControls()) == NULL) {
            delete m_editor;
            m_editor = NULL;
            return false;
        }
    } catch (...) {
        delete m_editor;
        m_editor = NULL;
        return false;
    }

    snapshotControls();
    m_syncedRate = (double)m_plugin.getSampleRate();
    m_editor->setSampleRate(m_syncedRate);

    AEffEditor::open(ptr);      //records the parent, so isOpen() agrees
    return true;
}

void ParaEQEditor::close()
{
    //The Editor destructor destroys the window if it still exists, which is the
    //contract its header states. The settings are not lost with it: they live in
    //the plug-in's sixteen parameters, which is where a VST's state belongs and
    //why reopening is a fresh window over unchanged settings.
    delete m_editor;
    m_editor = NULL;
    AEffEditor::close();
}

bool ParaEQEditor::isOpen()
{
    return m_editor != NULL;
}

/*  Called by the host while the window is up, and this is the only place a
 *  change made anywhere other than in this editor can get into it. VST2 has no
 *  "a parameter moved" callback in the plug-in's direction - setParameter() is
 *  how the host tells the plug-in, and nothing tells the editor - so the editor
 *  has to look. Which means a host that never sends effEditIdle shows an
 *  editor that does not follow automation; there is no second mechanism in the
 *  ABI to fall back on, and inventing one out of a timer would fight the
 *  editor's own.
 *
 *  Cheap enough to run at whatever rate a host likes: sixteen float
 *  comparisons, and everything after them is skipped in the case that is almost
 *  always the case.
 */
void ParaEQEditor::idle()
{
    if (m_editor == NULL) return;

    const double rate = (double)m_plugin.getSampleRate();
    if (rate != m_syncedRate) {
        m_syncedRate = rate;
        //Only the top of the plot can tell the difference, but that is where
        //the hiss shelf lives.
        m_editor->setSampleRate(rate);
    }

    if (!controlsMovedElsewhere()) return;

    snapshotControls();
    //setParams() ignores this while the user is dragging, which is what stops a
    //push of our own coming back round from yanking the handle out from under
    //the pointer.
    m_editor->setParams(m_plugin.paramsFromControls());
}

// ---------------------------------------------------------------------------
// paraeq_editor::Host
// ---------------------------------------------------------------------------

void ParaEQEditor::editorParamsChanged(const paraeq::Params & p)
{
    float want[kNumParameters];
    ParaEQ::controlsFromParams(p, want);

    for (int i = 0; i < kNumParameters; ++i) {
        if (want[i] == m_plugin.getParameter(i)) continue;

        //setParameterAutomated() rather than setParameter(): a host that is
        //recording automation has to be told that the user moved something, and
        //audioMasterAutomate is the only thing in VST2 that says so. Sending it
        //only for the sliders that actually moved is what keeps a drag on one
        //handle out of the other fifteen automation lanes.
        m_plugin.setParameterAutomated(i, want[i]);
    }

    //Agreed, in both directions: idle() must not read this back as a change
    //somebody else made and push it into the editor again.
    snapshotControls();
}

// ---------------------------------------------------------------------------

const paraeq::Params * ParaEQEditor::shownParams() const
{
    return m_editor ? &m_editor->params() : NULL;
}

// ---------------------------------------------------------------------------

void ParaEQEditor::snapshotControls()
{
    for (int i = 0; i < kNumParameters; ++i) m_synced[i] = m_plugin.getParameter(i);
}

bool ParaEQEditor::controlsMovedElsewhere() const
{
    for (int i = 0; i < kNumParameters; ++i) {
        if (m_plugin.getParameter(i) != m_synced[i]) return true;
    }
    return false;
}
