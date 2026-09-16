/* ========================================
 *  vst2_shim/audioeffectx.h
 *
 *  The C++ side of the shim: AudioEffect and AudioEffectX, the two classes an
 *  Airwindows-pattern plug-in derives from. MIT licensed with the rest of this
 *  tree, and not Steinberg's header of the same name - see vst2_abi.h for why
 *  there is a reimplementation here at all.
 *
 *  The filename is not a choice: every plug-in in plugins/WinVST and
 *  plugins/LinuxVST opens with
 *
 *      #ifndef __audioeffect__
 *      #include "audioeffectx.h"
 *      #endif
 *
 *  so this has to be called that and has to define that guard macro. What it
 *  contains is the surface those plug-ins actually use, plus the virtuals the
 *  dispatcher needs somewhere to send an opcode. It is not the whole SDK:
 *  there is no MIDI, no offline processing, no speaker arrangements, no
 *  parameter properties. Every one of those is answered "not supported", which
 *  is what the stock plug-ins answered anyway by not overriding them.
 *
 *  There IS an editor, as of ParaEQ. AEffEditor below is the SDK's class cut
 *  down to the four opcodes a host actually needs to put a window on screen -
 *  getRect, open, close, idle - with no VSTGUI and no knowledge of what is
 *  drawn inside. A plug-in that calls setEditor() gets effFlagsHasEditor set
 *  for it and the eff*Edit* opcodes routed; one that does not is exactly the
 *  plug-in it was before, which is why Declick and Dehum are untouched by
 *  this and still assert that no editor is claimed.
 *
 *  Deliberate departures from the SDK, all in the same direction - toward the
 *  plug-in actually loading and behaving:
 *
 *    setProcessPrecision() returns true for both precisions instead of the
 *        SDK's blanket false. A plug-in that sets effFlagsCanDoubleReplacing
 *        and then tells the host it cannot switch precision is contradicting
 *        itself, and some hosts believe the second answer.
 *    float2string() picks a precision that fits the field instead of
 *        truncating a fixed one, so 150 Hz reads "150.00" rather than being
 *        cut off mid-number.
 *    getAeffect() is public. It is real SDK API, but the SDK keeps it
 *        protected-ish by convention; the DLL entry point and the tests both
 *        need it.
 *    VSTPluginMain() does not refuse a host that answers 0 to
 *        audioMasterVersion - see the note in vstplugmain.cpp.
 *
 *  Anything else that differs is a bug. The tests are what say which is which:
 *  declick_vst_verify and dehum_vst_verify compile against this header
 *  directly, and vst_host_verify loads a finished plug-in and talks to it only
 *  through the C ABI, so the dispatcher wiring underneath is exercised too.
 * ======================================== */

#ifndef __audioeffectx__
#define __audioeffectx__

/*  The guard the plug-in sources test before including this file. */
#ifndef __audioeffect__
#define __audioeffect__
#endif

#include "vst2_abi.h"

#include <stdio.h>
#include <string.h>

/*  The SDK's own bounded copy: at most maxLen characters, always terminated,
 *  and the terminator goes at dst[maxLen] - so the caller's buffer needs
 *  maxLen + 1 bytes, which is why every plug-in here declares
 *  _programName[kVstMaxProgNameLen + 1]. */
inline void vst_strncpy(char * dst, const char * src, size_t maxLen)
{
    if (!dst) return;
    if (!src) { dst[0] = 0; return; }
    size_t i = 0;
    for (; i < maxLen && src[i]; ++i) dst[i] = src[i];
    dst[i] = 0;
}

inline void vst_strncat(char * dst, const char * src, size_t maxLen)
{
    if (!dst || !src) return;
    size_t at = strlen(dst);
    if (at >= maxLen) { dst[maxLen] = 0; return; }
    vst_strncpy(dst + at, src, maxLen - at);
}

