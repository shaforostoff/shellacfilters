/* ========================================
 *  Dehum - Dehum.cpp
 *  MIT license, as the rest of the tree
 * ======================================== */

#ifndef __Dehum_H
#include "Dehum.h"
#endif

AudioEffect* createEffectInstance(audioMasterCallback audioMaster) {return new Dehum(audioMaster);}

//Slider positions for the calibrated defaults foo_dsp_dehum ships. Kept as
//named constants because two of them are not round numbers - Bandwidth and
//Rumble are linear in Hz, so their default Hz values land mid-slider - and a
//literal would be a silent way for the two ports to disagree about what
//"default" means. dehum_vst_verify pins these against Params::defaults().
static const float kDefaultA = 0.5f;            //Sensitv 0.5 -> 16 dB
static const float kDefaultB = 0.9f / 4.9f;     //Bandwid -> 1.00 Hz
static const float kDefaultC = 60.0f / 460.0f;  //SearchTo -> 100 Hz
static const float kDefaultD = 0.0f;            //Harmnics -> 1
static const float kDefaultE = 0.0f;            //Freq -> auto
static const float kDefaultF = 0.02f + (57.0f / 190.0f) * 0.98f;  //Rumble -> 67 Hz
static const float kDefaultG = 1.0f;            //Dry/Wet

Dehum::Dehum(audioMasterCallback audioMaster) :
    AudioEffectX(audioMaster, kNumPrograms, kNumParameters)
{
	A = kDefaultA;
	B = kDefaultB;
	C = kDefaultC;
	D = kDefaultD;
	E = kDefaultE;
	F = kDefaultF;
	G = kDefaultG;

	//the channels allocate on configure(), so get that out of the audio thread
	//by doing it here at the host's guess of a sample rate. The first process
	//call redoes it if the host disagrees.
	active = paramsFromControls();
	cfg.compute(active, 44100.0);
	chanL.configure(cfg);
	chanR.configure(cfg);
	activeRate = 0.0;             //forces updateConfig() to run once for real
	needsConfigure = true;

	//Zero latency, and no tail: the detector reads the signal but does not sit
	//in the path, so what goes in comes out aligned. Nothing to declare.
	setInitialDelay(0);

	fpdL = 1.0; while (fpdL < 16386) fpdL = rand()*UINT32_MAX;
	fpdR = 1.0; while (fpdR < 16386) fpdR = rand()*UINT32_MAX;
	//this is reset: values being initialized only once. Startup values, whatever they are.

    _canDo.insert("plugAsChannelInsert"); // plug-in can be used as a channel insert effect.
    _canDo.insert("plugAsSend"); // plug-in can be used as a send effect.
    _canDo.insert("x2in2out");
    setNumInputs(kNumInputs);
    setNumOutputs(kNumOutputs);
    setUniqueID(kUniqueId);
    setVersion(kVersion);
    canProcessReplacing();     // supports output replacing
    canDoubleReplacing();      // supports double precision processing
	programsAreChunks(true);
    vst_strncpy (_programName, "Default", kVstMaxProgNameLen); // default program name
}

Dehum::~Dehum() {}
VstInt32 Dehum::getVendorVersion () {return kVersion;}
void Dehum::setProgramName(char *name) {vst_strncpy (_programName, name, kVstMaxProgNameLen);}
void Dehum::getProgramName(char *name) {vst_strncpy (name, _programName, kVstMaxProgNameLen);}
//airwindows likes to ignore this stuff. Make your own programs, and make a different plugin rather than
//trying to do versioning and preventing people from using older versions. Maybe they like the old one!

static float pinParameter(float data)
{
	if (data < 0.0f) return 0.0f;
	if (data > 1.0f) return 1.0f;
	return data;
}

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
	p.sensitivity = A;
	p.bandwidth   = 0.1f + B * 4.9f;                //0.1 .. 5 Hz
	p.searchTo    = 40.0f + C * 460.0f;             //40 .. 500 Hz
	p.harmonics   = 1 + (int)(D * 7.999f);          //1 .. 8
	p.frequency   = withOffPosition(E, 10.0f, 500.0f);   //0 = automatic
	p.rumbleHz    = withOffPosition(F, 10.0f, 200.0f);   //0 = off
	p.dryWet      = G;
	p.sanitize();
	return p;
}

//Called at the top of every process call, because a VST finds out about sample
//rate changes and parameter moves the same way: by noticing they happened.
void Dehum::updateConfig()
{
	const double rate = getSampleRate();
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
	//No setInitialDelay() or ioChanged() here: the latency is zero whatever the
	//parameters are, so there is never anything to renegotiate.
}

void Dehum::resume()
{
	//flush(), not reset(): the analysis window is stale across a transport jump
	//and the integrators are holding a discontinuity, but the hum on the far
	//side is the same hum. Forgetting the lines would mean several seconds of it
	//coming back every time the user hits play.
	chanL.flush();
	chanR.flush();
	AudioEffectX::resume();
}

