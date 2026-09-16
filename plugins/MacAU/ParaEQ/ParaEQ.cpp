/*
*	File:		ParaEQ.cpp
*	
*	Version:	1.0
* 
*	Created:	8/20/26
*	
*	Copyright:  Copyright © 2026 ShellacFilters, MIT license as the rest of the tree
* 
*	Disclaimer:	IMPORTANT:  This Apple software is supplied to you by Apple Computer, Inc. ("Apple") in 
*				consideration of your agreement to the following terms, and your use, installation, modification 
*				or redistribution of this Apple software constitutes acceptance of these terms.  If you do 
*				not agree with these terms, please do not use, install, modify or redistribute this Apple 
*				software.
*
*				In consideration of your agreement to abide by the following terms, and subject to these terms, 
*				Apple grants you a personal, non-exclusive license, under Apple's copyrights in this 
*				original Apple software (the "Apple Software"), to use, reproduce, modify and redistribute the 
*				Apple Software, with or without modifications, in source and/or binary forms; provided that if you 
*				redistribute the Apple Software in its entirety and without modifications, you must retain this 
*				notice and the following text and disclaimers in all such redistributions of the Apple Software. 
*				Neither the name, trademarks, service marks or logos of Apple Computer, Inc. may be used to 
*				endorse or promote products derived from the Apple Software without specific prior written 
*				permission from Apple.  Except as expressly stated in this notice, no other rights or 
*				licenses, express or implied, are granted by Apple herein, including but not limited to any 
*				patent rights that may be infringed by your derivative works or by other works in which the 
*				Apple Software may be incorporated.
*
*				The Apple Software is provided by Apple on an "AS IS" basis.  APPLE MAKES NO WARRANTIES, EXPRESS OR 
*				IMPLIED, INCLUDING WITHOUT LIMITATION THE IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY 
*				AND FITNESS FOR A PARTICULAR PURPOSE, REGARDING THE APPLE SOFTWARE OR ITS USE AND OPERATION ALONE 
*				OR IN COMBINATION WITH YOUR PRODUCTS.
*
*				IN NO EVENT SHALL APPLE BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL OR CONSEQUENTIAL 
*				DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS 
*				OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) ARISING IN ANY WAY OUT OF THE USE, 
*				REPRODUCTION, MODIFICATION AND/OR DISTRIBUTION OF THE APPLE SOFTWARE, HOWEVER CAUSED AND WHETHER 
*				UNDER THEORY OF CONTRACT, TORT (INCLUDING NEGLIGENCE), STRICT LIABILITY OR OTHERWISE, EVEN 
*				IF APPLE HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/
/*=============================================================================
	ParaEQ.cpp
	
=============================================================================*/
#include "ParaEQ.h"

#include <string.h>

/*  There is no per-sample DSP written out here, and that is deliberate: the
 *  cascade is six cookbook biquads with a per-sample coefficient glide, and it
 *  lives in paraeq_core.cpp so that the foobar2000 DSP and this Audio Unit are
 *  running the identical code. What belongs in this file is only what an AU has
 *  to do that the foobar2000 component does not:
 *
 *    - notice control changes, since an AU is only told about them by being
 *      asked for the value (updateConfig(), below),
 *    - publish sixteen controls with their units, ranges, groupings and the
 *      one set of value strings, which is the bulk of the file,
 *    - copy in to out when a host declines to render in place.
 *
 *  What it does NOT do, and both absences are the point:
 *
 *    - no latency or tail arithmetic. The core is zero latency - what goes in
 *      comes out, same count, same alignment - so there is nothing to declare
 *      and nothing to pre-roll. Its two siblings in this folder are most of
 *      their own wrappers for exactly the machinery this one does not need.
 *    - no dither. Declick and Dehum compute in double and dither on the way
 *      down to the host's 32 bit float, which is the Airwindows house pattern.
 *      Here the core is asked for floats directly - process() is instantiated
 *      for float and computes each stage in double regardless - so the AU
 *      writes the same float foo_dsp_paraeq writes, bit for bit. Adding a
 *      dither would be adding a difference between the two ports for the sake
 *      of the house pattern, and the core's own contract is the stronger
 *      claim.
 *
 *  That float path is also what keeps this an in-place effect: the samples are
 *  never copied out into a double buffer and back, so a host that hands the
 *  same buffer in and out has the whole cascade run over it where it lies.
 */


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

