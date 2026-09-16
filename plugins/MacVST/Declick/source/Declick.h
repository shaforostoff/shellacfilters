/* ========================================
 *  Declick - Declick.h
 *  An autoregressive detect-and-interpolate declicker for shellac and vinyl
 *  transfers. Not an Airwindows algorithm; MIT licensed like the rest of the
 *  tree. The maths lives in declick_core.{h,cpp}, which is a verbatim copy of
 *  the file foo_dsp_declick builds - see the note at the top of that header.
 * ======================================== */

#ifndef __Declick_H
#define __Declick_H

#ifndef __audioeffect__
#include "audioeffectx.h"
#endif

#include <set>
#include <string>
#include <math.h>

#include "declick_core.h"

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
const unsigned long kUniqueId = 'dclk';    //Change this to what the AU identity is!
const int kVersion = 0x00010001;           //1.0.1, matching the AU. See setVersion().

class Declick :
    public AudioEffectX
{
public:
    Declick(audioMasterCallback audioMaster);
    ~Declick();
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
	//the rest of airwindows is zero latency and stateless between takes. This
	//one is neither, so it has to talk to the host about both:
	virtual void resume();                                        // transport start: don't play the previous take back
	virtual VstInt32 getGetTailSize();                            // so an offline bounce collects the last block
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
	//default stuff
};

#endif
