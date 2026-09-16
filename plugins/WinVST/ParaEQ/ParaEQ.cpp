/* ========================================
 *  ParaEQ - ParaEQ.cpp
 *  MIT license, as the rest of the tree
 * ======================================== */

#ifndef __ParaEQ_H
#include "ParaEQ.h"
#endif

AudioEffect* createEffectInstance(audioMasterCallback audioMaster) {return new ParaEQ(audioMaster);}

static float pinParameter(float data)
{
	if (data < 0.0f) return 0.0f;
	if (data > 1.0f) return 1.0f;
	return data;
}

//Frequency and Q take logarithmic sliders. A linear control over 1.5 to 16 kHz
//spends four fifths of its travel in the top octave and a half, which is not
//where any of these bands do their work: the useful settings on a shellac
//transfer are clustered at the bottom of every one of these ranges. A log
//slider gives each octave the same amount of knob, so 2 kHz and 8 kHz are the
//same distance apart under the finger as 1 kHz and 4 kHz are.
//
//Gains stay linear, because dB already is: the core's units are decibels, so
//a linear slider over -20..+20 dB puts unity exactly at the centre detent.
static float logFromControl(float control, float lo, float hi)
{
	return lo * powf(hi / lo, control);
}

static float controlFromLog(float value, float lo, float hi)
{
	if (!(value > 0.0f) || !(lo > 0.0f) || !(hi > lo)) return 0.0f;
	return logf(value / lo) / logf(hi / lo);
}

static float linearFromControl(float control, float lo, float hi)
{
	return lo + control * (hi - lo);
}

static float controlFromLinear(float value, float lo, float hi)
{
	if (hi <= lo) return 0.0f;
	return (value - lo) / (hi - lo);
}

//The discrete controls - the high-pass slope and the three switches - get
//equal-width buckets, and the inverse returns the centre of one rather than its
//lower edge. A round trip through the centre cannot land on a boundary and come
//back as the neighbouring setting, which is the whole failure mode a stored
//preset has to survive.
static int bucketFromControl(float control, int count)
{
	int v = (int)(control * (float)count);
	if (v < 0) v = 0;
	if (v >= count) v = count - 1;
	return v;
}

static float controlFromBucket(int index, int count)
{
	if (count < 1) return 0.0f;
	if (index < 0) index = 0;
	if (index >= count) index = count - 1;
	return ((float)index + 0.5f) / (float)count;
}

//Sliders to core units.
paraeq::Params ParaEQ::paramsFromControls()
{
	paraeq::Params p = paraeq::Params::defaults();

	p.hpFrequency  = logFromControl(A, paraeq::kHpFreqMin, paraeq::kHpFreqMax);
	p.hpSlope      = bucketFromControl(B, 3);           //off, 12, 24 dB/oct

	p.lfGain       = linearFromControl(C, -paraeq::kGainMaxDb, paraeq::kGainMaxDb);
	p.lfFrequency  = logFromControl(D, paraeq::kLfFreqMin, paraeq::kLfFreqMax);
	p.lfBell       = (bucketFromControl(E, 2) != 0);

	p.lmfGain      = linearFromControl(F, -paraeq::kGainMaxDb, paraeq::kGainMaxDb);
	p.lmfFrequency = logFromControl(G, paraeq::kLmfFreqMin, paraeq::kLmfFreqMax);
	p.lmfQ         = logFromControl(H, paraeq::kQMin, paraeq::kQMax);

	p.hmfGain      = linearFromControl(I, -paraeq::kGainMaxDb, paraeq::kGainMaxDb);
	p.hmfFrequency = logFromControl(J, paraeq::kHmfFreqMin, paraeq::kHmfFreqMax);
	p.hmfQ         = logFromControl(K, paraeq::kQMin, paraeq::kQMax);

	p.hfGain       = linearFromControl(L, -paraeq::kGainMaxDb, paraeq::kGainMaxDb);
	p.hfFrequency  = logFromControl(M, paraeq::kHfFreqMin, paraeq::kHfFreqMax);
	p.hfBell       = (bucketFromControl(N, 2) != 0);

	p.outputGain   = linearFromControl(O, -paraeq::kOutputMaxDb, paraeq::kOutputMaxDb);
	p.bypass       = (bucketFromControl(P, 2) != 0);

	p.sanitize();
	return p;
}

