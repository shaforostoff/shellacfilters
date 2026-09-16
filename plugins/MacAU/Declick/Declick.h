/*
*	File:		Declick.h
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
#include "DeclickVersion.h"

#if AU_DEBUG_DISPATCHER
	#include "AUDebugDispatcher.h"
#endif


#ifndef __Declick_h__
#define __Declick_h__

#include <math.h>

#include "declick_core.h"

/*  An autoregressive detect-and-interpolate declicker for shellac and vinyl
 *  transfers. Not an Airwindows algorithm; MIT licensed like the rest of the
 *  tree. The maths lives in declick_core.{h,cpp}, which is a verbatim copy of
 *  the file foo_dsp_declick builds - see the note at the top of that header.
 *
 *  This is the Audio Unit of the same port that plugins/MacVST/Declick is the
 *  VST of: same sliders, same mappings, same defaults, so the two formats hear
 *  the same thing at the same settings.
 */

#pragma mark ____Declick Parameters

// parameters
//these are the calibrated defaults foo_dsp_declick ships, expressed as slider
//positions: see paramsFromControls() for the mappings, and the foobar2000_dsp
//README for how each number was arrived at.
static const float kDefaultValue_ParamA = 0.6;  //Sensitv, 0.6 puts the trigger at 3.9 sigma
static const float kDefaultValue_ParamB = 0.5;  //Extent
static const float kDefaultValue_ParamC = 0.2;  //MaxLen, 4.0 ms
static const float kDefaultValue_ParamD = 0.0;  //Depth, the fraction that adds the least error of its own
static const float kDefaultValue_ParamE = 0.5;  //Passes, 2
static const float kDefaultValue_ParamF = 1.0;  //Order, 64 - the top of this slider, which stops
                                                //where the real-time budget does; the core itself
                                                //goes to kMaxOrder
static const float kDefaultValue_ParamG = 1.0;  //Dry/Wet

static CFStringRef kParameterAName = CFSTR("Sensitv");
static CFStringRef kParameterBName = CFSTR("Extent");
static CFStringRef kParameterCName = CFSTR("MaxLen");
static CFStringRef kParameterDName = CFSTR("Depth");
static CFStringRef kParameterEName = CFSTR("Passes");
static CFStringRef kParameterFName = CFSTR("Order");
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

#pragma mark ____Declick
class Declick : public AUEffectBase
{
public:
	Declick(AudioUnit component);
#if AU_DEBUG_DISPATCHER
	virtual ~Declick () { delete mDebugDispatcher; }
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
	//the rest of airwindows is zero latency and stateless between takes. This
	//one is neither, so it has to talk to the host about both. GetLatency() is
	//what setInitialDelay() is to the VST and GetTailTime() what
	//getGetTailSize() is: without the second, an offline bounce comes up short
	//by the reported delay.
	virtual bool				SupportsTail () { return true; }
    virtual Float64				GetTailTime() {return (1.0/GetSampleRate())*cfg.latency;} //in SECONDS! gsr * a number = in samples
    virtual Float64				GetLatency() {return (1.0/GetSampleRate())*cfg.latency;}	// in SECONDS! gsr * a number = in samples
	
	/*! @method Version */
	virtual ComponentResult		Version() { return kDeclickVersion; }
	
	private:
	
	//the sliders are the host's 0..1; these turn them into the core's units,
	//and reconfigure or retune the two channels when anything actually moved.
	declick::Params paramsFromControls();
	void updateConfig();

	declick::Channel chanL;
	declick::Channel chanR;
	declick::Config cfg;
	declick::Params active;
	double activeRate;
	int reportedLatency;
	bool needsPrime;

	uint32_t fpdL;
	uint32_t fpdR;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


#endif
