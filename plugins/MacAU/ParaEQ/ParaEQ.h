/*
*	File:		ParaEQ.h
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
#include "ParaEQVersion.h"

#if AU_DEBUG_DISPATCHER
	#include "AUDebugDispatcher.h"
#endif


#ifndef __ParaEQ_h__
#define __ParaEQ_h__

#include <math.h>

#include "paraeq_core.h"

/*  A console channel equaliser for disc transfers: high-pass, low shelf, two
 *  peaking bands, high shelf, output trim. Not an Airwindows algorithm; MIT
 *  licensed like the rest of the tree. The maths lives in paraeq_core.{h,cpp},
 *  which is a verbatim copy of the file foo_dsp_paraeq builds - see the note at
 *  the top of that header.
 *
 *  This is the Audio Unit of the same equaliser the foobar2000 component runs,
 *  and unlike Declick and Dehum it has no VST twin to stay interchangeable
 *  with. That is worth one deliberate difference from its two siblings.
 *
 *  Those two publish every control as a generic 0..1 slider, because a VST
 *  parameter is a float in that range and the two formats have to mean the same
 *  thing at the same setting - which costs the host any idea of what a position
 *  represents, so Max repair reads 0.2 rather than "4.0 ms". Here there is no
 *  second format to agree with, so the parameters carry their real units:
 *  frequencies in Hz, gains in dB, the high-pass slope as an indexed control
 *  with names. A generic AU view then draws "100 Hz" and "-3.5 dB", and - more
 *  useful than either - the number a host stores is the number
 *  paraeq::Params holds, so a setting written down here is the same setting in
 *  foo_dsp_paraeq with no mapping in between.
 */

#pragma mark ____ParaEQ Parameters

// parameters
//
//Ranges are paraeq_core's own, named rather than repeated: a control that could
//drift out of step with the core is a control that can ask for a filter the
//core will clamp behind the host's back. Defaults come from Params::defaults()
//at construction for the same reason, so the only numbers written out here are
//the ones the AU needs and the core does not have - names, units and clumps.

static CFStringRef kParameterAName = CFSTR("HP Freq");
static CFStringRef kParameterBName = CFSTR("HP Slope");
static CFStringRef kParameterCName = CFSTR("Low Gain");
static CFStringRef kParameterDName = CFSTR("Low Freq");
static CFStringRef kParameterEName = CFSTR("Low Bell");
static CFStringRef kParameterFName = CFSTR("LoMid Gain");
static CFStringRef kParameterGName = CFSTR("LoMid Freq");
static CFStringRef kParameterHName = CFSTR("LoMid Q");
static CFStringRef kParameterIName = CFSTR("HiMid Gain");
static CFStringRef kParameterJName = CFSTR("HiMid Freq");
static CFStringRef kParameterKName = CFSTR("HiMid Q");
static CFStringRef kParameterLName = CFSTR("High Gain");
static CFStringRef kParameterMName = CFSTR("High Freq");
static CFStringRef kParameterNName = CFSTR("High Bell");
static CFStringRef kParameterOName = CFSTR("Output");
static CFStringRef kParameterPName = CFSTR("Bypass");

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

//Sixteen controls is a wall of sliders in a generic view unless they are
//grouped, so each band is a clump and the host draws the headings. Clump 0 is
//reserved by the API for "no clump", so these start at 1.
enum {
	kClump_HighPass  = 1,
	kClump_Low       = 2,
	kClump_LowMid    = 3,
	kClump_HighMid   = 4,
	kClump_High      = 5,
	kClump_Output    = 6,
	kNumberOfClumps  = 6
};

//The high-pass slope is the one control whose number means nothing on its own.
enum { kNumberOfSlopes = 3 };

#pragma mark ____ParaEQ
class ParaEQ : public AUEffectBase
{
public:
	ParaEQ(AudioUnit component);
#if AU_DEBUG_DISPATCHER
	virtual ~ParaEQ () { delete mDebugDispatcher; }
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

	//Names the six clumps the controls are grouped into. This rather than a
	//GetProperty case: kAudioUnitProperty_ParameterClumpName is dispatched to
	//this virtual before a subclass GetProperty is ever consulted.
	virtual ComponentResult		CopyClumpName(AudioUnitScope		inScope,
											  UInt32				inClumpID,
											  UInt32				inDesiredNameLength,
											  CFStringRef *			outClumpName);

	virtual ComponentResult    Initialize();
	//Zero latency and no read-ahead, so unlike the two siblings there is no
	//GetLatency() or GetTailTime() to override and no tail to declare. The
	//biquad states do decay rather than stop, but a host that trusts
	//SupportsTail() to mean "keep rendering after the input ends" would be
	//holding an equaliser open for the sake of a ring 120 dB down; the
	//foobar2000 component reports 0 for the same reason and the two agree.
	
	/*! @method Version */
	virtual ComponentResult		Version() { return kParaEQVersion; }
	
	private:
	
	//the host's parameters are already in the core's units - see the note above
	//- so this gathers rather than converts, and retunes when anything moved.
	paraeq::Params paramsFromControls();
	void updateConfig();

	//Two channels, because SupportedNumChannels() offers mono and stereo and
	//nothing wider. Fixed members: paraeq::Channel has heapBytes() == 0, so
	//there is nothing here for a rate change or a control move to allocate.
	paraeq::Channel chan[2];
	paraeq::Config cfg;
	paraeq::Params active;
	double activeRate;
	bool needsReset;
};

//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~


#endif
