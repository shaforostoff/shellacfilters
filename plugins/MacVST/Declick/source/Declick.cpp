/* ========================================
 *  Declick - Declick.cpp
 *  MIT license, as the rest of the tree
 * ======================================== */

#ifndef __Declick_H
#include "Declick.h"
#endif

AudioEffect* createEffectInstance(audioMasterCallback audioMaster) {return new Declick(audioMaster);}

Declick::Declick(audioMasterCallback audioMaster) :
    AudioEffectX(audioMaster, kNumPrograms, kNumParameters)
{
	//these are the calibrated defaults foo_dsp_declick ships, expressed as
	//slider positions: see paramsFromControls() for the mappings, and the
	//foobar2000_dsp README for how each number was arrived at.
	A = 0.6;  //Sensitv, 0.6 puts the trigger at 3.9 sigma
	B = 0.5;  //Extent
	C = 0.2;  //MaxLen, 4.0 ms
	D = 0.0;  //Depth, the fraction that adds the least error of its own
	E = 0.5;  //Passes, 2
	F = 1.0;  //Order, 64 - the top of this slider, which stops where the
	          //real-time budget does; the core itself goes to kMaxOrder
	G = 1.0;  //Dry/Wet

	//the channels allocate on configure(), so get that out of the audio thread
	//by doing it here at the host's guess of a sample rate. The first process
	//call redoes it if the host disagrees.
	active = paramsFromControls();
	cfg.compute(active, 44100.0);
	chanL.configure(cfg);
	chanR.configure(cfg);
	activeRate = 0.0;             //forces updateConfig() to run once for real
	reportedLatency = cfg.latency;
	needsPrime = true;
	setInitialDelay(cfg.latency);

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

Declick::~Declick() {}
VstInt32 Declick::getVendorVersion () {return kVersion;}
void Declick::setProgramName(char *name) {vst_strncpy (_programName, name, kVstMaxProgNameLen);}
void Declick::getProgramName(char *name) {vst_strncpy (name, _programName, kVstMaxProgNameLen);}
//airwindows likes to ignore this stuff. Make your own programs, and make a different plugin rather than
//trying to do versioning and preventing people from using older versions. Maybe they like the old one!

static float pinParameter(float data)
{
	if (data < 0.0f) return 0.0f;
	if (data > 1.0f) return 1.0f;
	return data;
}

//Sliders to core units. The quantized ones use equal-width buckets, so E at 0.5
//lands on the 2 passes the tuning was done at, and F at 1.0 on order 64.
declick::Params Declick::paramsFromControls()
{
	declick::Params p = declick::Params::defaults();
	p.sensitivity = A;
	p.extent      = B;
	p.maxLengthMs = C * 20.0f;                     //0.2 .. 20 ms, clamped below
	p.depth       = D;
	p.passes      = 1 + (int)(E * 2.999f);         //1, 2, 3
	p.order       = 8 + 8 * (int)(F * 7.999f);     //8 .. 64 in eights
	p.dryWet      = G;
	p.sanitize();
	return p;
}

//Called at the top of every process call, because a VST finds out about sample
//rate changes and parameter moves the same way: by noticing they happened.
void Declick::updateConfig()
{
	const double rate = getSampleRate();
	const declick::Params p = paramsFromControls();
	if (!needsPrime && rate == activeRate && p == active) return;

	declick::Config next;
	next.compute(p, rate);

	//Sensitivity, Extent, Depth, Passes and Dry/Wet need the same buffers, so
	//they go in live. Max repair and Model order resize the pipeline and cannot.
	if (!needsPrime && next.structurallyEquals(cfg)
		&& chanL.retune(next) && chanR.retune(next)) {
		cfg = next;
		active = p;
		activeRate = rate;
		return;
	}

	chanL.configure(next);
	chanR.configure(next);
	//pre-roll, so that from here on every push of n samples leaves at least n
	//ready to pull and the host's n-in-n-out contract is met without the core
	//ever having to zero-fill mid-stream
	chanL.prime();
	chanR.prime();

	cfg = next;
	active = p;
	activeRate = rate;
	needsPrime = false;

	if (next.latency != reportedLatency) {
		reportedLatency = next.latency;
		setInitialDelay(next.latency);
		ioChanged();   //only fires when Max repair or Model order moved
	}
}

void Declick::resume()
{
	//The pipeline is holding the tail of whatever played last. Start the next
	//transport from silence instead of splicing that in.
	needsPrime = true;
	AudioEffectX::resume();
}

VstInt32 Declick::getGetTailSize()
{
	//What the host has to keep pulling for after the last input sample, or an
	//offline bounce comes up short by the reported delay.
	return cfg.latency;
}

VstInt32 Declick::getChunk (void** data, bool isPreset)
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

VstInt32 Declick::setChunk (void* data, VstInt32 byteSize, bool isPreset)
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

void Declick::setParameter(VstInt32 index, float value) {
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

float Declick::getParameter(VstInt32 index) {
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

void Declick::getParameterName(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "Sensitv", kVstMaxParamStrLen); break;
		case kParamB: vst_strncpy (text, "Extent", kVstMaxParamStrLen); break;
		case kParamC: vst_strncpy (text, "MaxLen", kVstMaxParamStrLen); break;
		case kParamD: vst_strncpy (text, "Depth", kVstMaxParamStrLen); break;
		case kParamE: vst_strncpy (text, "Passes", kVstMaxParamStrLen); break;
		case kParamF: vst_strncpy (text, "Order", kVstMaxParamStrLen); break;
		case kParamG: vst_strncpy (text, "Dry/Wet", kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
    } //this is our labels for displaying in the VST host
}

void Declick::getParameterDisplay(VstInt32 index, char *text) {
	declick::Params p = paramsFromControls();
    switch (index) {
        case kParamA: float2string (A, text, kVstMaxParamStrLen); break;
        case kParamB: float2string (B, text, kVstMaxParamStrLen); break;
        case kParamC: float2string (p.maxLengthMs, text, kVstMaxParamStrLen); break;
        case kParamD: float2string (D, text, kVstMaxParamStrLen); break;
        case kParamE: int2string ((VstInt32)p.passes, text, kVstMaxParamStrLen); break;
        case kParamF: int2string ((VstInt32)p.order, text, kVstMaxParamStrLen); break;
        case kParamG: float2string (G, text, kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
	} //this displays the values and handles 'popups' where it's discrete choices
}

void Declick::getParameterLabel(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamB: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamC: vst_strncpy (text, "ms", kVstMaxParamStrLen); break;
        case kParamD: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamE: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamF: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamG: vst_strncpy (text, "", kVstMaxParamStrLen); break;
		default: break; // unknown parameter, shouldn't happen!
    }
}

VstInt32 Declick::canDo(char *text)
{ return (_canDo.find(text) == _canDo.end()) ? -1: 1; } // 1 = yes, -1 = no, 0 = don't know

bool Declick::getEffectName(char* name) {
    vst_strncpy(name, "Declick", kVstMaxProductStrLen); return true;
}

VstPlugCategory Declick::getPlugCategory() {return kPlugCategEffect;}

bool Declick::getProductString(char* text) {
  	vst_strncpy (text, "Declick (AR interpolation)", kVstMaxProductStrLen); return true;
}

bool Declick::getVendorString(char* text) {
	//not airwindows: DeCrackle is Chris Johnson's, this one is not
  	vst_strncpy (text, "ShellacFilters", kVstMaxVendorStrLen); return true;
}
