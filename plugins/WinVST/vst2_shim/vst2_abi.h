/* ========================================
 *  vst2_shim/vst2_abi.h
 *
 *  The VST 2.4 plug-in ABI: the AEffect structure a plug-in hands back to its
 *  host, and the opcode numbers they talk over. Written from the published
 *  description of the interface, MIT licensed with the rest of this tree.
 *
 *  This is NOT Steinberg's aeffect.h / aeffectx.h. Steinberg's vst2.x sources
 *  are not redistributable and are not in this repository - see
 *  plugins/AirwindowsWinVSTTemplate.txt, which says so and says you are on your
 *  own about it. So this file exists to make that survivable: with it, the
 *  WinVST plug-ins in this tree build into loadable DLLs on a machine that has
 *  never seen the SDK. JUCE, Ardour and LMMS all reached the same conclusion
 *  and did the same thing.
 *
 *  ---------------------------------------------------------------------------
 *  Why this file is mostly comments and assertions
 *
 *  An ABI is not an interface you get to design. Every byte offset in AEffect
 *  and every integer in the opcode enums is fixed by the hosts already in the
 *  world, and a shim that gets one of them wrong does not fail to compile - it
 *  loads, and then the host reads a function pointer out of the middle of an
 *  integer field and jumps to it. So the layout is not merely written down
 *  here, it is asserted:
 *
 *      - every field's offset is pinned with a static_assert, so a stray
 *        pragma, a changed type or an accidental reorder is a compile error;
 *      - sizeof(AEffect) is pinned to the two documented values, 144 bytes on
 *        32 bit and 192 on 64 bit, which is the one number an outsider can
 *        check this file against without reading it;
 *      - the opcode enums list every value the SDK declares in its own order,
 *        deprecated entries included, because the deprecated ones are what
 *        space the live ones correctly. Removing effGetVu because nothing uses
 *        it would silently move all ten opcodes after it. Spot checks on the
 *        ones the plug-ins actually depend on are asserted at the bottom.
 *
 *  What the assertions cannot do is prove that 144 and 192 and effGetChunk==23
 *  are themselves right; for that, see tests/vst_host_verify.cpp, which loads
 *  a finished plug-in and drives it through this ABI and nothing else, and
 *  ultimately a real host. They do stop the numbers from drifting once set.
 * ======================================== */

#ifndef __vst2_abi__
#define __vst2_abi__

#include <stddef.h>
#include <stdint.h>

/*  VstInt32 is the SDK's plain int; VstIntPtr is pointer sized, which is what
 *  makes AEffect a different size on the two architectures. Using the stdint
 *  spellings rather than the SDK's #if ladder gets the same types with none of
 *  the platform guessing. */
typedef int16_t  VstInt16;
typedef int32_t  VstInt32;
typedef int64_t  VstInt64;
typedef intptr_t VstIntPtr;

/*  Empty in the SDK too: the AEffect function pointers use the compiler's
 *  default convention, which on Windows means __cdecl. It is named rather than
 *  omitted because that default is a load-bearing part of the ABI - building
 *  a plug-in with /Gz or /Gr would produce a DLL that corrupts the stack of
 *  every host that calls it, and the name is where to hang that warning. */
#define VSTCALLBACK

/*  Four character codes, most significant byte first: 'VstP', 'dhum', 'dclk'. */
#define VST_CCONST(a, b, c, d) \
    ((((VstInt32)(a)) << 24) | (((VstInt32)(b)) << 16) | \
     (((VstInt32)(c)) <<  8) | (((VstInt32)(d)) <<  0))

struct AEffect;

/*  The host's side of the conversation. A plug-in gets one of these handed to
 *  it and can call back into the host with it - see kAudioMaster* below. */
typedef VstIntPtr (VSTCALLBACK *audioMasterCallback)
    (AEffect * effect, VstInt32 opcode, VstInt32 index, VstIntPtr value,
     void * ptr, float opt);