COMPONENT_ENTRY(ParaEQ)


#pragma mark ____Control table

/*  One table rather than sixteen near-identical switch arms. Every range in it
 *  is a paraeq_core constant rather than a number typed again: a control whose
 *  travel disagreed with the core's would be asking for filters the core
 *  silently clamps, and the host's knob would stop meaning what it reads.
 *
 *  `logarithmic` is kAudioUnitParameterFlag_DisplayLogarithmic, which decides
 *  how a host spaces a knob's travel and nothing else. Every frequency gets it
 *  - an octave is an octave wherever it sits, and linear travel would spend
 *  half a high-pass knob between 180 and 350 Hz - and so do the two Q controls,
 *  which are a ratio in the same way. Gains do not: dB is already logarithmic,
 *  and the useful thing about a gain control is that 0 sits in the middle.
 */

namespace {

struct ControlSpec {
	CFStringRef            name;
	AudioUnitParameterUnit unit;
	float                  minValue;
	float                  maxValue;
	UInt32                 clump;
	bool                   logarithmic;
};

const ControlSpec kControls[kNumberOfParameters] = {
	//                                                   min                max             clump             log
	{ kParameterAName, kAudioUnitParameterUnit_Hertz,    paraeq::kHpFreqMin,  paraeq::kHpFreqMax,  kClump_HighPass, true  },
	{ kParameterBName, kAudioUnitParameterUnit_Indexed,  0.0f,                (float)(kNumberOfSlopes - 1), kClump_HighPass, false },
	{ kParameterCName, kAudioUnitParameterUnit_Decibels, -paraeq::kGainMaxDb, paraeq::kGainMaxDb,  kClump_Low,      false },
	{ kParameterDName, kAudioUnitParameterUnit_Hertz,    paraeq::kLfFreqMin,  paraeq::kLfFreqMax,  kClump_Low,      true  },
	{ kParameterEName, kAudioUnitParameterUnit_Boolean,  0.0f,                1.0f,                kClump_Low,      false },
	{ kParameterFName, kAudioUnitParameterUnit_Decibels, -paraeq::kGainMaxDb, paraeq::kGainMaxDb,  kClump_LowMid,   false },
	{ kParameterGName, kAudioUnitParameterUnit_Hertz,    paraeq::kLmfFreqMin, paraeq::kLmfFreqMax, kClump_LowMid,   true  },
	{ kParameterHName, kAudioUnitParameterUnit_Generic,  paraeq::kQMin,       paraeq::kQMax,       kClump_LowMid,   true  },
	{ kParameterIName, kAudioUnitParameterUnit_Decibels, -paraeq::kGainMaxDb, paraeq::kGainMaxDb,  kClump_HighMid,  false },
	{ kParameterJName, kAudioUnitParameterUnit_Hertz,    paraeq::kHmfFreqMin, paraeq::kHmfFreqMax, kClump_HighMid,  true  },
	{ kParameterKName, kAudioUnitParameterUnit_Generic,  paraeq::kQMin,       paraeq::kQMax,       kClump_HighMid,  true  },
	{ kParameterLName, kAudioUnitParameterUnit_Decibels, -paraeq::kGainMaxDb, paraeq::kGainMaxDb,  kClump_High,     false },
	{ kParameterMName, kAudioUnitParameterUnit_Hertz,    paraeq::kHfFreqMin,  paraeq::kHfFreqMax,  kClump_High,     true  },
	{ kParameterNName, kAudioUnitParameterUnit_Boolean,  0.0f,                1.0f,                kClump_High,     false },
	{ kParameterOName, kAudioUnitParameterUnit_Decibels, -paraeq::kOutputMaxDb, paraeq::kOutputMaxDb, kClump_Output, false },
	{ kParameterPName, kAudioUnitParameterUnit_Boolean,  0.0f,                1.0f,                kClump_Output,   false },
};

const CFStringRef kClumpNames[kNumberOfClumps] = {
	CFSTR("High-pass"),
	CFSTR("Low"),
	CFSTR("Low mid"),
	CFSTR("High mid"),
	CFSTR("High"),
	CFSTR("Output")
};

static CFStringRef kSlopeItem_Off  = CFSTR("Off");
static CFStringRef kSlopeItem_12dB = CFSTR("12 dB/oct");
static CFStringRef kSlopeItem_24dB = CFSTR("24 dB/oct");

//! The one place that says which control is which field of paraeq::Params.
//! ParaEQ::paramsFromControls() is its inverse; the two are read together, and
//! a control added to one without the other shows up at once as a default that
//! does not survive a round trip.
void controlsFromParams(const paraeq::Params & p, float * out)
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

} // anonymous namespace


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::ParaEQ
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ParaEQ::ParaEQ(AudioUnit component)
	: AUEffectBase(component)
{
	CreateElements();
	Globals()->UseIndexedParameters(kNumberOfParameters);

	//Defaults are the core's, read out of Params::defaults() rather than
	//written down again here. Flat, with every band parked on the target its
	//knob is for - see the note on defaults() in paraeq_core.h.
	active = paraeq::Params::defaults();
	float initial[kNumberOfParameters];
	controlsFromParams(active, initial);
	for (AudioUnitParameterID i = 0; i < kNumberOfParameters; ++i) {
		SetParameter(i, initial[i]);
	}

	//Nothing in this core is sized by the parameters or by the sample rate -
	//heapBytes() is 0 - so this is the whole of the setup and Initialize() only
	//has to redo the arithmetic at the rate the host turns out to have.
	cfg.compute(active, 44100.0);
	chan[0].configure(cfg);
	chan[1].configure(cfg);
	activeRate = 0.0;             //forces updateConfig() to run once for real
	needsReset = false;

#if AU_DEBUG_DISPATCHER
	mDebugDispatcher = new AUDebugDispatcher (this);
#endif
	
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::GetParameterValueStrings
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			ParaEQ::GetParameterValueStrings(AudioUnitScope		inScope,
                                                                AudioUnitParameterID	inParameterID,
                                                                CFArrayRef *		outStrings)
{
	//The high-pass slope is the only control whose number means nothing on its
	//own. The two Bell switches and Bypass are kAudioUnitParameterUnit_Boolean,
	//which a host draws as a checkbox and does not ask for strings for.
    if ((inScope == kAudioUnitScope_Global) && (inParameterID == kParam_B))
	{
		if (outStrings == NULL) return noErr;
		CFStringRef strings [] =
		{
			kSlopeItem_Off,
			kSlopeItem_12dB,
			kSlopeItem_24dB,
		};
		*outStrings = CFArrayCreate (
									 NULL,
									 (const void **) strings,
									 (sizeof (strings) / sizeof (strings [0])),
									 NULL
									 );
		return noErr;
	}
    return kAudioUnitErr_InvalidProperty;
}



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::GetParameterInfo
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			ParaEQ::GetParameterInfo(AudioUnitScope		inScope,
                                                        AudioUnitParameterID	inParameterID,
                                                        AudioUnitParameterInfo	&outParameterInfo )
{
	if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidParameter;
	if (inParameterID >= (AudioUnitParameterID)kNumberOfParameters) {
		return kAudioUnitErr_InvalidParameter;
	}

	const ControlSpec & spec = kControls[inParameterID];

	outParameterInfo.flags = 	kAudioUnitParameterFlag_IsWritable
						|		kAudioUnitParameterFlag_IsReadable
						|		kAudioUnitParameterFlag_HasClump;
	if (spec.logarithmic) outParameterInfo.flags |= kAudioUnitParameterFlag_DisplayLogarithmic;

	//FillInParameterName sets clumpID itself - to none - so the clump goes on
	//after it rather than before.
	AUBase::FillInParameterName (outParameterInfo, spec.name, false);
	outParameterInfo.clumpID = spec.clump;
	outParameterInfo.unit = spec.unit;
	outParameterInfo.minValue = spec.minValue;
	outParameterInfo.maxValue = spec.maxValue;

	float defaults[kNumberOfParameters];
	controlsFromParams(paraeq::Params::defaults(), defaults);
	outParameterInfo.defaultValue = defaults[inParameterID];

	return noErr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::GetPropertyInfo
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			ParaEQ::GetPropertyInfo (AudioUnitPropertyID	inID,
                                                        AudioUnitScope		inScope,
                                                        AudioUnitElement	inElement,
                                                        UInt32 &		outDataSize,
                                                        bool &			outWritable)
{
	return AUEffectBase::GetPropertyInfo (inID, inScope, inElement, outDataSize, outWritable);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// The equaliser is per-channel and holds nothing that couples the two, so mono
// is not a special case of stereo here - it is the same thing with one channel.
// Both are offered; nothing wider is, because chan[] is two.
UInt32 ParaEQ::SupportedNumChannels(const AUChannelInfo ** outInfo)
{
	static const AUChannelInfo info[] = { { 1, 1 }, { 2, 2 } };
	if (outInfo != NULL) *outInfo = info;
	return sizeof(info) / sizeof(info[0]);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::GetProperty
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			ParaEQ::GetProperty(	AudioUnitPropertyID inID,
                                                        AudioUnitScope 		inScope,
                                                        AudioUnitElement 	inElement,
                                                        void *			outData )
{
	return AUEffectBase::GetProperty (inID, inScope, inElement, outData);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::CopyClumpName
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//What turns the clumpID on each control into a heading a host can draw. Without
//it the sixteen controls are one undifferentiated list, which is the thing the
//clumps are there to avoid.
//
//Copy, not Get: the caller owns what comes back and releases it, so the
//constant is retained on the way out. inDesiredLength is a hint about how much
//room the host has; these are one or two words already, so there is no shorter
//form to offer and it is ignored.
ComponentResult		ParaEQ::CopyClumpName(AudioUnitScope		inScope,
											  UInt32				inClumpID,
											  UInt32				/*inDesiredNameLength*/,
											  CFStringRef *			outClumpName)
{
	if (inScope != kAudioUnitScope_Global || outClumpName == NULL) {
		return kAudioUnitErr_InvalidProperty;
	}
	//Clump 0 is the API's "no clump" and is rejected before this is called.
	if (inClumpID == 0 || inClumpID > (UInt32)kNumberOfClumps) {
		return kAudioUnitErr_InvalidPropertyValue;
	}
	*outClumpName = (CFStringRef)CFRetain(kClumpNames[inClumpID - 1]);
	return noErr;
}

//	ParaEQ::Initialize
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult ParaEQ::Initialize()
{
    ComponentResult result = AUEffectBase::Initialize();
    if (result == noErr) {
        //An AU is told its sample rate before it renders, so the coefficients
        //can be designed for the real rate here rather than on the first render
        //call. needsReset makes updateConfig() snap to the new curve instead of
        //gliding to it: nothing has been heard yet, so there is nothing for a
        //glide to be smooth across.
        needsReset = true;
        updateConfig();
    }
    return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::paramsFromControls
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//The inverse of controlsFromParams(). Nothing is scaled on the way through:
//every control is published in the core's own units, so this gathers sixteen
//values and hands them to sanitize(), which is the core's own guard against a
//host that sends something outside the range it was given.
paraeq::Params ParaEQ::paramsFromControls()
{
	paraeq::Params p = paraeq::Params::defaults();
	p.hpFrequency  = GetParameter( kParam_A );
	p.hpSlope      = (int)(GetParameter( kParam_B ) + 0.5f);
	p.lfGain       = GetParameter( kParam_C );
	p.lfFrequency  = GetParameter( kParam_D );
	p.lfBell       = GetParameter( kParam_E ) > 0.5f;
	p.lmfGain      = GetParameter( kParam_F );
	p.lmfFrequency = GetParameter( kParam_G );
	p.lmfQ         = GetParameter( kParam_H );
	p.hmfGain      = GetParameter( kParam_I );
	p.hmfFrequency = GetParameter( kParam_J );
	p.hmfQ         = GetParameter( kParam_K );
	p.hfGain       = GetParameter( kParam_L );
	p.hfFrequency  = GetParameter( kParam_M );
	p.hfBell       = GetParameter( kParam_N ) > 0.5f;
	p.outputGain   = GetParameter( kParam_O );
	p.bypass       = GetParameter( kParam_P ) > 0.5f;
	p.sanitize();
	return p;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::updateConfig
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//Called at the top of every render call, because that is how an AU finds out
//that a control moved: by reading it and noticing. Cheap when nothing did - a
//Params compare - and when something did, the work is designing six biquads,
//which is the same arithmetic the foobar2000 component does on the same event.
void ParaEQ::updateConfig()
{
	const double rate = GetSampleRate();
	const paraeq::Params p = paramsFromControls();
	if (!needsReset && rate == activeRate && p == active) return;

	paraeq::Config next;
	next.compute(p, rate);

	if (needsReset) {
		//Starting, not adjusting: snap to the curve and start from silence.
		chan[0].configure(next);
		chan[1].configure(next);
		needsReset = false;
	} else {
		//Every control move retunes in place. retune() never fails and never
		//allocates - see Config::structurallyEquals - so unlike the two
		//siblings there is no rebuild path here and no rate change that forces
		//one. The curve glides to the new one over kGlideSec rather than
		//switching to it, which is what keeps a knob drag from stepping the
		//output.
		chan[0].retune(next);
		chan[1].retune(next);
	}

	cfg = next;
	active = p;
	activeRate = rate;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::Reset()
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult		ParaEQ::Reset(AudioUnitScope inScope, AudioUnitElement inElement)
{
	//The biquad states describe samples that are not adjacent to whatever comes
	//next, so they go. The coefficients and where a glide had got to are the
	//user's settings and stay - which is why this is the core's reset() rather
	//than a reconfigure: an equaliser that came back flat and glided up to its
	//own settings after every transport stop would be putting a swell into the
	//first 60 ms of every take.
	chan[0].reset();
	chan[1].reset();
	return noErr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	ParaEQ::ProcessBufferLists
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
OSStatus		ParaEQ::ProcessBufferLists(AudioUnitRenderActionFlags & ioActionFlags,
													const AudioBufferList & inBuffer,
                                                    AudioBufferList & outBuffer,
                                                    UInt32 			inFramesToProcess)
{
	//FTZ for the duration of the buffer. Six biquads per channel are six
	//recursions towards zero, so a fade-out reaches denormal range and pays for
	//it on hardware that traps. It also changes results in the last bits, which
	//is why it is the core's own guard rather than a build flag: every port
	//holds it across its processing loop and so they all round alike.
	paraeq::scoped_flush_denormals ftz;
	updateConfig();

	//A biquad rings rather than stops, so silence going in does not mean
	//silence coming out until the states have decayed - which at a high Q and a
	//low corner is most of a second. Say so, or a host that trusts the flag
	//truncates the decay of every fade.
	ioActionFlags &= ~kAudioUnitRenderAction_OutputIsSilence;

	UInt32 channels = inBuffer.mNumberBuffers;
	if (outBuffer.mNumberBuffers < channels) channels = outBuffer.mNumberBuffers;
	if (channels > 2) channels = 2;

	for (UInt32 c = 0; c < channels; ++c) {
		Float32 * input = (Float32*)(inBuffer.mBuffers[c].mData);
		Float32 * output = (Float32*)(outBuffer.mBuffers[c].mData);
		if (input == NULL || output == NULL) continue;

		//The core works in place. In place is also how a host normally renders
		//an effect, so the copy is the exception rather than the path.
		if (output != input) {
			memcpy(output, input, sizeof(Float32) * (size_t)inFramesToProcess);
		}
		chan[c].process(output, (size_t)inFramesToProcess, 1);
	}
	return noErr;
}
