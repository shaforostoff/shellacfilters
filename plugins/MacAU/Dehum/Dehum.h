/*
*	File:		Dehum.h
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
#include "AUEffectBase.h"
#include "DehumVersion.h"

#if AU_DEBUG_DISPATCHER
	#include "AUDebugDispatcher.h"
#endif


#ifndef __Dehum_h__
#define __Dehum_h__

#include <math.h>

#include "dehum_core.h"

/*  Removes hum and other continuous tones from transfers, finding the
 *  frequencies itself rather than assuming 50 or 60 Hz. Not an Airwindows
 *  algorithm; MIT licensed like the rest of the tree. The maths lives in
 *  dehum_core.{h,cpp}, which is a verbatim copy of the file foo_dsp_dehum
 *  builds - see the note at the top of that header.
 *
 *  This is the Audio Unit of the same port that plugins/MacVST/Dehum is the
 *  VST of: same sliders, same mappings, same defaults, so the two formats hear
 *  the same thing at the same settings.
 */

#pragma mark ____Dehum Parameters

// parameters
//Slider positions for the calibrated defaults foo_dsp_dehum ships. Kept as the
//same expressions the VST uses because two of them are not round numbers -
//Bandwidth and Rumble are linear in Hz, so their default Hz values land
//mid-slider - and a literal would be a silent way for the ports to disagree
//about what "default" means.
static const float kDefaultValue_ParamA = 0.5;            //Sensitv 0.5 -> 16 dB
static const float kDefaultValue_ParamB = 0.9f / 4.9f;    //Bandwid -> 1.00 Hz
static const float kDefaultValue_ParamC = 60.0f / 460.0f; //SrchTo -> 100 Hz
static const float kDefaultValue_ParamD = 0.0;            //Harmncs -> 1
static const float kDefaultValue_ParamE = 0.0;            //Freq -> auto
static const float kDefaultValue_ParamF = 0.02f + (57.0f / 190.0f) * 0.98f;  //Rumble -> 67 Hz
static const float kDefaultValue_ParamG = 1.0;            //Dry/Wet

static CFStringRef kParameterAName = CFSTR("Sensitv");
static CFStringRef kParameterBName = CFSTR("Bandwid");
static CFStringRef kParameterCName = CFSTR("SrchTo");
static CFStringRef kParameterDName = CFSTR("Harmncs");
static CFStringRef kParameterEName = CFSTR("Freq");
static CFStringRef kParameterFName = CFSTR("Rumble");
static CFStringRef kParameterGName = CFSTR("Dry/Wet");

enum {
	kParam_A =0,
	kParam_B =1,
	kParam_C =2,
	kParam_D =3,
	kParam_E =4,
	kParam_F =5,
	kParam_G =6,
	//Add your parameters here...
	kNumberOfParameters=7
};

#pragma mark ____Dehum
class Dehum : public AUEffectBase
{
public:
	Dehum(AudioUnit component);
#if AU_DEBUG_DISPATCHER
	virtual ~Dehum () { delete mDebugDispatcher; }
#endif
	
	virtual ComponentResult Reset(AudioUnitScope inScope, AudioUnitElement inElement);

	virtual OSStatus ProcessBufferLists(AudioUnitRenderActionFlags & ioActionFlags, 
						const AudioBufferList & inBuffer, AudioBufferList & outBuffer, 
						UInt32 inFramesToProcess);
	virtual UInt32 SupportedNumChannels(const AUChannelInfo ** outInfo);

	virtual	ComponentResult		GetParameterValueStrings(AudioUnitScope			inScope,
														 AudioUnitParameterID		inParameterID,
														 CFArrayRef *			outStrings);
    
	virtual	ComponentResult		GetParameterInfo(AudioUnitScope			inScope,
												 AudioUnitParameterID	inParameterID,
												 AudioUnitParameterInfo	&outParameterInfo);
    
	//`bool &` where the other 542 folders here say `Boolean &`. AudioUnitSDK
	//spells it bool, and that is what scripts/build.sh builds against - so
	//this is what makes the override an override, not a typo to tidy up.
	virtual ComponentResult		GetPropertyInfo(AudioUnitPropertyID		inID,
												AudioUnitScope			inScope,
												AudioUnitElement		inElement,
												UInt32 &			outDataSize,
												bool	&			outWritable );
	
	virtual ComponentResult		GetProperty(AudioUnitPropertyID inID,
											AudioUnitScope 		inScope,
											AudioUnitElement 		inElement,
											void *			outData);

	virtual ComponentResult    Initialize();
	//Unlike Declick this one is zero latency and needs no tail: the detector
	//reads the signal but does not sit in the path, so what goes in comes out
	//aligned. Both of these stay at the airwindows nothing-to-declare zero.
	virtual bool				SupportsTail () { return true; }
    virtual Float64				GetTailTime() {return (1.0/GetSampleRate())*0.0;} //in SECONDS! gsr * a number = in samples
    virtual Float64				GetLatency() {return (1.0/GetSampleRate())*0.0;}	// in SECONDS! gsr * a number = in samples
	
	/*! @method Version */
	virtual ComponentResult		Version() { return kDehumVersion; }
	
	private:
	
	//the sliders are the host's 0..1; these turn them into the core's units,
	//and retune or reconfigure the two channels when anything actually moved.
	dehum::Params paramsFromControls();
	void updateConfig();

	dehum::Channel chanL;
	dehum::Channel chanR;
	dehum::Config cfg;
	dehum::Params active;
	double activeRate;
	bool needsConfigure;

	//The core works in place and the DSP is done in double, so the AU's float
	//buffers need somewhere to put doubles before dithering them back down.
	//Fixed size and a member, because the render thread must not allocate;
	//longer buffers are processed in chunks, which the core cannot tell apart
	//from one long call.
	enum { kScratch = 1024 };
	double scratchL[kScratch];
	double scratchR[kScratch];

	uint32_t fpdL;
	uint32_t fpdR;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


#endif