//Core units back to sliders. Only the constructor calls this, and only with
//paraeq::Params::defaults() - which is the point of it existing. Dehum writes
//its default slider positions out as constants and has a test pinning them
//against the core's defaults; running the mapping backwards instead means there
//is nothing to pin, because this file no longer holds a second opinion about
//what a default is.
void ParaEQ::setControlsFromParams(const paraeq::Params & in)
{
	paraeq::Params p = in;
	p.sanitize();

	A = pinParameter(controlFromLog(p.hpFrequency, paraeq::kHpFreqMin, paraeq::kHpFreqMax));
	B = controlFromBucket(p.hpSlope, 3);

	C = pinParameter(controlFromLinear(p.lfGain, -paraeq::kGainMaxDb, paraeq::kGainMaxDb));
	D = pinParameter(controlFromLog(p.lfFrequency, paraeq::kLfFreqMin, paraeq::kLfFreqMax));
	E = controlFromBucket(p.lfBell ? 1 : 0, 2);

	F = pinParameter(controlFromLinear(p.lmfGain, -paraeq::kGainMaxDb, paraeq::kGainMaxDb));
	G = pinParameter(controlFromLog(p.lmfFrequency, paraeq::kLmfFreqMin, paraeq::kLmfFreqMax));
	H = pinParameter(controlFromLog(p.lmfQ, paraeq::kQMin, paraeq::kQMax));

	I = pinParameter(controlFromLinear(p.hmfGain, -paraeq::kGainMaxDb, paraeq::kGainMaxDb));
	J = pinParameter(controlFromLog(p.hmfFrequency, paraeq::kHmfFreqMin, paraeq::kHmfFreqMax));
	K = pinParameter(controlFromLog(p.hmfQ, paraeq::kQMin, paraeq::kQMax));

	L = pinParameter(controlFromLinear(p.hfGain, -paraeq::kGainMaxDb, paraeq::kGainMaxDb));
	M = pinParameter(controlFromLog(p.hfFrequency, paraeq::kHfFreqMin, paraeq::kHfFreqMax));
	N = controlFromBucket(p.hfBell ? 1 : 0, 2);

	O = pinParameter(controlFromLinear(p.outputGain, -paraeq::kOutputMaxDb, paraeq::kOutputMaxDb));
	P = controlFromBucket(p.bypass ? 1 : 0, 2);
}

ParaEQ::ParaEQ(audioMasterCallback audioMaster) :
    AudioEffectX(audioMaster, kNumPrograms, kNumParameters)
{
	//The sixteen sliders start wherever the core's own defaults put them - flat,
	//with every band parked on the target its knob is for.
	setControlsFromParams(paraeq::Params::defaults());

	//Nothing here allocates - heapBytes() is 0 and every buffer is a fixed
	//member - so this is not the guard against an allocating audio thread that
	//the sibling plug-ins need it to be. It is here so that the first process
	//call has coefficients rather than a default-constructed Config, at the
	//host's guess of a sample rate; the first call redoes it if the host
	//disagrees.
	active = paramsFromControls();
	cfg.compute(active, 44100.0);
	chanL.configure(cfg);
	chanR.configure(cfg);
	activeRate = 0.0;             //forces updateConfig() to run once for real
	needsConfigure = true;

	//Zero latency, and no tail: an equaliser is a filter cascade in the path
	//with no read-ahead and nothing held back. Nothing to declare.
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
    canProcessReplacing();     // supports output replacing
    canDoubleReplacing();      // supports double precision processing
	programsAreChunks(true);
    vst_strncpy (_programName, "Default", kVstMaxProgNameLen); // default program name
}