/* ---------------------------------------------------------------------------
 *  AEffEditor - the plug-in's window, as much of it as the ABI cares about
 *
 *  The SDK's class carries a VSTGUI dependency and a pile of opcodes no host
 *  in practice sends. This is the part that matters: the host asks how big the
 *  window wants to be, hands over a parent to put it in, takes it away again,
 *  and pings it while it is up. Everything about what the window contains is
 *  the subclass's business.
 *
 *  Lifetime is the plug-in's. The editor is constructed by the plug-in's
 *  constructor and destroyed by its destructor, and open()/close() may happen
 *  many times in between - a host opens the window when the user asks for it
 *  and closes it when they put it away, repeatedly, without the plug-in being
 *  touched. So an editor must survive close() with its state intact, and must
 *  not assume open() happens at all.
 * ------------------------------------------------------------------------ */

class AudioEffect;

class AEffEditor
{
public:
    AEffEditor() : effect(0), systemWindow(0) {}
    virtual ~AEffEditor() {}

    /*  The size the editor wants, as a pointer to storage the EDITOR owns. The
     *  host reads it and moves on; it never frees it. Return false, or leave
     *  *rect null, and the host will decide the size itself - usually badly. */
    virtual bool getRect(ERect ** rect) { (void)rect; return false; }

    /*  `ptr` is the platform's parent window: an HWND on Windows, an NSView on
     *  macOS, a Window on X11. The host has already created it and will destroy
     *  it after close(), so the editor parents its own window to it and must
     *  not outlive it. Returning false tells the host the editor did not open. */
    virtual bool open(void * ptr) { systemWindow = ptr; return true; }

    /*  The window is going away. Whatever open() created is destroyed here, not
     *  in the destructor: the host may open it again afterwards. */
    virtual void close() { systemWindow = 0; }

    /*  Called by the host while the editor is up, at whatever rate it likes and
     *  possibly never. Nothing here needs it - the editor is a real child
     *  window with its own message loop behind it - so the default does
     *  nothing, and that is the honest implementation rather than a stub. */
    virtual void idle() {}

    virtual bool isOpen() { return systemWindow != 0; }

    /*  Set by AudioEffect::setEditor(), so an editor can reach the plug-in it
     *  belongs to without being handed it twice. */
    virtual void setEffect(AudioEffect * eff) { effect = eff; }
    AudioEffect * getEffect() const { return effect; }

    void * getSystemWindow() const { return systemWindow; }

protected:
    AudioEffect * effect;
    void *        systemWindow;
};

/* ---------------------------------------------------------------------------
 *  AudioEffect - VST 1.0's surface, and the part that owns the AEffect
 * ------------------------------------------------------------------------ */

class AudioEffect
{
public:
    AudioEffect(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams);
    virtual ~AudioEffect();

