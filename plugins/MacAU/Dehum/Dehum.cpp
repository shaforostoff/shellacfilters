/*
*	File:		Dehum.cpp
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
	Dehum.cpp
	
=============================================================================*/
#include "Dehum.h"

/*  There is no per-sample DSP written out here, and that is deliberate: the
 *  algorithm is an FFT-based line detector feeding a bank of heterodyne
 *  notches, and it lives in dehum_core.cpp so that the foobar2000 DSP, the
 *  offline CLI, the VST and this Audio Unit are all running the identical
 *  code. What belongs in this file is only what an AU has to do that the
 *  others do not:
 *
 *    - notice slider changes, since like a VST an AU is only told about them
 *      by being asked for the value (updateConfig(), below). The sample rate
 *      it does get told about, at Initialize(), which is where the one
 *      allocation a rate change forces is taken off the render thread,
 *    - dither on the way down to 32 bit float.
 *
 *  Handing the host n samples out for every n in - the third item in the
 *  equivalent Declick file - is not a problem here, because this core has no
 *  latency and works in place. The detector reads the signal but does not sit
 *  in the path, so there is no lookahead to pre-roll, no FIFO, nothing to
 *  declare and nothing to flush at the end of a bounce.
 *
 *  The chunking below exists only because the DSP is done in double while the
 *  AU's buffers are float and have to be dithered on the way out: the scratch
 *  buffers are where the doubles live in between. Splitting a long buffer
 *  across several calls is something the core cannot detect - process() is a
 *  plain per-sample loop with its own hop counter - which is what dehum_verify's
 *  block size invariance check establishes.
 */


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

COMPONENT_ENTRY(Dehum)


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::Dehum
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
Dehum::Dehum(AudioUnit component)
	: AUEffectBase(component)
{
	CreateElements();
	Globals()->UseIndexedParameters(kNumberOfParameters);
	SetParameter(kParam_A, kDefaultValue_ParamA );
	SetParameter(kParam_B, kDefaultValue_ParamB );
	SetParameter(kParam_C, kDefaultValue_ParamC );
	SetParameter(kParam_D, kDefaultValue_ParamD );
	SetParameter(kParam_E, kDefaultValue_ParamE );
	SetParameter(kParam_F, kDefaultValue_ParamF );
	SetParameter(kParam_G, kDefaultValue_ParamG );

	//the channels allocate on configure(), so get that out of the render thread
	//by doing it here at a nominal sample rate. Initialize() redoes it at the
	//rate the host actually has, before anything renders.
	active = paramsFromControls();
	cfg.compute(active, 44100.0);
	chanL.configure(cfg);
	chanR.configure(cfg);
	activeRate = 0.0;             //forces updateConfig() to run once for real
	needsConfigure = true;

	fpdL = 1.0; while (fpdL < 16386) fpdL = rand()*UINT32_MAX;
	fpdR = 1.0; while (fpdR < 16386) fpdR = rand()*UINT32_MAX;
         
#if AU_DEBUG_DISPATCHER
	mDebugDispatcher = new AUDebugDispatcher (this);
#endif
	
}


//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::GetParameterValueStrings
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			Dehum::GetParameterValueStrings(AudioUnitScope		inScope,
                                                                AudioUnitParameterID	inParameterID,
                                                                CFArrayRef *		outStrings)
{
        
    return kAudioUnitErr_InvalidProperty;
}



