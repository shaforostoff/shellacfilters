/* ========================================
 *  Dehum - Dehum.h
 *  Removes hum and other continuous tones from transfers, finding the
 *  frequencies itself rather than assuming 50 or 60 Hz. Not an Airwindows
 *  algorithm; MIT licensed like the rest of the tree. The maths lives in
 *  dehum_core.{h,cpp}, which is a verbatim copy of the file foo_dsp_dehum
 *  builds - see the note at the top of that header.
 * ======================================== */

#ifndef __Dehum_H
#define __Dehum_H

#ifndef __audioeffect__
#include "audioeffectx.h"
#endif

#include <set>
#include <string>
#include <math.h>

#include "dehum_core.h"

enum {
	kParamA =0,
	kParamB =1,
	kParamC =2,
	kParamD =3,
	kParamE =4,
	kParamF =5,
	kParamG =6,
  kNumParameters = 7
}; //

const int kNumPrograms = 0;
const int kNumInputs = 2;
const int kNumOutputs = 2;
const unsigned long kUniqueId = 'dhum';    //Change this to what the AU identity is!
const int kVersion = 0x00010001;           //1.0.1, matching the AU. See setVersion().

class Dehum :
    public AudioEffectX
{
public:
    Dehum(audioMasterCallback audioMaster);
    ~Dehum();
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
	//Unlike Declick this one is zero latency and needs no tail, so it says
	//nothing to the host about either. It is not stateless between takes
	//though - the detector has learned where the hum is - so resume() has an
	//opinion about what to keep.
	virtual void resume();                                        // transport start
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

	//A..G are the host's 0..1 sliders; these turn them into the core's units,
	//and retune or reconfigure the two channels when anything actually moved.
	dehum::Params paramsFromControls();
	void updateConfig();

	dehum::Channel chanL;
	dehum::Channel chanR;
	dehum::Config cfg;
	dehum::Params active;
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
