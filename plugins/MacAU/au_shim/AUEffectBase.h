/* ========================================
 *  au_shim - AUEffectBase.h
 *
 *  Enough of Apple's Audio Unit base classes to compile the plug-ins in this
 *  folder and drive them from a test, and nothing else. MIT licensed, like the
 *  rest of the tree. See README.md next to this file for what it is not.
 * ======================================== */

#ifndef AU_SHIM_AUEFFECTBASE_H
#define AU_SHIM_AUEFFECTBASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

//! CoreAudio spellings of the scalar types. These are the sizes the real
//! headers give them; nothing here depends on that, but the plug-in sources are
//! written against these names and are not going to be edited for the shim.
typedef int32_t  OSStatus;
typedef OSStatus ComponentResult;
typedef uint8_t  Boolean;
typedef int16_t  SInt16;
typedef int32_t  SInt32;
typedef uint32_t UInt32;
typedef float    Float32;
typedef double   Float64;

enum { noErr = 0 };

typedef void *   AudioUnit;
typedef uint32_t AudioUnitScope;
typedef uint32_t AudioUnitElement;
typedef uint32_t AudioUnitParameterID;
typedef uint32_t AudioUnitPropertyID;
typedef uint32_t AudioUnitRenderActionFlags;
typedef float    AudioUnitParameterValue;
typedef uint32_t AudioUnitParameterUnit;

//! The AU parameter names are CFStrings in the real SDK. Nothing in this tree
//! does anything with them but hand them straight back to the host, so here
//! they are plain C strings and CFSTR is the identity - which is also what lets
//! the tests build somewhere that has no CoreFoundation.
typedef const char * CFStringRef;
typedef const void * CFArrayRef;
#define CFSTR(s) (s)

//! A plug-in with an indexed parameter hands the host an array of names for it.
//! With C strings for the names there is nothing to allocate and nothing to
//! retain, so both of these are the identity - which is all the tests ask of
//! them: that the plug-in offers strings for the parameters that should have
//! them and refuses for the ones that should not.
inline CFArrayRef CFArrayCreate(const void * /*allocator*/, const void ** values,
                                long /*count*/, const void * /*callBacks*/)
{ return (CFArrayRef)values; }

inline CFStringRef CFRetain(CFStringRef s) { return s; }

static const OSStatus kAudioUnitErr_InvalidProperty      = -10879;
static const OSStatus kAudioUnitErr_InvalidParameter     = -10878;
static const OSStatus kAudioUnitErr_InvalidPropertyValue = -10851;

static const AudioUnitScope kAudioUnitScope_Global = 0;

static const AudioUnitPropertyID kAudioUnitProperty_Latency = 12;

static const AudioUnitParameterUnit kAudioUnitParameterUnit_Generic  = 0;
static const AudioUnitParameterUnit kAudioUnitParameterUnit_Indexed  = 1;
static const AudioUnitParameterUnit kAudioUnitParameterUnit_Boolean  = 2;
static const AudioUnitParameterUnit kAudioUnitParameterUnit_Hertz    = 8;
static const AudioUnitParameterUnit kAudioUnitParameterUnit_Decibels = 13;

static const UInt32 kAudioUnitParameterFlag_HasClump           = (1u << 20);
static const UInt32 kAudioUnitParameterFlag_DisplayLogarithmic = (1u << 22);
static const UInt32 kAudioUnitParameterFlag_HasCFNameString    = (1u << 27);
static const UInt32 kAudioUnitParameterFlag_IsReadable         = (1u << 30);
static const UInt32 kAudioUnitParameterFlag_IsWritable         = (1u << 31);

static const AudioUnitRenderActionFlags kAudioUnitRenderAction_OutputIsSilence = (1u << 4);

//! Real AudioBufferList ends in a flexible mBuffers[1] that callers
//! over-allocate. Nothing in this folder goes past stereo - Declick and Dehum
//! say 2 in 2 out and nothing else, ParaEQ adds mono - so two is the whole
//! story and a plain array keeps the test's stack frames honest.
struct AudioBuffer {
    UInt32 mNumberChannels;
    UInt32 mDataByteSize;
    void * mData;
};

struct AudioBufferList {
    UInt32 mNumberBuffers;
    AudioBuffer mBuffers[2];
};

struct AUChannelInfo {
    SInt16 inChannels;
    SInt16 outChannels;
};

struct AudioUnitParameterInfo {
    char                   name[52];
    CFStringRef            unitName;
    UInt32                 clumpID;
    CFStringRef            cfNameString;
    AudioUnitParameterUnit unit;
    AudioUnitParameterValue minValue;
    AudioUnitParameterValue maxValue;
    AudioUnitParameterValue defaultValue;
    UInt32                 flags;
};

//! What Globals() hands back. The only thing asked of it is how many indexed
//! parameters the plug-in wants.
class AUElement {
public:
    AUElement() : m_indexedParameters(0) {}
    void UseIndexedParameters(UInt32 inCount) { m_indexedParameters = inCount; }
    UInt32 GetIndexedParameterCount() const { return m_indexedParameters; }
private:
    UInt32 m_indexedParameters;
};

enum { kAUShimMaxParameters = 32 };

class AUBase {
public:
    explicit AUBase(AudioUnit /*inInstance*/)
        : m_sampleRate(44100.0), m_propertyChanges(0),
          m_lastPropertyChanged(0), m_globals()
    {
        for (int i = 0; i < kAUShimMaxParameters; ++i) m_parameters[i] = 0.0f;
    }
    virtual ~AUBase() {}

