/* ========================================
 *  ParaEQ - ParaEQ.h
 *  A console channel equaliser for disc transfers: high-pass, low shelf, two
 *  peaking bands, high shelf, output trim. Not an Airwindows algorithm; MIT
 *  licensed like the rest of the tree. The maths lives in paraeq_core.{h,cpp},
 *  which is a verbatim copy of the file foo_dsp_paraeq builds - see the note at
 *  the top of that header.
 *
 *  Sixteen parameters, which is a lot for a host's generic slider list and is
 *  the price of a fixed layout: the foobar2000 build draws a curve and lets the
 *  user grab a band, and a VST2 with no editor has nothing to draw with. The
 *  mapping from sliders to the core's units is in ParaEQ.cpp, and every
 *  frequency and Q slider is logarithmic there - the one thing a generic UI can
 *  still get right.
 * ======================================== */

#ifndef __ParaEQ_H
#define __ParaEQ_H

#ifndef __audioeffect__
#include "audioeffectx.h"
#endif

#include <set>
#include <string>
#include <math.h>

#include "paraeq_core.h"

enum {
	kParamA =0,     //HP Freq
	kParamB =1,     //HP Slope
	kParamC =2,     //LF Gain
	kParamD =3,     //LF Freq
	kParamE =4,     //LF Shape
	kParamF =5,     //LMF Gain
	kParamG =6,     //LMF Freq
	kParamH =7,     //LMF Q
	kParamI =8,     //HMF Gain
	kParamJ =9,     //HMF Freq
	kParamK =10,    //HMF Q
	kParamL =11,    //HF Gain
	kParamM =12,    //HF Freq
	kParamN =13,    //HF Shape
	kParamO =14,    //Output
	kParamP =15,    //Bypass
  kNumParameters = 16
}; //

const int kNumPrograms = 0;
const int kNumInputs = 2;
const int kNumOutputs = 2;
const unsigned long kUniqueId = 'preq';    //Change this to what the AU identity is!

class ParaEQ :
    public AudioEffectX
{
public:
    ParaEQ(audioMasterCallback audioMaster);
    ~ParaEQ();
    virtual bool getEffectName(char* name);                       // The plug-in name
    virtual VstPlugCategory getPlugCategory();                    // The general category for the plug-in
    virtual bool getProductString(char* text);                    // This is a unique plug-in string provided by Steinberg
    virtual bool getVendorString(char* text);                     // Vendor info
    virtual VstInt32 getVendorVersion();                          // Version number
    virtual void processReplacing (float** inputs, float** outputs, VstInt32 sampleFrames);
    virtual void processDoubleReplacing (double** inputs, double** outputs, VstInt32 sampleFrames);
    virtual void getProgramName(char *name);                      // read the name from the host
    virtual void setProgramName(char *name);                      // changes the name of the preset displayed in the host
	virtual VstInt32 getChunk (void** data, bool isPreset);
	virtual VstInt32 setChunk (void* data, VstInt32 byteSize, bool isPreset);
    virtual float getParameter(VstInt32 index);                   // get the parameter value at the specified index
    virtual void setParameter(VstInt32 index, float value);       // set the parameter at index to value
    virtual void getParameterLabel(VstInt32 index, char *text);  // label for the parameter (eg dB)
    virtual void getParameterName(VstInt32 index, char *text);    // name of the parameter
    virtual void getParameterDisplay(VstInt32 index, char *text); // text description of the current value
    virtual VstInt32 canDo(char *text);
	//Zero latency and no tail, as Dehum is, so nothing is declared about
	//either. Unlike Dehum there is also nothing learned from the signal: the
	//only state is the biquads' and where a glide had got to, so resume() wants
	//the first of those dropped and the second kept.
	virtual void resume();                                        // transport start

	//What the sixteen sliders currently mean, in the core's units. Public
	//because paraeq_vst_verify needs the exact Params this plug-in is running
	//in order to build the same Config and compare to the bit, and the
	//alternative - a second copy of the mapping living in the test - is the
	//thing that would drift. Not part of the VST2 interface; a host never sees
	//it.
	paraeq::Params paramsFromControls();

private:
    char _programName[kVstMaxProgNameLen + 1];
    std::set< std::string > _canDo;

    float A;
    float B;
    float C;
    float D;
    float E;
    float F;
    float G;
    float H;
    float I;
    float J;
    float K;
    float L;
    float M;
    float N;
    float O;
    float P;

	//setControlsFromParams() is the inverse of paramsFromControls() above, and
	//exists so the constructor can place the sliders on
	//paraeq::Params::defaults() rather than on a second copy of those numbers
	//written out in this file. updateConfig() is what notices a slider moved.
	void setControlsFromParams(const paraeq::Params & p);
	void updateConfig();

	paraeq::Channel chanL;
	paraeq::Channel chanR;
	paraeq::Config cfg;
	paraeq::Params active;
	double activeRate;
	bool needsConfigure;

	//The core works in place and the DSP is done in double, so the float path
	//needs somewhere to put doubles before dithering them back down. Fixed
	//size and a member, because the audio thread must not allocate; longer
	//buffers are processed in chunks, which the core cannot tell apart from one
	//long call.
	enum { kScratch = 1024 };
	double scratchL[kScratch];
	double scratchR[kScratch];

	uint32_t fpdL;
	uint32_t fpdR;
	//default stuff
};

#endif