ParaEQ::~ParaEQ() {}
VstInt32 ParaEQ::getVendorVersion () {return 1000;}
void ParaEQ::setProgramName(char *name) {vst_strncpy (_programName, name, kVstMaxProgNameLen);}
void ParaEQ::getProgramName(char *name) {vst_strncpy (name, _programName, kVstMaxProgNameLen);}
//airwindows likes to ignore this stuff. Make your own programs, and make a different plugin rather than
//trying to do versioning and preventing people from using older versions. Maybe they like the old one!

//Called at the top of every process call, because a VST finds out about sample
//rate changes and parameter moves the same way: by noticing they happened.
void ParaEQ::updateConfig()
{
	const double rate = getSampleRate();
	const paraeq::Params p = paramsFromControls();
	if (!needsConfigure && rate == activeRate && p == active) return;

	paraeq::Config next;
	next.compute(p, rate);

	//A sample rate change is snapped to and a parameter move is glided to, and
	//those are the only two cases: nothing in this core is sized by the
	//parameters, so retune() cannot fail and there is no rebuild branch to fall
	//back to - not even the one Dehum needs for a rate change. A glide across a
	//rate change would be gliding over a discontinuity, and the filter state
	//describes samples at a rate that is no longer the rate, so configure()
	//takes it instead.
	if (needsConfigure || rate != activeRate) {
		chanL.configure(next);
		chanR.configure(next);
		needsConfigure = false;
	} else {
		//What a control move costs: one coefficient design, then 20 ms of
		//gliding to it. That is why dragging a slider here does not step the
		//audio, and why there is no gap in it either.
		chanL.retune(next);
		chanR.retune(next);
	}

	cfg = next;
	active = p;
	activeRate = rate;
	//No setInitialDelay() or ioChanged() here: the latency is zero whatever the
	//parameters are, so there is never anything to renegotiate.
}

void ParaEQ::resume()
{
	//flush(), not reset(): the biquad states describe samples that are no longer
	//adjacent to what comes next, so they go, but the coefficients and where a
	//glide had got to are the user's settings and stay. reset() would snap a
	//glide in progress to its target, which is the one thing a transport start
	//has no business doing.
	chanL.flush();
	chanR.flush();
	AudioEffectX::resume();
}

VstInt32 ParaEQ::getChunk (void** data, bool isPreset)
{
	float *chunkData = (float *)calloc(kNumParameters, sizeof(float));
	chunkData[0] = A;
	chunkData[1] = B;
	chunkData[2] = C;
	chunkData[3] = D;
	chunkData[4] = E;
	chunkData[5] = F;
	chunkData[6] = G;
	chunkData[7] = H;
	chunkData[8] = I;
	chunkData[9] = J;
	chunkData[10] = K;
	chunkData[11] = L;
	chunkData[12] = M;
	chunkData[13] = N;
	chunkData[14] = O;
	chunkData[15] = P;
	/* Note: The way this is set up, it will break if you manage to save settings on an Intel
	 machine and load them on a PPC Mac. However, it's fine if you stick to the machine you
	 started with. */

	*data = chunkData;
	return kNumParameters * sizeof(float);
}

VstInt32 ParaEQ::setChunk (void* data, VstInt32 byteSize, bool isPreset)
{
	float *chunkData = (float *)data;
	A = pinParameter(chunkData[0]);
	B = pinParameter(chunkData[1]);
	C = pinParameter(chunkData[2]);
	D = pinParameter(chunkData[3]);
	E = pinParameter(chunkData[4]);
	F = pinParameter(chunkData[5]);
	G = pinParameter(chunkData[6]);
	H = pinParameter(chunkData[7]);
	I = pinParameter(chunkData[8]);
	J = pinParameter(chunkData[9]);
	K = pinParameter(chunkData[10]);
	L = pinParameter(chunkData[11]);
	M = pinParameter(chunkData[12]);
	N = pinParameter(chunkData[13]);
	O = pinParameter(chunkData[14]);
	P = pinParameter(chunkData[15]);
	/* We're ignoring byteSize as we found it to be a filthy liar */

	/* calculate any other fields you need here - you could copy in
	 code from setParameter() here. */
	return 0;
}

