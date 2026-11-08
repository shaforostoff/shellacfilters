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

//Sensitivity, Model order, Dry/Wet and Extra cautious: the controls
//Params::restrictToControls() keeps. The other four the stages after the AR
//repair were tuned against at their defaults, so they are not offered.
enum {
	kParamA =0,  //Sensitv
	kParamB =1,  //Order: 64, 128 or 256
	kParamC =2,  //Dry/Wet
	kParamD =3,  //Cautious: off below 0.5, on from it
  kNumParameters = 4
}; //

//A chunk keeps the seven-float layout of the builds that had seven sliders, so
//presets travel both ways: these are the slots the three sliders live in, and
//the other four are written as the old defaults and ignored on the way in.
//Extra cautious goes after them, behind a marker: older builds read seven
//floats and stop, and a seven-float chunk from one of them has no marker
//where this build looks for it, so it reads as off.
enum {
	kChunkSensitivity = 0,
	kChunkOrder = 5,
	kChunkDryWet = 6,
	kChunkMarker = 7,
	kChunkCautious = 8,
	kChunkSlots = 9
};
const float kChunkMarkerValue = 8051.0f;   //"cautious follows"; no slider gives it

const int kNumPrograms = 0;
const int kNumInputs = 2;
const int kNumOutputs = 2;
const unsigned long kUniqueId = 'dclk';    //Change this to what the AU identity is!
const int kVersion = 0x00010102;           //1.1.2, matching the AU. See setVersion().

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
    float chunk[kChunkSlots];   //!< what getChunk() hands the host, which never frees it

	//A..D are the host's 0..1 sliders; these turn them into the core's units,
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
