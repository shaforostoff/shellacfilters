/*
 *	ParaEQControls.h
 *	ParaEQ
 *
 *	The sixteen controls, and which field of paraeq::Params each one is.
 *
 *	Split out of ParaEQ.h so that two things can share it: the wrapper, which
 *	reads the controls to design the filters, and the editor in ParaEQView.mm,
 *	which reads the same controls to draw the curve. The editor holds an opaque
 *	AudioUnit rather than a ParaEQ *, so it cannot reach the wrapper's own
 *	accessor - and a second copy of "which control is which field" is a second
 *	place for the curve on screen to disagree with what the audio is doing.
 *
 *	Header-only and free of the AU SDK on purpose: this is what lets an
 *	Objective-C++ file include it beside AppKit without pulling AUBase in
 *	behind it.
 */

#ifndef __ParaEQControls_h__
#define __ParaEQControls_h__

#include "paraeq_core.h"

enum {
	kParam_A =0,	//HP Freq
	kParam_B =1,	//HP Slope
	kParam_C =2,	//Low Gain
	kParam_D =3,	//Low Freq
	kParam_E =4,	//Low Bell
	kParam_F =5,	//LoMid Gain
	kParam_G =6,	//LoMid Freq
	kParam_H =7,	//LoMid Q
	kParam_I =8,	//HiMid Gain
	kParam_J =9,	//HiMid Freq
	kParam_K =10,	//HiMid Q
	kParam_L =11,	//High Gain
	kParam_M =12,	//High Freq
	kParam_N =13,	//High Bell
	kParam_O =14,	//Output
	kParam_P =15,	//Bypass
	kNumberOfParameters=16
};

//The high-pass slope is the one control whose number means nothing on its own.
enum { kNumberOfSlopes = 3 };

//! The one place that says which control is which field of paraeq::Params.
//! ParaEQParamsFromControls() is its inverse; the two are read together, and a
//! control added to one without the other shows up at once as a default that
//! does not survive a round trip.
inline void ParaEQControlsFromParams(const paraeq::Params & p, float * out)
{
	out[kParam_A] = p.hpFrequency;
	out[kParam_B] = (float)p.hpSlope;
	out[kParam_C] = p.lfGain;
	out[kParam_D] = p.lfFrequency;
	out[kParam_E] = p.lfBell ? 1.0f : 0.0f;
	out[kParam_F] = p.lmfGain;
	out[kParam_G] = p.lmfFrequency;
	out[kParam_H] = p.lmfQ;
	out[kParam_I] = p.hmfGain;
	out[kParam_J] = p.hmfFrequency;
	out[kParam_K] = p.hmfQ;
	out[kParam_L] = p.hfGain;
	out[kParam_M] = p.hfFrequency;
	out[kParam_N] = p.hfBell ? 1.0f : 0.0f;
	out[kParam_O] = p.outputGain;
	out[kParam_P] = p.bypass ? 1.0f : 0.0f;
}

//! The inverse. Nothing is scaled on the way through: every control is
//! published in the core's own units, so this gathers sixteen values and hands
//! them to sanitize(), which is the core's own guard against a caller that
//! sends something outside the range it was given.
inline paraeq::Params ParaEQParamsFromControls(const float * in)
{
	paraeq::Params p = paraeq::Params::defaults();
	p.hpFrequency  = in[kParam_A];
	p.hpSlope      = (int)(in[kParam_B] + 0.5f);
	p.lfGain       = in[kParam_C];
	p.lfFrequency  = in[kParam_D];
	p.lfBell       = in[kParam_E] > 0.5f;
	p.lmfGain      = in[kParam_F];
	p.lmfFrequency = in[kParam_G];
	p.lmfQ         = in[kParam_H];
	p.hmfGain      = in[kParam_I];
	p.hmfFrequency = in[kParam_J];
	p.hmfQ         = in[kParam_K];
	p.hfGain       = in[kParam_L];
	p.hfFrequency  = in[kParam_M];
	p.hfBell       = in[kParam_N] > 0.5f;
	p.outputGain   = in[kParam_O];
	p.bypass       = in[kParam_P] > 0.5f;
	p.sanitize();
	return p;
}

#endif