void ParaEQ::setParameter(VstInt32 index, float value) {
    switch (index) {
        case kParamA: A = value; break;
        case kParamB: B = value; break;
        case kParamC: C = value; break;
        case kParamD: D = value; break;
        case kParamE: E = value; break;
        case kParamF: F = value; break;
        case kParamG: G = value; break;
        case kParamH: H = value; break;
        case kParamI: I = value; break;
        case kParamJ: J = value; break;
        case kParamK: K = value; break;
        case kParamL: L = value; break;
        case kParamM: M = value; break;
        case kParamN: N = value; break;
        case kParamO: O = value; break;
        case kParamP: P = value; break;
        default: throw; // unknown parameter, shouldn't happen!
    }
	//no work here: updateConfig() picks the change up on the audio thread, where
	//it can tell a rate change from a control move
}

float ParaEQ::getParameter(VstInt32 index) {
    switch (index) {
        case kParamA: return A; break;
        case kParamB: return B; break;
        case kParamC: return C; break;
        case kParamD: return D; break;
        case kParamE: return E; break;
        case kParamF: return F; break;
        case kParamG: return G; break;
        case kParamH: return H; break;
        case kParamI: return I; break;
        case kParamJ: return J; break;
        case kParamK: return K; break;
        case kParamL: return L; break;
        case kParamM: return M; break;
        case kParamN: return N; break;
        case kParamO: return O; break;
        case kParamP: return P; break;
        default: break; // unknown parameter, shouldn't happen!
    } return 0.0; //we only need to update the relevant name, this is simple to manage
}

void ParaEQ::getParameterName(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "HP Freq", kVstMaxParamStrLen); break;
		case kParamB: vst_strncpy (text, "HP Slope", kVstMaxParamStrLen); break;
		case kParamC: vst_strncpy (text, "LF Gain", kVstMaxParamStrLen); break;
		case kParamD: vst_strncpy (text, "LF Freq", kVstMaxParamStrLen); break;
		case kParamE: vst_strncpy (text, "LF Shape", kVstMaxParamStrLen); break;
		case kParamF: vst_strncpy (text, "LMF Gain", kVstMaxParamStrLen); break;
		case kParamG: vst_strncpy (text, "LMF Freq", kVstMaxParamStrLen); break;
		case kParamH: vst_strncpy (text, "LMF Q", kVstMaxParamStrLen); break;
		case kParamI: vst_strncpy (text, "HMF Gain", kVstMaxParamStrLen); break;
		case kParamJ: vst_strncpy (text, "HMF Freq", kVstMaxParamStrLen); break;
		case kParamK: vst_strncpy (text, "HMF Q", kVstMaxParamStrLen); break;
		case kParamL: vst_strncpy (text, "HF Gain", kVstMaxParamStrLen); break;
		case kParamM: vst_strncpy (text, "HF Freq", kVstMaxParamStrLen); break;
		case kParamN: vst_strncpy (text, "HF Shape", kVstMaxParamStrLen); break;
		case kParamO: vst_strncpy (text, "Output", kVstMaxParamStrLen); break;
		case kParamP: vst_strncpy (text, "Bypass", kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
    } //this is our labels for displaying in the VST host
}

//The high-pass slope, in dB/oct, by bucket. Index 0 is off rather than 0 dB/oct,
//which is the same thing and reads better on a slider.
static const char * kSlopeText[3] = { "off", "12", "24" };