    /*  Everything the host asks that is not audio arrives here. Overridden by
     *  AudioEffectX to add the 2.x opcodes; a plug-in never touches it. */
    virtual VstIntPtr dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value,
                                 void * ptr, float opt);

    virtual void open() {}
    virtual void close() {}

    /*  suspend() is the transport stopping, resume() it starting. Note that a
     *  host may call resume() many times per session and is entitled to expect
     *  the plug-in not to play the previous take back. */
    virtual void suspend() {}
    virtual void resume() {}

    virtual void setParameter(VstInt32 index, float value) { (void)index; (void)value; }
    virtual float getParameter(VstInt32 index) { (void)index; return 0.0f; }

    /*  For a plug-in whose own UI moved a control - tells the host to record
     *  the automation, then applies it. Nothing in this tree has a UI. */
    virtual void setParameterAutomated(VstInt32 index, float value);

    virtual VstInt32 getProgram() { return curProgram; }
    virtual void setProgram(VstInt32 program) { curProgram = program; }
    virtual void setProgramName(char * name) { (void)name; }
    virtual void getProgramName(char * name) { if (name) name[0] = 0; }

    virtual void getParameterLabel(VstInt32 index, char * label) { (void)index; if (label) label[0] = 0; }
    virtual void getParameterDisplay(VstInt32 index, char * text) { (void)index; if (text) text[0] = 0; }
    virtual void getParameterName(VstInt32 index, char * text) { (void)index; if (text) text[0] = 0; }

    virtual void setSampleRate(float rate)      { sampleRate = rate; }
    virtual void setBlockSize(VstInt32 frames)  { blockSize  = frames; }

    /*  Pure, as in the SDK from 2.4 on: a plug-in that cannot replace has
     *  nothing to offer a modern host. The double variant is optional. */
    virtual void processReplacing(float ** inputs, float ** outputs, VstInt32 sampleFrames) = 0;
    virtual void processDoubleReplacing(double ** inputs, double ** outputs, VstInt32 sampleFrames)
    { (void)inputs; (void)outputs; (void)sampleFrames; }

    /*  Opaque preset blobs. isPreset distinguishes one program from the bank;
     *  with numPrograms == 0 the distinction is academic. */
    virtual VstInt32 getChunk(void ** data, bool isPreset = false)
    { (void)data; (void)isPreset; return 0; }
    virtual VstInt32 setChunk(void * data, VstInt32 byteSize, bool isPreset = false)
    { (void)data; (void)byteSize; (void)isPreset; return 0; }

    /*  What VSTPluginMain hands the host. */
    AEffect * getAeffect() { return &cEffect; }

    /*  --- what the plug-in constructor calls --- */
    void setNumInputs(VstInt32 inputs)   { cEffect.numInputs  = inputs; }
    void setNumOutputs(VstInt32 outputs) { cEffect.numOutputs = outputs; }
    void setUniqueID(VstInt32 iD)        { cEffect.uniqueID   = iD; }

    /*  The plug-in's own version, byte packed and most significant first, so
     *  0x00010001 is 1.0.1. Hosts that print a version split it back into
     *  bytes - Audacity does - which is why the stock Airwindows answer of
     *  1000 shows up there as "3.232" rather than as anything like a version.
     *  Whatever is set here should also come back from getVendorVersion(): a
     *  host reads whichever of the two it prefers, and Audacity asks for the
     *  vendor version first and only falls back to this field. The three
     *  plug-ins in this tree keep the number in kVersion, beside kUniqueId,
     *  and their AU ports carry the same one in k<Name>Version. */
    void setVersion(VstInt32 version)    { cEffect.version    = version; }
    void canProcessReplacing(bool state = true) { setFlag(effFlagsCanReplacing, state); }
    void programsAreChunks(bool state = true)   { setFlag(effFlagsProgramChunks, state); }

    /*  Latency, in samples. The host reads the field out of AEffect whenever it
     *  feels like it, so setting this before the host has looked is enough at
     *  construction time; changing it later also needs ioChanged(). */
    void setInitialDelay(VstInt32 delay) { cEffect.initialDelay = delay; }
    VstInt32 getInitialDelay() const { return cEffect.initialDelay; }

    /*  Hands the plug-in its editor and sets effFlagsHasEditor, which is the
     *  flag a host reads to decide whether to offer a window at all. Passing 0
     *  clears both. The plug-in keeps ownership - this stores the pointer and
     *  does not delete it - because an Airwindows-pattern plug-in holds its
     *  editor as a member and the destructor order is then the compiler's
     *  problem rather than ours. */
    void setEditor(AEffEditor * e) {
        editor = e;
        if (editor) editor->setEffect(this);
        setFlag(effFlagsHasEditor, editor != 0);
    }
    AEffEditor * getEditor() const { return editor; }

    float    getSampleRate() { return sampleRate; }
    VstInt32 getBlockSize()  { return blockSize; }
    VstInt32 getNumPrograms() const { return numPrograms; }
    VstInt32 getNumParams()   const { return numParams; }

    /*  --- the SDK's display helpers ---
     *  maxLen is the character budget, normally kVstMaxParamStrLen, and these
     *  aim to use it rather than overrun it: 150 Hz in eight characters is
     *  "150.00", not "150.000" truncated to something that reparses wrong. */
    void float2string(float value, char * text, VstInt32 maxLen);
    void int2string(VstInt32 value, char * text, VstInt32 maxLen);
    void dB2string(float value, char * text, VstInt32 maxLen);
    void Hz2string(float samples, char * text, VstInt32 maxLen);
    void ms2string(float samples, char * text, VstInt32 maxLen);