    //! The real one retains the CFString and copies a UTF-8 rendering into
    //! `name`; with C strings for both, that is a strncpy.
    static void FillInParameterName(AudioUnitParameterInfo & outInfo,
                                    CFStringRef inName, bool /*inShouldRelease*/)
    {
        outInfo.cfNameString = inName;
        outInfo.unitName = NULL;
        outInfo.clumpID = 0;
        if (inName) {
            strncpy(outInfo.name, inName, sizeof(outInfo.name) - 1);
            outInfo.name[sizeof(outInfo.name) - 1] = '\0';
        } else {
            outInfo.name[0] = '\0';
        }
        outInfo.flags |= kAudioUnitParameterFlag_HasCFNameString;
    }

    //! A host listens for this and re-reads the property. Counting the calls is
    //! the point of having it here: it is what the AU says instead of the VST's
    //! ioChanged(), and the tests assert on exactly when it happens.
    void PropertyChanged(AudioUnitPropertyID inID, AudioUnitScope /*inScope*/,
                         AudioUnitElement /*inElement*/)
    {
        ++m_propertyChanges;
        m_lastPropertyChanged = inID;
    }

    void CreateElements() {}
    AUElement * Globals() { return &m_globals; }

    void SetParameter(AudioUnitParameterID inID, AudioUnitParameterValue inValue)
    {
        if (inID < (AudioUnitParameterID)kAUShimMaxParameters) m_parameters[inID] = inValue;
    }
    AudioUnitParameterValue GetParameter(AudioUnitParameterID inID)
    {
        return (inID < (AudioUnitParameterID)kAUShimMaxParameters) ? m_parameters[inID] : 0.0f;
    }

    Float64 GetSampleRate() { return m_sampleRate; }

    // ---- shim only, standing in for what a host does through the ABI ----
    void AUShimSetSampleRate(Float64 inRate) { m_sampleRate = inRate; }
    UInt32 AUShimPropertyChangeCount() const { return m_propertyChanges; }
    AudioUnitPropertyID AUShimLastPropertyChanged() const { return m_lastPropertyChanged; }
    void AUShimResetPropertyChangeCount() { m_propertyChanges = 0; m_lastPropertyChanged = 0; }

private:
    Float64 m_sampleRate;
    UInt32 m_propertyChanges;
    AudioUnitPropertyID m_lastPropertyChanged;
    AUElement m_globals;
    AudioUnitParameterValue m_parameters[kAUShimMaxParameters];
};

class AUEffectBase : public AUBase {
public:
    explicit AUEffectBase(AudioUnit inInstance, bool /*inProcessesInPlace*/ = true)
        : AUBase(inInstance) {}

    virtual ComponentResult Initialize() { return noErr; }
    virtual ComponentResult Reset(AudioUnitScope /*inScope*/, AudioUnitElement /*inElement*/)
    { return noErr; }

    virtual OSStatus ProcessBufferLists(AudioUnitRenderActionFlags & /*ioActionFlags*/,
                                        const AudioBufferList & /*inBuffer*/,
                                        AudioBufferList & /*outBuffer*/,
                                        UInt32 /*inFramesToProcess*/)
    { return noErr; }

    virtual UInt32 SupportedNumChannels(const AUChannelInfo ** /*outInfo*/) { return 0; }

    virtual ComponentResult GetParameterValueStrings(AudioUnitScope, AudioUnitParameterID,
                                                    CFArrayRef *)
    { return kAudioUnitErr_InvalidProperty; }

    virtual ComponentResult GetParameterInfo(AudioUnitScope, AudioUnitParameterID,
                                             AudioUnitParameterInfo &)
    { return kAudioUnitErr_InvalidParameter; }

    //! `bool &`, not the `Boolean &` the Xcode 3 headers had: AudioUnitSDK
    //! spells it bool, and the plug-in sources have to override whichever of
    //! the two this folder is built against.
    virtual ComponentResult GetPropertyInfo(AudioUnitPropertyID, AudioUnitScope,
                                            AudioUnitElement, UInt32 &, bool &)
    { return kAudioUnitErr_InvalidProperty; }

    virtual ComponentResult GetProperty(AudioUnitPropertyID, AudioUnitScope,
                                        AudioUnitElement, void *)
    { return kAudioUnitErr_InvalidProperty; }

    //! How a host asks what to call a group of parameters. The real SDK
    //! dispatches kAudioUnitProperty_ParameterClumpName to this before it
    //! consults GetProperty, so a plug-in that groups its controls - ParaEQ -
    //! answers here.
    virtual ComponentResult CopyClumpName(AudioUnitScope, UInt32, UInt32, CFStringRef *)
    { return kAudioUnitErr_InvalidProperty; }

    virtual bool    SupportsTail() { return false; }
    virtual Float64 GetTailTime() { return 0.0; }
    virtual Float64 GetLatency()  { return 0.0; }

    virtual ComponentResult Version() { return 0; }
};

//! Against the real SDK this expands to the factory function a modern host
//! looks up by name from the Info.plist - see ../au_compat/AUEffectBase.h.
//! Here it is a plain entry point, so that a missing or misspelled one is still
//! a link error rather than something nobody notices until a DAW scans.
#define COMPONENT_ENTRY(Class)                          \
    extern "C" void * Class##Entry(void);               \
    extern "C" void * Class##Entry(void) { return new Class((AudioUnit)0); }

#endif // AU_SHIM_AUEFFECTBASE_H
