/* foo_dsp_paraeq - resource identifiers
 *
 *  Sparse next to the other components' because the editor is owner-drawn: the
 *  dialog template carries a placeholder to size the editor against and the
 *  three buttons, and everything else is painted. See paraeq_editor.h for why.
 */

#ifndef FOO_DSP_PARAEQ_RESOURCE_H
#define FOO_DSP_PARAEQ_RESOURCE_H

#ifndef IDC_STATIC
#define IDC_STATIC                  (-1)
#endif

#define IDD_PARAEQ                  401

//! The editor child window is created over this control's rectangle and the
//! placeholder is then hidden. It is a real control rather than a hard-coded
//! rectangle so the dialog editor and the layout stay in one place.
#define IDC_EDITOR_PLACEHOLDER      4001

//! The editor itself, once created. Named rather than anonymous so the smoke
//! test can find it and type at it - which, the editor being owner-drawn, is
//! the only handle an outside process has on it.
#define IDC_EDITOR                  4002

#define IDC_HINT                    4020
#define IDC_DEFAULTS                4030

#endif // FOO_DSP_PARAEQ_RESOURCE_H