protected:
    void setFlag(VstInt32 flag, bool state)
    { if (state) cEffect.flags |= flag; else cEffect.flags &= ~flag; }

    audioMasterCallback audioMaster;
    AEffEditor * editor;      //!< not owned; see setEditor()
    AEffect  cEffect;
    float    sampleRate;
    VstInt32 blockSize;
    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 curProgram;
};

/* ---------------------------------------------------------------------------
 *  AudioEffectX - the 2.x additions
 * ------------------------------------------------------------------------ */

class AudioEffectX : public AudioEffect
{
public:
    AudioEffectX(audioMasterCallback audioMaster, VstInt32 numPrograms, VstInt32 numParams);

    virtual VstIntPtr dispatcher(VstInt32 opcode, VstInt32 index, VstIntPtr value,
                                 void * ptr, float opt);

    void canDoubleReplacing(bool state = true) { setFlag(effFlagsCanDoubleReplacing, state); }

    /*  Tell the host the latency or the channel counts changed under it. This
     *  is the only callback anything in this tree makes, and it is why the
     *  audioMaster pointer is kept at all. Returns false if the host said no,
     *  or if there is no host - which is the case in the unit tests. */
    bool ioChanged();

    virtual bool getEffectName(char * name)    { (void)name; return false; }
    virtual bool getVendorString(char * text)  { (void)text; return false; }
    virtual bool getProductString(char * text) { (void)text; return false; }

    /*  Answered to effGetVendorVersion. 0 is "did not say"; the plug-ins here
     *  return their kVersion, so it agrees with setVersion() - see there. */
    virtual VstInt32 getVendorVersion() { return 0; }
    virtual VstPlugCategory getPlugCategory();

    virtual VstIntPtr vendorSpecific(VstInt32 index, VstIntPtr value, void * ptr, float opt)
    { (void)index; (void)value; (void)ptr; (void)opt; return 0; }

    /*  1 yes, -1 no, 0 do not know. The stock answer is "do not know", which is
     *  why the plug-ins keep a _canDo set and consult it. */
    virtual VstInt32 canDo(char * text) { (void)text; return 0; }

    /*  Samples of output still to come after the input stops. 0 means "no
     *  tail"; 1 means "no tail, and I mean it" to hosts that read 0 as
     *  unanswered. Declick returns its latency here so an offline bounce
     *  collects the last block. */
    virtual VstInt32 getGetTailSize() { return 0; }

    virtual bool canParameterBeAutomated(VstInt32 index) { (void)index; return true; }
    virtual bool string2parameter(VstInt32 index, char * text) { (void)index; (void)text; return false; }
    virtual bool getProgramNameIndexed(VstInt32 category, VstInt32 index, char * text)
    { (void)category; (void)index; if (text) text[0] = 0; return false; }

    virtual VstInt32 getVstVersion() { return 2400; }

    virtual VstInt32 processEvents(VstEvents * events) { (void)events; return 0; }

    virtual bool setBypass(bool onOff) { (void)onOff; return false; }
    virtual VstInt32 startProcess() { return 0; }
    virtual VstInt32 stopProcess() { return 0; }

    /*  Both precisions are genuinely supported here - see the header comment. */
    virtual bool setProcessPrecision(VstInt32 precision)
    { return precision == kVstProcessPrecision32 || precision == kVstProcessPrecision64; }

    virtual VstInt32 getNumMidiInputChannels()  { return 0; }
    virtual VstInt32 getNumMidiOutputChannels() { return 0; }
};

/*  Defined by each plug-in's own .cpp, called once by VSTPluginMain. */
extern AudioEffect * createEffectInstance(audioMasterCallback audioMaster);

#endif /* __audioeffectx__ */
