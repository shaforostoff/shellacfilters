/* ========================================
 *  vst2_shim/audioeffectx.cpp
 *
 *  The dispatcher: where a host's opcode becomes a virtual call, and where the
 *  five C function pointers in AEffect land. MIT licensed with the rest of this
 *  tree; see vst2_abi.h for why this is not Steinberg's file.
 * ======================================== */

#include "audioeffectx.h"

#include <math.h>

/* ---------------------------------------------------------------------------
 *  The thunks
 *
 *  AEffect holds plain C function pointers, so each one is a free function that
 *  recovers the object from AEffect::object and makes the virtual call. This is
 *  the entire bridge between the host's C world and the plug-in's C++ one, and
 *  it is worth being clear about what each of these promises:
 *
 *    - the calling convention is __cdecl, because VSTCALLBACK is empty and MSVC
 *      defaults to __cdecl. Building this file with /Gz or /Gr would compile
 *      cleanly and then smash the stack of every host that calls in;
 *    - a C++ exception must not cross back out. Nothing in this tree throws in
 *      anger - the one throw statement is Airwindows house style in a default
 *      case a host cannot reach with a valid index - and the SDK does not guard
 *      either, so neither does this. An exception here terminates the process
 *      rather than unwinding into a host that may not even be MSVC-built,
 *      which is the lesser of the two evils;
 *    - effClose destroys the plug-in. The host will not free anything for us
 *      and will never touch the AEffect again, so a shim that forgets the
 *      delete leaks the whole instance. Dehum carries about two megabytes of
 *      analysis state per channel, so that is not a subtle leak - see the
 *      repeated open/close check in tests/vst_host_verify.cpp.
 * ------------------------------------------------------------------------ */

static AudioEffect * effectOf(AEffect * e)
{
    return (e && e->magic == kEffectMagic) ? (AudioEffect *)e->object : 0;
}

static VstIntPtr VSTCALLBACK DispatcherProc(AEffect * e, VstInt32 opcode, VstInt32 index,
                                            VstIntPtr value, void * ptr, float opt)
{
    AudioEffect * effect = effectOf(e);
    if (!effect) return 0;

    if (opcode == effClose) {
        effect->dispatcher(opcode, index, value, ptr, opt);
        delete effect;                 //the host's last word; nothing else frees this
        return 1;
    }
    return effect->dispatcher(opcode, index, value, ptr, opt);
}

/*  VST 1.0's accumulating process, deprecated since 2.4 and not implemented by
 *  anything here. It stays a real function rather than a null pointer because a
 *  host old enough to call it is old enough not to check first, and silence is
 *  a better failure than a jump to zero. Any host that reads
 *  effFlagsCanReplacing - which is every host since 1999 - uses
 *  processReplacing and never arrives here. */
static void VSTCALLBACK ProcessProc(AEffect * e, float ** inputs, float ** outputs,
                                    VstInt32 sampleFrames)
{
    (void)e; (void)inputs; (void)outputs; (void)sampleFrames;
}

static void VSTCALLBACK ProcessReplacingProc(AEffect * e, float ** inputs, float ** outputs,
                                             VstInt32 sampleFrames)
{
    AudioEffect * effect = effectOf(e);
    if (effect) effect->processReplacing(inputs, outputs, sampleFrames);
}

static void VSTCALLBACK ProcessDoubleReplacingProc(AEffect * e, double ** inputs, double ** outputs,
                                                   VstInt32 sampleFrames)
{
    AudioEffect * effect = effectOf(e);
    if (effect) effect->processDoubleReplacing(inputs, outputs, sampleFrames);
}

static void VSTCALLBACK SetParameterProc(AEffect * e, VstInt32 index, float value)
{
    AudioEffect * effect = effectOf(e);
    if (effect) effect->setParameter(index, value);
}

static float VSTCALLBACK GetParameterProc(AEffect * e, VstInt32 index)
{
    AudioEffect * effect = effectOf(e);
    return effect ? effect->getParameter(index) : 0.0f;
}

/* ---------------------------------------------------------------------------
 *  AudioEffect
 * ------------------------------------------------------------------------ */

