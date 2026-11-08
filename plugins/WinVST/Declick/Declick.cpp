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
	A = 0.9;  //Sensitv, 0.9 puts the trigger at 2.85 sigma
	B = 0.4677;  //Order, 128 - the old seven-slider builds' default position,
	             //which reads as 128 there too; 64 below, 256 above
	C = 1.0;  //Dry/Wet
	D = 0.0;  //Extra cautious, off

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

//Sliders to core units: the core's own mapping, which the AU and VirtualDJ
//ports use too, so a setting means the same in every format.
declick::Params Declick::paramsFromControls()
{
	const float s[declick::Params::kSliderCount] = { A, B, C };
	return declick::Params::fromSliders(s, D >= 0.5f);
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

	//Sensitivity and Dry/Wet need the same buffers, so they go in live. Model
	//order and Extra cautious resize the pipeline and cannot.
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
		ioChanged();   //only fires when Model order or Extra cautious moved, or the rate
	}
}

void Declick::resume()
{
	//The pipeline is holding the tail of whatever played last. Start the next
	//transport from silence instead of splicing that in. The rebuild happens
	//here rather than on the first process call: the host has set its real
	//sample rate by now, this is not the audio thread, and a latency
	//announced before the transport starts is one the host can still act on.
	needsPrime = true;
	{
		declick::scoped_flush_denormals ftz;
		updateConfig();
	}
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
	//The seven-slot layout of the builds that had seven sliders. The four
	//slots no slider fills get those builds' default positions, and Order goes
	//in as a position the old 16..256 slider reads as the same order, so a
	//preset saved here opens in one of them sounding as it does here.
	float *chunkData = chunk;
	chunkData[kChunkSensitivity] = A;
	chunkData[1] = 0.5f;  //Extent
	chunkData[2] = 0.2f;  //MaxLen, 4.0 ms
	chunkData[3] = 1.0f;  //Depth
	chunkData[4] = 0.5f;  //Passes, 2
	//B itself where the old slider already reads it as 64, 128 or 256 - the
	//default 0.4677 among them - so that a round trip gives back what was set
	{
		const float b = (B > 0.0f) ? ((B < 1.0f) ? B : 1.0f) : 0.0f;   //NaN to 0
		const int oldOrder = 16 + 8 * (int)(b * 30.999f);
		chunkData[kChunkOrder] = (oldOrder == declick::Params::snapOrder(oldOrder))
			? b : declick::Params::orderSlider(oldOrder);
	}
	chunkData[kChunkDryWet] = C;
	chunkData[kChunkMarker] = kChunkMarkerValue;
	chunkData[kChunkCautious] = (D >= 0.5f) ? 1.0f : 0.0f;
	/* Note: The way this is set up, it will break if you manage to save settings on an Intel
	 machine and load them on a PPC Mac. However, it's fine if you stick to the machine you
	 started with. */

	*data = chunkData;
	return kChunkSlots * sizeof(float);
}

VstInt32 Declick::setChunk (void* data, VstInt32 byteSize, bool isPreset)
{
	float *chunkData = (float *)data;
	//Slots 1..4 held Extent, MaxLen, Depth and Passes. They are not read: those
	//are at their defaults now whatever a preset says. Order's slot reads the
	//same way the old 16..256 slider did, snapped to the nearest of the three.
	A = pinParameter(chunkData[kChunkSensitivity]);
	B = pinParameter(chunkData[kChunkOrder]);
	C = pinParameter(chunkData[kChunkDryWet]);
	/* We're ignoring byteSize as we found it to be a filthy liar - except that
	 a chunk shorter than the marker's slot cannot have one, and must not be
	 read past its end. One that claims to be long enough is believed only if
	 the marker is there too. */
	D = 0.0f;
	if (byteSize >= (VstInt32)(kChunkSlots * sizeof(float))
		&& chunkData[kChunkMarker] == kChunkMarkerValue)
		D = pinParameter(chunkData[kChunkCautious]) >= 0.5f ? 1.0f : 0.0f;
	return 0;
}

void Declick::setParameter(VstInt32 index, float value) {
    switch (index) {
        case kParamA: A = value; break;
        case kParamB: B = value; break;
        case kParamC: C = value; break;
        case kParamD: D = value; break;
        default: break;
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
        default: break; // unknown parameter, shouldn't happen!
    } return 0.0; //we only need to update the relevant name, this is simple to manage
}

void Declick::getParameterName(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "Sensitv", kVstMaxParamStrLen); break;
		case kParamB: vst_strncpy (text, "Order", kVstMaxParamStrLen); break;
		case kParamC: vst_strncpy (text, "Dry/Wet", kVstMaxParamStrLen); break;
		case kParamD: vst_strncpy (text, "Caution", kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
    } //this is our labels for displaying in the VST host
}

void Declick::getParameterDisplay(VstInt32 index, char *text) {
	declick::Params p = paramsFromControls();
    switch (index) {
        case kParamA: float2string (A, text, kVstMaxParamStrLen); break;
        case kParamB: int2string ((VstInt32)p.order, text, kVstMaxParamStrLen); break;
        case kParamC: float2string (C, text, kVstMaxParamStrLen); break;
        case kParamD: vst_strncpy (text, (D >= 0.5f) ? "on" : "off", kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
	} //this displays the values and handles 'popups' where it's discrete choices
}

void Declick::getParameterLabel(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamB: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamC: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamD: vst_strncpy (text, "", kVstMaxParamStrLen); break;
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