void ParaEQ::getParameterDisplay(VstInt32 index, char *text) {
	paraeq::Params p = paramsFromControls();
    switch (index) {
        case kParamA: float2string (p.hpFrequency, text, kVstMaxParamStrLen); break;
        case kParamB: vst_strncpy (text, kSlopeText[bucketFromControl(B, 3)], kVstMaxParamStrLen); break;
        case kParamC: float2string (p.lfGain, text, kVstMaxParamStrLen); break;
        case kParamD: float2string (p.lfFrequency, text, kVstMaxParamStrLen); break;
        //the two shelves can be told to be bells instead, which is a different
        //filter rather than a different setting of one, so it says which
        case kParamE: vst_strncpy (text, p.lfBell ? "bell" : "shelf", kVstMaxParamStrLen); break;
        case kParamF: float2string (p.lmfGain, text, kVstMaxParamStrLen); break;
        case kParamG: float2string (p.lmfFrequency, text, kVstMaxParamStrLen); break;
        case kParamH: float2string (p.lmfQ, text, kVstMaxParamStrLen); break;
        case kParamI: float2string (p.hmfGain, text, kVstMaxParamStrLen); break;
        case kParamJ: float2string (p.hmfFrequency, text, kVstMaxParamStrLen); break;
        case kParamK: float2string (p.hmfQ, text, kVstMaxParamStrLen); break;
        case kParamL: float2string (p.hfGain, text, kVstMaxParamStrLen); break;
        case kParamM: float2string (p.hfFrequency, text, kVstMaxParamStrLen); break;
        case kParamN: vst_strncpy (text, p.hfBell ? "bell" : "shelf", kVstMaxParamStrLen); break;
        case kParamO: float2string (p.outputGain, text, kVstMaxParamStrLen); break;
        case kParamP: vst_strncpy (text, p.bypass ? "on" : "off", kVstMaxParamStrLen); break;
        default: break; // unknown parameter, shouldn't happen!
	} //this displays the values and handles 'popups' where it's discrete choices
}

void ParaEQ::getParameterLabel(VstInt32 index, char *text) {
    switch (index) {
        case kParamA: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        //an off high-pass has no slope, so it gets no unit either
        case kParamB: vst_strncpy (text, (bucketFromControl(B, 3) == 0) ? "" : "dB/oct", kVstMaxParamStrLen); break;
        case kParamC: vst_strncpy (text, "dB", kVstMaxParamStrLen); break;
        case kParamD: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        case kParamE: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamF: vst_strncpy (text, "dB", kVstMaxParamStrLen); break;
        case kParamG: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        case kParamH: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamI: vst_strncpy (text, "dB", kVstMaxParamStrLen); break;
        case kParamJ: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        case kParamK: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamL: vst_strncpy (text, "dB", kVstMaxParamStrLen); break;
        case kParamM: vst_strncpy (text, "Hz", kVstMaxParamStrLen); break;
        case kParamN: vst_strncpy (text, "", kVstMaxParamStrLen); break;
        case kParamO: vst_strncpy (text, "dB", kVstMaxParamStrLen); break;
        case kParamP: vst_strncpy (text, "", kVstMaxParamStrLen); break;
		default: break; // unknown parameter, shouldn't happen!
    }
}

VstInt32 ParaEQ::canDo(char *text)
{ return (_canDo.find(text) == _canDo.end()) ? -1: 1; } // 1 = yes, -1 = no, 0 = don't know

bool ParaEQ::getEffectName(char* name) {
    vst_strncpy(name, "ParaEQ", kVstMaxProductStrLen); return true;
}

VstPlugCategory ParaEQ::getPlugCategory() {return kPlugCategEffect;}

bool ParaEQ::getProductString(char* text) {
  	vst_strncpy (text, "ParaEQ (console strip)", kVstMaxProductStrLen); return true;
}

bool ParaEQ::getVendorString(char* text) {
	//not airwindows: this one is not Chris Johnson's
  	vst_strncpy (text, "ShellacFilters", kVstMaxVendorStrLen); return true;
}