AudioEffect::AudioEffect(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
{
    this->audioMaster = audioMaster;
    this->numPrograms = numPrograms;
    this->numParams   = numParams;
    curProgram        = 0;
    editor            = 0;     //until setEditor(); effFlagsHasEditor stays clear

    /*  The host's guess until it says otherwise with effSetSampleRate and
     *  effSetBlockSize. Every plug-in here re-derives its state when the rate
     *  it is given stops matching the rate it configured for, so these values
     *  only matter for the work done in the constructor. */
    sampleRate = 44100.0f;
    blockSize  = 1024;

    memset(&cEffect, 0, sizeof cEffect);   //future[] must be zero, and so must resvd1/2
    cEffect.magic        = kEffectMagic;
    cEffect.dispatcher   = DispatcherProc;
    cEffect.process      = ProcessProc;
    cEffect.setParameter = SetParameterProc;
    cEffect.getParameter = GetParameterProc;
    cEffect.processReplacing       = ProcessReplacingProc;
    cEffect.processDoubleReplacing = ProcessDoubleReplacingProc;
    cEffect.numPrograms  = numPrograms;
    cEffect.numParams    = numParams;
    cEffect.object       = this;
    cEffect.uniqueID     = VST_CCONST('N', 'o', 'E', 'f');   //until setUniqueID()
    cEffect.version      = 1;
}

AudioEffect::~AudioEffect() {}

void AudioEffect::setParameterAutomated(VstInt32 index, float value)
{
    setParameter(index, value);
    if (audioMaster) audioMaster(&cEffect, audioMasterAutomate, index, 0, 0, value);
}

VstIntPtr AudioEffect::dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value,
                                 void * ptr, float opt)
{
    VstIntPtr v = 0;

    switch (opcode) {
        case effOpen:  open();  break;
        case effClose: close(); break;

        case effSetProgram:
            if (value >= 0 && value < numPrograms) setProgram((VstInt32)value);
            break;
        case effGetProgram:     v = getProgram(); break;
        case effSetProgramName: setProgramName((char *)ptr); break;
        case effGetProgramName: getProgramName((char *)ptr); break;

        case effGetParamLabel:   getParameterLabel(index, (char *)ptr);   break;
        case effGetParamDisplay: getParameterDisplay(index, (char *)ptr); break;
        case effGetParamName:    getParameterName(index, (char *)ptr);    break;

        case effSetSampleRate: setSampleRate(opt); break;
        case effSetBlockSize:  setBlockSize((VstInt32)value); break;

        /*  The transport. A host calls this constantly - every stop and start,
         *  every loop point in some hosts - so what a plug-in throws away here
         *  is a real design decision, not boilerplate. */
        case effMainsChanged:
            if (value) resume(); else suspend();
            v = 1;
            break;

        case effGetChunk: v = getChunk((void **)ptr, index != 0); break;
        case effSetChunk: v = setChunk(ptr, (VstInt32)value, index != 0); break;

        /*  The editor. A plug-in that never called setEditor() has a null here
         *  and answers 0 to all four, which is what "no editor" looks like over
         *  the ABI and is exactly what this shim did before there was one.
         *
         *  effEditGetRect arrives before effEditOpen and again afterwards, and
         *  some hosts send it with no editor open at all to size a window they
         *  have not created yet - so it must not depend on open() having
         *  happened. The host reads through the pointer straight away and does
         *  not free it; the storage is the editor's. */
        case effEditGetRect:
            if (editor) {
                ERect * r = 0;
                v = (editor->getRect(&r) && r) ? 1 : 0;
                if (ptr) *(ERect **)ptr = r;
            }
            break;

        case effEditOpen:
            if (editor) v = editor->open(ptr) ? 1 : 0;
            break;

        /*  Not conditional on isOpen(): a host that sends effEditClose without
         *  a matching effEditOpen is not rare, and an editor that can only be
         *  closed once it believes it is open leaks a window when it meets one. */
        case effEditClose:
            if (editor) { editor->close(); v = 1; }
            break;

        case effEditIdle:
            if (editor) editor->idle();
            break;

        default: break;
    }
    return v;
}

/*  --- display helpers ---
 *
 *  The SDK hand-rolls these to avoid pulling in stdio. Here they are snprintf,
 *  with one improvement: the precision is chosen to fit maxLen rather than
 *  fixed and then truncated, because truncation is worse than rounding. At
 *  kVstMaxParamStrLen, "150.00" is a reading of 150 Hz and "150.000" cut to
 *  eight characters is a reading of something else. */

