/* ========================================
 *  ParaEQ - ParaEQ.h
 *  A console channel equaliser for disc transfers: high-pass, low shelf, two
 *  peaking bands, high shelf, output trim. Not an Airwindows algorithm; MIT
 *  licensed like the rest of the tree. The maths lives in paraeq_core.{h,cpp},
 *  which is a verbatim copy of the file foo_dsp_paraeq builds - see the note at
 *  the top of that header.
 *
 *  Sixteen parameters, which is a lot for a host's generic slider list - so
 *  there is an editor, and it is the same curve with five handles on it that
 *  the foobar2000 build draws. See ParaEQEditor.h. The sliders are still there
 *  underneath it, because that is what a VST parameter is and what a host
 *  automates; the editor moves them rather than replacing them.
 *
 *  The mapping from sliders to the core's units is in ParaEQ.cpp, and every
 *  frequency and Q slider is logarithmic there - which matters less now that a
 *  generic list is the fallback rather than the interface, but a host's own
 *  knob is still the only thing some users will see.
 *
 *  The names are the ones the restoration literature uses - Low cut, Bass,
 *  Reverb cut, Brilliance, Hiss cut - rather than a console's HP/LF/LMF/HMF/HF,
 *  so that an automation lane says what the knob is for. kVstMaxParamStrLen is
 *  eight characters, which is why two of them are abbreviated; the editor has
 *  the room to write them out in full and does.
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
#include "ParaEQEditor.h"

enum {
	kParamA =0,     //Low cut  - corner        (the strip's HP)
	kParamB =1,     //Slope    - off/12/24
	kParamC =2,     //Bass     - gain          (LF)
	kParamD =3,     //Bass Hz  - corner
	kParamE =4,     //Bass Shp - shelf or bell
	kParamF =5,     //Revrb    - gain          (LMF)
	kParamG =6,     //Revrb Hz - centre
	kParamH =7,     //Revrb Q
	kParamI =8,     //Brill    - gain          (HMF)
	kParamJ =9,     //Brill Hz - centre
	kParamK =10,    //Brill Q
	kParamL =11,    //Hiss     - gain          (HF)
	kParamM =12,    //Hiss Hz  - corner
	kParamN =13,    //Hiss Shp - shelf or bell
	kParamO =14,    //Output
	kParamP =15,    //Bypass
  kNumParameters = 16
}; //

const int kNumPrograms = 0;
const int kNumInputs = 2;
const int kNumOutputs = 2;
const unsigned long kUniqueId = 'preq';    //Change this to what the AU identity is!
const int kVersion = 0x00010000;           //1.0.0, matching the AU. See setVersion().

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

	//The inverse: the sixteen slider positions that mean `p`. Static because it
	//is a pure mapping with no plug-in state in it, and public because
	//ParaEQEditor turns a dragged curve back into sliders with it.
	static void controlsFromParams(const paraeq::Params & p, float * out);

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

	//setControlsFromParams() writes controlsFromParams() into A..P, and exists
	//so the constructor can place the sliders on paraeq::Params::defaults()
	//rather than on a second copy of those numbers written out in this file.
	//updateConfig() is what notices a slider moved.
	void setControlsFromParams(const paraeq::Params & p);
	void updateConfig();

	//By value and always constructed, because it costs a few dozen bytes until a
	//host opens it: the window is created by ParaEQEditor::open() and nothing
	//before that touches the screen. The constructor hands its address to
	//setEditor(), which is what sets effFlagsHasEditor.
	ParaEQEditor editorImpl;

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