//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::GetParameterInfo
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			Dehum::GetParameterInfo(AudioUnitScope		inScope,
                                                        AudioUnitParameterID	inParameterID,
                                                        AudioUnitParameterInfo	&outParameterInfo )
{
	ComponentResult result = noErr;

	outParameterInfo.flags = 	kAudioUnitParameterFlag_IsWritable
						|		kAudioUnitParameterFlag_IsReadable;
    
    //Every slider is 0..1 and generic, which is the airwindows house
    //arrangement and also what keeps this format and the VST interchangeable:
    //paramsFromControls() below is the VST's, unchanged, so a setting means the
    //same thing in both. What it costs is the VST's getParameterDisplay(), so
    //Freq reads 0.0 here rather than "auto" - see the README table for what
    //each position maps to.
    if (inScope == kAudioUnitScope_Global) {
        switch(inParameterID)
        {
            case kParam_A:
                AUBase::FillInParameterName (outParameterInfo, kParameterAName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamA;
                break;
            case kParam_B:
                AUBase::FillInParameterName (outParameterInfo, kParameterBName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamB;
                break;
            case kParam_C:
                AUBase::FillInParameterName (outParameterInfo, kParameterCName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamC;
                break;
            case kParam_D:
                AUBase::FillInParameterName (outParameterInfo, kParameterDName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamD;
                break;
            case kParam_E:
                AUBase::FillInParameterName (outParameterInfo, kParameterEName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamE;
                break;
            case kParam_F:
                AUBase::FillInParameterName (outParameterInfo, kParameterFName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamF;
                break;
            case kParam_G:
                AUBase::FillInParameterName (outParameterInfo, kParameterGName, false);
                outParameterInfo.unit = kAudioUnitParameterUnit_Generic;
                outParameterInfo.minValue = 0.0;
                outParameterInfo.maxValue = 1.0;
                outParameterInfo.defaultValue = kDefaultValue_ParamG;
                break;
			default:
                result = kAudioUnitErr_InvalidParameter;
                break;
            }
	} else {
        result = kAudioUnitErr_InvalidParameter;
    }
    


	return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::GetPropertyInfo
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			Dehum::GetPropertyInfo (AudioUnitPropertyID	inID,
                                                        AudioUnitScope		inScope,
                                                        AudioUnitElement	inElement,
                                                        UInt32 &		outDataSize,
                                                        bool &		outWritable)
{
	return AUEffectBase::GetPropertyInfo (inID, inScope, inElement, outDataSize, outWritable);
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
// state that plugin supports only stereo-in/stereo-out processing
UInt32 Dehum::SupportedNumChannels(const AUChannelInfo ** outInfo)
{
	if (outInfo != NULL)
	{
		static AUChannelInfo info;
		info.inChannels = 2;
		info.outChannels = 2;
		*outInfo = &info;
	}

	return 1;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::GetProperty
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult			Dehum::GetProperty(	AudioUnitPropertyID inID,
                                                        AudioUnitScope 		inScope,
                                                        AudioUnitElement 	inElement,
                                                        void *			outData )
{
	return AUEffectBase::GetProperty (inID, inScope, inElement, outData);
}

//	Dehum::Initialize
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult Dehum::Initialize()
{
    ComponentResult result = AUEffectBase::Initialize();
    if (result == noErr) {
        Reset(kAudioUnitScope_Global, 0);
        //An AU is told its sample rate before it renders, which a VST is not,
        //so the one allocation a rate change forces happens here rather than on
        //the first render call. From here on updateConfig() has nothing left to
        //do that needs the heap - every slider retunes live in this core.
        updateConfig();
    }
    return result;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::paramsFromControls
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//Two of the controls have an off position rather than a range that reaches
//zero - Freq is automatic at the bottom and pinned from 10 Hz up, Rumble is off
//at the bottom and a corner frequency from 10 Hz up - so they get a dead zone
//at the bottom of the slider instead of a mapping that runs to 0 Hz.
static const float kOffZone = 0.02f;

static float withOffPosition(float control, float lo, float hi)
{
	if (control <= kOffZone) return 0.0f;
	return lo + ((control - kOffZone) / (1.0f - kOffZone)) * (hi - lo);
}

//Sliders to core units. The quantized one uses equal-width buckets, so D at 0
//lands on the single harmonic that the tuning was done at.
dehum::Params Dehum::paramsFromControls()
{
	dehum::Params p = dehum::Params::defaults();
	p.sensitivity = GetParameter( kParam_A );
	p.bandwidth   = 0.1f + GetParameter( kParam_B ) * 4.9f;      //0.1 .. 5 Hz
	p.searchTo    = 40.0f + GetParameter( kParam_C ) * 460.0f;   //40 .. 500 Hz
	p.harmonics   = 1 + (int)(GetParameter( kParam_D ) * 7.999f);         //1 .. 8
	p.frequency   = withOffPosition(GetParameter( kParam_E ), 10.0f, 500.0f);  //0 = automatic
	p.rumbleHz    = withOffPosition(GetParameter( kParam_F ), 10.0f, 200.0f);  //0 = off
	p.dryWet      = GetParameter( kParam_G );
	p.sanitize();
	return p;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::updateConfig
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//Called at the top of every render call, because that is how an AU finds out
//that a slider moved: by reading it and noticing.
void Dehum::updateConfig()
{
	const double rate = GetSampleRate();
	const dehum::Params p = paramsFromControls();
	if (!needsConfigure && rate == activeRate && p == active) return;

	dehum::Config next;
	next.compute(p, rate);

	//Only the sample rate sizes anything in this core, so every parameter move
	//goes in live - no reallocation, no reset, and no gap in the audio while the
	//host is dragging a slider. That is a better deal than Declick gets, where
	//Max repair and Model order have to rebuild the pipeline.
	if (!needsConfigure && next.structurallyEquals(cfg)
		&& chanL.retune(next) && chanR.retune(next)) {
		cfg = next;
		active = p;
		activeRate = rate;
		return;
	}

	chanL.configure(next);
	chanR.configure(next);

	cfg = next;
	active = p;
	activeRate = rate;
	needsConfigure = false;
	//No PropertyChanged(kAudioUnitProperty_Latency) here: the latency is zero
	//whatever the parameters are, so there is never anything to renegotiate.
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::DehumKernel::Reset()
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
ComponentResult		Dehum::Reset(AudioUnitScope inScope, AudioUnitElement inElement)
{
	//flush(), not reset(): the analysis window is stale across a transport jump
	//and the integrators are holding a discontinuity, but the hum on the far
	//side is the same hum. Forgetting the lines would mean several seconds of it
	//coming back every time the user hits play. This is the AU's resume().
	chanL.flush();
	chanR.flush();

	fpdL = 1.0; while (fpdL < 16386) fpdL = rand()*UINT32_MAX;
	fpdR = 1.0; while (fpdR < 16386) fpdR = rand()*UINT32_MAX;
	return noErr;
}

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//	Dehum::ProcessBufferLists
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
OSStatus		Dehum::ProcessBufferLists(AudioUnitRenderActionFlags & ioActionFlags,
													const AudioBufferList & inBuffer,
                                                    AudioBufferList & outBuffer,
                                                    UInt32 			inFramesToProcess)
{
	Float32 * inputL = (Float32*)(inBuffer.mBuffers[0].mData);
	Float32 * inputR = (Float32*)(inBuffer.mBuffers[1].mData);
	Float32 * outputL = (Float32*)(outBuffer.mBuffers[0].mData);
	Float32 * outputR = (Float32*)(outBuffer.mBuffers[1].mData);
	UInt32 nSampleFrames = inFramesToProcess;

	//FTZ for the duration of the buffer. The notch integrators run recursions
	//down towards zero, so this is not just a speed guard: it is part of the
	//numerical contract, and the ports only match each other while they all
	//agree about it. foo_dsp_dehum holds the same guard.
	dehum::scoped_flush_denormals ftz;
	updateConfig();

	//the notches are integrators, so they ring on into a gap for as long as it
	//takes them to settle. Zero latency is not the same claim as zero tail per
	//buffer, and a host that trusts the flag would clip that settling off.
	ioActionFlags &= ~kAudioUnitRenderAction_OutputIsSilence;

	while (nSampleFrames > 0) {
		const UInt32 n = (nSampleFrames > (UInt32)kScratch)
		               ? (UInt32)kScratch : nSampleFrames;

		//in place, so the copy is the only reason a scratch buffer is involved:
		//a host may hand us the same pointer for input and output, or not
		for (UInt32 i = 0; i < n; ++i) {
			scratchL[i] = inputL[i];
			scratchR[i] = inputR[i];
		}

		chanL.process(scratchL, (size_t)n, 1);
		chanR.process(scratchR, (size_t)n, 1);
		//non-finite input is silenced inside the core, so nothing can lodge
		//itself in a notch integrator and stay there for the rest of the stream

		for (UInt32 i = 0; i < n; ++i) {
			double inputSampleL = scratchL[i];
			double inputSampleR = scratchR[i];

			//begin 32 bit stereo floating point dither
			int expon; frexpf((float)inputSampleL, &expon);
			fpdL ^= fpdL << 13; fpdL ^= fpdL >> 17; fpdL ^= fpdL << 5;
			inputSampleL += ((double(fpdL)-uint32_t(0x7fffffff)) * 5.5e-36l * pow(2,expon+62));
			frexpf((float)inputSampleR, &expon);
			fpdR ^= fpdR << 13; fpdR ^= fpdR >> 17; fpdR ^= fpdR << 5;
			inputSampleR += ((double(fpdR)-uint32_t(0x7fffffff)) * 5.5e-36l * pow(2,expon+62));
			//end 32 bit stereo floating point dither

			outputL[i] = inputSampleL;
			outputR[i] = inputSampleR;
			//direct stereo out
		}

		inputL += n; inputR += n; outputL += n; outputR += n;
		nSampleFrames -= n;
	}
	return noErr;
}