typedef VstIntPtr (VSTCALLBACK *AEffectDispatcherProc)
    (AEffect * effect, VstInt32 opcode, VstInt32 index, VstIntPtr value,
     void * ptr, float opt);
typedef void  (VSTCALLBACK *AEffectProcessProc)
    (AEffect * effect, float ** inputs, float ** outputs, VstInt32 sampleFrames);
typedef void  (VSTCALLBACK *AEffectProcessDoubleProc)
    (AEffect * effect, double ** inputs, double ** outputs, VstInt32 sampleFrames);
typedef void  (VSTCALLBACK *AEffectSetParameterProc)
    (AEffect * effect, VstInt32 index, float parameter);
typedef float (VSTCALLBACK *AEffectGetParameterProc)
    (AEffect * effect, VstInt32 index);

enum {
    kEffectMagic = VST_CCONST('V', 's', 't', 'P')
};

/*  8 byte packing, as the SDK sets around these declarations. Everything here
 *  aligns naturally to 8 or less, so this changes nothing on a default build -
 *  its job is to stop a project-wide /Zp4 from quietly repacking the struct
 *  underneath us. */
#if defined(_MSC_VER)
    #pragma pack(push)
    #pragma pack(8)
#else
    #pragma pack(push, 8)
#endif

/*  The whole plug-in, as far as the host is concerned: five function pointers,
 *  some counts, and 56 reserved bytes. Offsets in the comments are 32 bit / 64
 *  bit and are asserted below. */
struct AEffect
{
    VstInt32 magic;                 /*   0 /   0  must be kEffectMagic          */
    AEffectDispatcherProc dispatcher; /* 4 /   8  everything that is not audio   */

    /*  VST 1.0's accumulating process. Deprecated since 2.4 and unused here,
     *  but it stays a live field and gets a real no-op function rather than a
     *  null, because a host old enough to call it is old enough not to check. */
    AEffectProcessProc process;     /*   8 /  16                                */

    AEffectSetParameterProc setParameter; /* 12 / 24                            */
    AEffectGetParameterProc getParameter; /* 16 / 32                            */

    VstInt32 numPrograms;           /*  20 /  40                                */
    VstInt32 numParams;             /*  24 /  44  per program                   */
    VstInt32 numInputs;             /*  28 /  48                                */
    VstInt32 numOutputs;            /*  32 /  52                                */

    VstInt32 flags;                 /*  36 /  56  effFlags*                     */

    VstIntPtr resvd1;               /*  40 /  64  the host's; leave zero        */
    VstIntPtr resvd2;               /*  44 /  72  the host's; leave zero        */

    /*  Latency in samples. The host reads this field directly rather than
     *  asking, which is why changing it mid-session also needs a nudge - see
     *  audioMasterIOChanged. */
    VstInt32 initialDelay;          /*  48 /  80                                */

    VstInt32 realQualities;         /*  52 /  84  deprecated, unused            */
    VstInt32 offQualities;          /*  56 /  88  deprecated, unused            */
    float    ioRatio;               /*  60 /  92  deprecated, unused            */

    void *   object;                /*  64 /  96  our AudioEffect               */
    void *   user;                  /*  68 / 104  the host's to use             */

    VstInt32 uniqueID;              /*  72 / 112  four character code           */
    VstInt32 version;               /*  76 / 116                                */

    AEffectProcessProc       processReplacing;       /*  80 / 120               */
    AEffectProcessDoubleProc processDoubleReplacing; /*  84 / 128               */

    char future[56];                /*  88 / 136  reserved; must be zero        */
};                                  /* 144 / 192                                */

#pragma pack(pop)

/*  --- the layout, pinned ------------------------------------------------- */

#if defined(_WIN64) || defined(_M_X64) || defined(_M_ARM64) || \
    defined(__x86_64__) || defined(__aarch64__) || (defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8)
    #define VST2_ABI_64 1
#else
    #define VST2_ABI_64 0
#endif

