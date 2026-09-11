/* ========================================
 *  foo_dsp_paraeq - precompiled header
 * ======================================== */

#ifndef FOO_DSP_PARAEQ_STDAFX_H
#define FOO_DSP_PARAEQ_STDAFX_H

// The SDK has to come first: pfc-lite.h insists on pulling WinSock2.h in ahead
// of windows.h, and including windows.h before it produces a wall of
// redefinition errors. WIN32_LEAN_AND_MEAN stays undefined for the same kind
// of reason - pfc/timers.h wants timeGetTime out of mmsystem.h.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <SDK/foobar2000.h>

#ifdef _WIN32
#include <commctrl.h>
#include <windowsx.h>
#endif

#endif // FOO_DSP_PARAEQ_STDAFX_H