void AudioEffect::float2string(float value, char * text, VstInt32 maxLen)
{
    if (!text || maxLen <= 0) return;
    char buf[64];
    for (int precision = 3; precision >= 0; --precision) {
        snprintf(buf, sizeof buf, "%.*f", precision, (double)value);
        if ((VstInt32)strlen(buf) <= maxLen) { vst_strncpy(text, buf, (size_t)maxLen); return; }
    }
    //too wide even as an integer: at least keep it readable as a number
    snprintf(buf, sizeof buf, "%.1e", (double)value);
    vst_strncpy(text, buf, (size_t)maxLen);
}

void AudioEffect::int2string(VstInt32 value, char * text, VstInt32 maxLen)
{
    if (!text || maxLen <= 0) return;
    char buf[32];
    snprintf(buf, sizeof buf, "%d", (int)value);
    vst_strncpy(text, buf, (size_t)maxLen);
}

void AudioEffect::dB2string(float value, char * text, VstInt32 maxLen)
{
    if (value <= 0.0f) vst_strncpy(text, "-oo", (size_t)maxLen);
    else float2string((float)(20.0 * log10((double)value)), text, maxLen);
}

void AudioEffect::Hz2string(float samples, char * text, VstInt32 maxLen)
{
    if (samples <= 0.0f) { float2string(0.0f, text, maxLen); return; }
    float2string(sampleRate / samples, text, maxLen);
}

void AudioEffect::ms2string(float samples, char * text, VstInt32 maxLen)
{
    if (sampleRate <= 0.0f) { float2string(0.0f, text, maxLen); return; }
    float2string(samples * 1000.0f / sampleRate, text, maxLen);
}

/* ---------------------------------------------------------------------------
 *  AudioEffectX
 * ------------------------------------------------------------------------ */

AudioEffectX::AudioEffectX(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams)
    : AudioEffect(audioMaster, numPrograms, numParams)
{
}

bool AudioEffectX::ioChanged()
{
    /*  No host means nothing to renegotiate with, which is the situation in
     *  declick_vst_verify - it constructs the plug-in directly and passes its
     *  own callback in precisely so it can watch this happen. */
    if (!audioMaster) return false;
    return audioMaster(&cEffect, audioMasterIOChanged, 0, 0, 0, 0) != 0;
}

VstPlugCategory AudioEffectX::getPlugCategory()
{
    if (cEffect.flags & effFlagsIsSynth) return kPlugCategSynth;
    return kPlugCategUnknown;
}

VstIntPtr AudioEffectX::dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value,
                                  void * ptr, float opt)
{
    VstIntPtr v = 0;

    switch (opcode) {
        case effProcessEvents: v = processEvents((VstEvents *)ptr); break;

        case effCanBeAutomated:  v = canParameterBeAutomated(index) ? 1 : 0; break;
        case effString2Parameter: v = string2parameter(index, (char *)ptr) ? 1 : 0; break;

        case effGetProgramNameIndexed:
            v = getProgramNameIndexed((VstInt32)value, index, (char *)ptr) ? 1 : 0;
            break;

        case effGetPlugCategory: v = (VstIntPtr)getPlugCategory(); break;

        case effSetBypass: v = setBypass(value != 0) ? 1 : 0; break;

        case effGetEffectName:   v = getEffectName((char *)ptr) ? 1 : 0;    break;
        case effGetVendorString: v = getVendorString((char *)ptr) ? 1 : 0;  break;
        case effGetProductString:v = getProductString((char *)ptr) ? 1 : 0; break;
        case effGetVendorVersion:v = getVendorVersion(); break;

        case effVendorSpecific: v = vendorSpecific(index, value, ptr, opt); break;

        case effCanDo:       v = canDo((char *)ptr); break;
        case effGetTailSize: v = getGetTailSize();   break;
        case effGetVstVersion: v = getVstVersion();  break;

        case effStartProcess: v = startProcess(); break;
        case effStopProcess:  v = stopProcess();  break;

        case effSetProcessPrecision: v = setProcessPrecision((VstInt32)value) ? 1 : 0; break;

        case effGetNumMidiInputChannels:  v = getNumMidiInputChannels();  break;
        case effGetNumMidiOutputChannels: v = getNumMidiOutputChannels(); break;

        /*  effGetInputProperties, effGetOutputProperties, effGetSpeakerArrangement,
         *  effGetParameterProperties, the offline opcodes and the MIDI program
         *  ones all fall through to zero, which is "not supported" and is what
         *  the stock plug-ins answered by not overriding them. */
        default:
            v = AudioEffect::dispatcher(opcode, index, value, ptr, opt);
            break;
    }
    return v;
}