#if VST2_ABI_64
    #define VST2_ABI_OFF(field, off32, off64) \
        static_assert(offsetof(AEffect, field) == (off64), \
                      "AEffect::" #field " is at the wrong offset for a 64 bit VST2 host")
    static_assert(sizeof(AEffect) == 192, "sizeof(AEffect) must be 192 bytes on a 64 bit build");
    static_assert(sizeof(VstIntPtr) == 8, "VstIntPtr must be pointer sized");
#else
    #define VST2_ABI_OFF(field, off32, off64) \
        static_assert(offsetof(AEffect, field) == (off32), \
                      "AEffect::" #field " is at the wrong offset for a 32 bit VST2 host")
    static_assert(sizeof(AEffect) == 144, "sizeof(AEffect) must be 144 bytes on a 32 bit build");
    static_assert(sizeof(VstIntPtr) == 4, "VstIntPtr must be pointer sized");
#endif

VST2_ABI_OFF(magic,                    0,   0);
VST2_ABI_OFF(dispatcher,               4,   8);
VST2_ABI_OFF(process,                  8,  16);
VST2_ABI_OFF(setParameter,            12,  24);
VST2_ABI_OFF(getParameter,            16,  32);
VST2_ABI_OFF(numPrograms,             20,  40);
VST2_ABI_OFF(numParams,               24,  44);
VST2_ABI_OFF(numInputs,               28,  48);
VST2_ABI_OFF(numOutputs,              32,  52);
VST2_ABI_OFF(flags,                   36,  56);
VST2_ABI_OFF(resvd1,                  40,  64);
VST2_ABI_OFF(resvd2,                  44,  72);
VST2_ABI_OFF(initialDelay,            48,  80);
VST2_ABI_OFF(realQualities,           52,  84);
VST2_ABI_OFF(offQualities,            56,  88);
VST2_ABI_OFF(ioRatio,                 60,  92);
VST2_ABI_OFF(object,                  64,  96);
VST2_ABI_OFF(user,                    68, 104);
VST2_ABI_OFF(uniqueID,                72, 112);
VST2_ABI_OFF(version,                 76, 116);
VST2_ABI_OFF(processReplacing,        80, 120);
VST2_ABI_OFF(processDoubleReplacing,  84, 128);
VST2_ABI_OFF(future,                  88, 136);

/*  --- flags -------------------------------------------------------------- */

enum VstAEffectFlags {
    effFlagsHasEditor          = 1 << 0,
    effFlagsCanReplacing       = 1 << 4,   /* processReplacing is implemented  */
    effFlagsProgramChunks      = 1 << 5,   /* presets are opaque blobs         */
    effFlagsIsSynth            = 1 << 8,
    effFlagsNoSoundInStop      = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12   /* processDoubleReplacing too       */
};

/*  --- opcodes, plug-in side ----------------------------------------------
 *
 *  Order is the SDK's order and the deprecated entries are load bearing: they
 *  hold the numbering apart. Do not tidy them away. */

enum AEffectOpcodes {
    effOpen = 0,                /*  0                                          */
    effClose,                   /*  1  and the plug-in is deleted              */

    effSetProgram,              /*  2  [value]                                 */
    effGetProgram,              /*  3  -> program                              */
    effSetProgramName,          /*  4  [ptr] char*                             */
    effGetProgramName,          /*  5  [ptr] char*                             */

    effGetParamLabel,           /*  6  [index] [ptr] char*                     */
    effGetParamDisplay,         /*  7  [index] [ptr] char*                     */
    effGetParamName,            /*  8  [index] [ptr] char*                     */

    effGetVuDeprecated,         /*  9  deprecated; holds the numbering         */

    effSetSampleRate,           /* 10  [opt]                                   */
    effSetBlockSize,            /* 11  [value]                                 */
    effMainsChanged,            /* 12  [value] 0 suspend, 1 resume             */

    effEditGetRect,             /* 13                                          */
    effEditOpen,                /* 14                                          */
    effEditClose,               /* 15                                          */

    effEditDrawDeprecated,      /* 16  deprecated                              */
    effEditMouseDeprecated,     /* 17  deprecated                              */
    effEditKeyDeprecated,       /* 18  deprecated                              */

    effEditIdle,                /* 19                                          */

    effEditTopDeprecated,       /* 20  deprecated                              */
    effEditSleepDeprecated,     /* 21  deprecated                              */
    effIdentifyDeprecated,      /* 22  deprecated                              */

    effGetChunk,                /* 23  [ptr] void**  [index] 0 bank 1 program  */
    effSetChunk,                /* 24  [ptr] [value] size [index] as above     */

    effNumOpcodes               /* 25                                          */
};

enum AEffectXOpcodes {
    effProcessEvents = effSetChunk + 1,   /* 25                                */

    effCanBeAutomated,                    /* 26                                */
    effString2Parameter,                  /* 27                                */

    effGetNumProgramCategoriesDeprecated, /* 28  deprecated                    */

    effGetProgramNameIndexed,             /* 29                                */

    effCopyProgramDeprecated,             /* 30  deprecated                    */
    effConnectInputDeprecated,            /* 31  deprecated                    */
    effConnectOutputDeprecated,           /* 32  deprecated                    */

    effGetInputProperties,                /* 33                                */
    effGetOutputProperties,               /* 34                                */
    effGetPlugCategory,                   /* 35                                */

    effGetCurrentPositionDeprecated,      /* 36  deprecated                    */
    effGetDestinationBufferDeprecated,    /* 37  deprecated                    */

    effOfflineNotify,                     /* 38                                */
    effOfflinePrepare,                    /* 39                                */
    effOfflineRun,                        /* 40                                */

    effProcessVarIo,                      /* 41                                */
    effSetSpeakerArrangement,             /* 42                                */

    effSetBlockSizeAndSampleRateDeprecated, /* 43  deprecated                  */

    effSetBypass,                         /* 44                                */
    effGetEffectName,                     /* 45                                */

    effGetErrorTextDeprecated,            /* 46  deprecated                    */

    effGetVendorString,                   /* 47                                */
    effGetProductString,                  /* 48                                */
    effGetVendorVersion,                  /* 49                                */
    effVendorSpecific,                    /* 50                                */
    effCanDo,                             /* 51                                */
    effGetTailSize,                       /* 52                                */

    effIdleDeprecated,                    /* 53  deprecated                    */
    effGetIconDeprecated,                 /* 54  deprecated                    */
    effSetViewPositionDeprecated,         /* 55  deprecated                    */

    effGetParameterProperties,            /* 56                                */

    effKeysRequiredDeprecated,            /* 57  deprecated                    */

    effGetVstVersion,                     /* 58                                */

    effEditKeyDown,                       /* 59                                */
    effEditKeyUp,                         /* 60                                */
    effSetEditKnobMode,                   /* 61                                */

    effGetMidiProgramName,                /* 62                                */
    effGetCurrentMidiProgram,             /* 63                                */
    effGetMidiProgramCategory,            /* 64                                */
    effHasMidiProgramsChanged,            /* 65                                */
    effGetMidiKeyName,                    /* 66                                */

    effBeginSetProgram,                   /* 67                                */
    effEndSetProgram,                     /* 68                                */

    effGetSpeakerArrangement,             /* 69                                */
    effShellGetNextPlugin,                /* 70                                */

    effStartProcess,                      /* 71                                */
    effStopProcess,                       /* 72                                */
    effSetTotalSampleToProcess,           /* 73                                */
    effSetPanLaw,                         /* 74                                */

    effBeginLoadBank,                     /* 75                                */
    effBeginLoadProgram,                  /* 76                                */

    effSetProcessPrecision,               /* 77                                */
    effGetNumMidiInputChannels,           /* 78                                */
    effGetNumMidiOutputChannels,          /* 79                                */

    effNumOpcodesX                        /* 80                                */
};

/*  Spot checks. Every one of these is a number a host has compiled into it, so
 *  if the ordering above ever gets edited these are what notice. */
static_assert(effGetParamName        ==  8, "effGetParamName must be 8");
static_assert(effSetSampleRate       == 10, "effSetSampleRate must be 10");
static_assert(effMainsChanged        == 12, "effMainsChanged must be 12");
static_assert(effGetChunk            == 23, "effGetChunk must be 23");
static_assert(effSetChunk            == 24, "effSetChunk must be 24");
static_assert(effProcessEvents       == 25, "effProcessEvents must be 25");
static_assert(effCanBeAutomated      == 26, "effCanBeAutomated must be 26");
static_assert(effGetPlugCategory     == 35, "effGetPlugCategory must be 35");
static_assert(effGetEffectName       == 45, "effGetEffectName must be 45");
static_assert(effGetVendorString     == 47, "effGetVendorString must be 47");
static_assert(effGetVendorVersion    == 49, "effGetVendorVersion must be 49");
static_assert(effCanDo               == 51, "effCanDo must be 51");
static_assert(effGetTailSize         == 52, "effGetTailSize must be 52");
static_assert(effGetVstVersion       == 58, "effGetVstVersion must be 58");
static_assert(effStartProcess        == 71, "effStartProcess must be 71");
static_assert(effSetProcessPrecision == 77, "effSetProcessPrecision must be 77");
static_assert(effGetNumMidiOutputChannels == 79, "effGetNumMidiOutputChannels must be 79");

/*  --- opcodes, host side -------------------------------------------------
 *
 *  Only the handful a plug-in without an editor ever calls. The full list is
 *  long and none of the rest is reachable from this tree, so listing them
 *  would be inventing numbers nothing checks. */
enum {
    audioMasterAutomate     =  0,   /* [index] [opt]  a control moved itself   */
    audioMasterVersion      =  1,   /* -> 2400ish, 0 from a pre-VST2 host      */
    audioMasterCurrentId    =  2,
    audioMasterIdle         =  3,
    audioMasterIOChanged    = 13,   /* re-read initialDelay and the i/o counts */
    audioMasterGetSampleRate = 16,
    audioMasterGetBlockSize  = 17
};

/*  --- odds and ends ------------------------------------------------------ */

enum {
    kVstMaxProgNameLen   = 24,
    kVstMaxParamStrLen   = 8,
    kVstMaxVendorStrLen  = 64,
    kVstMaxProductStrLen = 64,
    kVstMaxEffectNameLen = 32
};

enum VstProcessPrecision {
    kVstProcessPrecision32 = 0,
    kVstProcessPrecision64 = 1
};

enum VstPlugCategory {
    kPlugCategUnknown = 0,
    kPlugCategEffect,
    kPlugCategSynth,
    kPlugCategAnalysis,
    kPlugCategMastering,
    kPlugCategSpacializer,
    kPlugCategRoomFx,
    kPlugSurroundFx,
    kPlugCategRestoration,
    kPlugCategOfflineProcess,
    kPlugCategShell,
    kPlugCategGenerator,
    kPlugCategMaxCount
};

/*  The editor's size, in pixels. Four 16 bit fields and the order is
 *  top, left, bottom, right - not the left, top, right, bottom a Win32 RECT
 *  uses, which is a transposition waiting to happen and is why ParaEQEditor
 *  converts between the two in one place.
 *
 *  A host receives a pointer to one of these from effEditGetRect and reads it
 *  immediately; it never frees it and never writes to it, so the plug-in keeps
 *  the storage and hands out its address. Eight bytes with no padding on either
 *  architecture, which is what lets a 32 bit host read a struct a 32 bit
 *  plug-in wrote without either side negotiating. */
struct ERect {
    VstInt16 top;
    VstInt16 left;
    VstInt16 bottom;
    VstInt16 right;
};

/*  Declared, never defined: nothing in this tree processes MIDI or overrides
 *  the pin properties, so the dispatcher answers "not supported" for both and
 *  no layout has to be guessed at. A plug-in that needs them needs a real
 *  definition here first. */
struct VstEvents;
struct VstPinProperties;

#endif /* __vst2_abi__ */