VstInt32 Dehum::getChunk (void** data, bool isPreset)
{
	float *chunkData = (float *)calloc(kNumParameters, sizeof(float));
	chunkData[0] = A;
	chunkData[1] = B;
	chunkData[2] = C;
	chunkData[3] = D;
	chunkData[4] = E;
	chunkData[5] = F;
	chunkData[6] = G;
	/* Note: The way this is set up, it will break if you manage to save settings on an Intel
	 machine and load them on a PPC Mac. However, it's fine if you stick to the machine you
	 started with. */

	*data = chunkData;
	return kNumParameters * sizeof(float);
}

VstInt32 Dehum::setChunk (void* data, VstInt32 byteSize, bool isPreset)
{
	float *chunkData = (float *)data;
	A = pinParameter(chunkData[0]);
	B = pinParameter(chunkData[1]);
	C = pinParameter(chunkData[2]);
	D = pinParameter(chunkData[3]);
	E = pinParameter(chunkData[4]);
	F = pinParameter(chunkData[5]);
	G = pinParameter(chunkData[6]);
	/* We're ignoring byteSize as we found it to be a filthy liar */

	/* calculate any other fields you need here - you could copy in
	 code from setParameter() here. */
	return 0;
}

void Dehum::setParameter(VstInt32 index, float value) {
    switch (index) {
        case kParamA: A = value; break;
        case kParamB: B = value; break;
        case kParamC: C = value; break;
        case kParamD: D = value; break;
        case kParamE: E = value; break;
        case kParamF: F = value; break;
        case kParamG: G = value; break;
        default: throw; // unknown parameter, shouldn't happen!
    }
	//no work here: updateConfig() picks the change up on the audio thread,
	//where it can tell a live retune from a rebuild
}

float Dehum::getParameter(VstInt32 index) {
    switch (index) {
        case kParamA: return A; break;
        case kParamB: return B; break;
        case kParamC: return C; break;
        case kParamD: return D; break;
        case kParamE: return E; break;
        case kParamF: return F; break;
        case kParamG: return G; break;
        default: break; // unknown parameter, shouldn't happen!
    } return 0.0; //we only need to update the relevant name, this is simple to manage
}

void Dehum::getParameterName(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "Sensitv", kVstMaxParamStrLen); break;
		case kParamB: vst_strncpy (text, "Bandwid", kVstMaxParamStrLen); break;
		case kParamC: vst_strncpy (text, "SrchTo", kVstMaxParamStrLen); break;
		case kParamD: vst_strncpy (text, "Harmncs", kVstMaxParamStrLen); break;
		case kParamE: vst_strncpy (text, "Freq", kVstMaxParamStrLen); break;
		case kParamF: vst_strncpy (text, "Rumble", kVstMaxParamStrLen); break;
		case kParamG: vst_strncpy (text, "Dry/Wet", kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
    } //this is our labels for displaying in the VST host
}

void Dehum::getParameterDisplay(VstInt32 index, char *text) {
	dehum::Params p = paramsFromControls();
    switch (index) {
        case kParamA: float2string (A, text, kVstMaxParamStrLen); break;
        case kParamB: float2string (p.bandwidth, text, kVstMaxParamStrLen); break;
        case kParamC: float2string (p.searchTo, text, kVstMaxParamStrLen); break;
        case kParamD: int2string ((VstInt32)p.harmonics, text, kVstMaxParamStrLen); break;
        //the off positions say so rather than showing 0, which would read as a
        //frequency of zero rather than as "not doing that"
        case kParamE:
			if (p.frequency <= 0.0f) vst_strncpy (text, "auto", kVstMaxParamStrLen);
			else float2string (p.frequency, text, kVstMaxParamStrLen);
			break;
        case kParamF:
			if (p.rumbleHz <= 0.0f) vst_strncpy (text, "off", kVstMaxParamStrLen);
			else float2string (p.rumbleHz, text, kVstMaxParamStrLen);
			break;
        case kParamG: float2string (G, text, kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
	} //this displays the values and handles 'popups' where it's discrete choices
}

void Dehum::getParameterLabel(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamB: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        case kParamC: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        case kParamD: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamE: vst_strncpy (text, (E <= kOffZone) ? "" : "Hz", kVstMaxParamStrLen); break;
        case kParamF: vst_strncpy (text, (F <= kOffZone) ? "" : "Hz", kVstMaxParamStrLen); break;
        case kParamG: vst_strncpy (text, "", kVstMaxParamStrLen); break;
		default: break; // unknown parameter, shouldn't happen!
    }
}

VstInt32 Dehum::canDo(char *text)
{ return (_canDo.find(text) == _canDo.end()) ? -1: 1; } // 1 = yes, -1 = no, 0 = don't know

bool Dehum::getEffectName(char* name) {
    vst_strncpy(name, "Dehum", kVstMaxProductStrLen); return true;
}

VstPlugCategory Dehum::getPlugCategory() {return kPlugCategEffect;}

bool Dehum::getProductString(char* text) {
  	vst_strncpy (text, "Dehum (line detection)", kVstMaxProductStrLen); return true;
}

bool Dehum::getVendorString(char* text) {
	//not airwindows: DeCrackle is Chris Johnson's, this one is not
  	vst_strncpy (text, "ShellacFilters", kVstMaxVendorStrLen); return true;
}
