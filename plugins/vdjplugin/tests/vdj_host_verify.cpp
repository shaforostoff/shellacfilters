/* ========================================
 *  vdj_host_verify
 *
 *  Loads each finished plug-in the way VirtualDJ does - LoadLibrary, one
 *  GetProcAddress, the IID, and the vtable - and requires the audio that comes
 *  out to match the same plug-in linked statically, to the bit.
 *
 *  This is the counterpart of vst_host_verify in ../../foobar2000_dsp/tests, and
 *  it exists for the same reason: declick_vdj_verify and dehum_vdj_verify link
 *  the wrappers directly, so they see the code but not the ABI. Everything
 *  between the two is what this covers, and none of it is testable any other
 *  way:
 *
 *    - that the module exports DllGetClassObject under the name VirtualDJ looks
 *      up, undecorated, which on 32 bit needs vdjplugin.def and on 64 bit does
 *      not,
 *    - that it answers the IID it is meant to answer and declines the others -
 *      a live plug-in must not claim to be a buffer one, because the interface
 *      it would then be called through has a different vtable,
 *    - that the vtable VirtualDJ builds from vdjDsp8.h and the one the plug-in
 *      compiled are the same shape, and that __stdcall on 32 bit agrees at both
 *      ends,
 *    - that OnLoad, OnGetPluginInfo, OnStart, the processing call and Release
 *      all survive being called across a module boundary, with the callbacks
 *      pointing at somebody else's memory.
 *
 *  The comparison is against a statically linked instance driven with identical
 *  input, rather than against a recording: a recording would have to be
 *  regenerated whenever the DSP changed, and would then be checking the DSP
 *  again instead of the ABI.
 * ======================================== */

#include "vdj_test_support.h"

#include "declick_engine.h"
#include "dehum_engine.h"

#include <stdlib.h>

#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int vdjtest::g_failures = 0;

using namespace vdjtest;

namespace {

const double kRate = 44100.0;

typedef HRESULT (VDJ_API * GetClassObjectFn)(const GUID &, const GUID &, void **);

// ---------------------------------------------------------------------------

//! One loaded module, and the one function it exports.
class Module {
public:
    ~Module() { unload(); }

    bool load(const std::string & dir, const char * name) {
#if defined(_WIN32)
        m_path = dir + "\\" + name + ".dll";
        m_handle = LoadLibraryA(m_path.c_str());
        if (m_handle == NULL) {
            printf("  FAIL  LoadLibrary(%s) failed, error %lu\n",
                   m_path.c_str(), (unsigned long)GetLastError());
            ++g_failures;
            return false;
        }
        m_entry = (GetClassObjectFn)GetProcAddress(m_handle, "DllGetClassObject");
#else
        // A .bundle is a directory; dlopen wants the Mach-O inside it. Loading
        // it this way rather than through CFBundle is a deliberate narrowing:
        // what is under test is the export and the vtable, and dlopen is the
        // shortest path to both.
        m_path = dir + "/" + name + ".bundle/Contents/MacOS/" + name;
        m_handle = dlopen(m_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (m_handle == NULL) {
            printf("  FAIL  dlopen(%s) failed: %s\n", m_path.c_str(), dlerror());
            ++g_failures;
            return false;
        }
        m_entry = (GetClassObjectFn)dlsym(m_handle, "DllGetClassObject");
#endif
        if (m_entry == NULL) {
            printf("  FAIL  %s does not export DllGetClassObject\n", m_path.c_str());
            ++g_failures;
            return false;
        }
        return true;
    }

    void unload() {
        if (m_handle == NULL) return;
#if defined(_WIN32)
        FreeLibrary(m_handle);
#else
        dlclose(m_handle);
#endif
        m_handle = NULL;
        m_entry = NULL;
    }

    //! Exactly what VirtualDJ asks. NULL means declined, which for the wrong
    //! IID is the right answer.
    void * create(const GUID & iid) const {
        if (m_entry == NULL) return NULL;
        void * object = NULL;
        const HRESULT hr = m_entry(CLSID_VdjPlugin8, iid, &object);
        if (hr != NO_ERROR) return NULL;
        return object;
    }

    const std::string & path() const { return m_path; }

private:
#if defined(_WIN32)
    HMODULE m_handle = NULL;
#else
    void * m_handle = NULL;
#endif
    GetClassObjectFn m_entry = NULL;
    std::string m_path;
};

//! The IID a plug-in must decline. Getting an object back for the interface a
//! module does not implement would be worse than getting none: VirtualDJ would
//! then call it through the wrong vtable.
const GUID & otherIid(bool buffer) {
    return buffer ? IID_IVdjPluginDsp8 : IID_IVdjPluginBuffer8;
}

void checkIdentity(Module & mod, const char * expectedName, bool buffer) {
    if (mod.create(otherIid(buffer)) != NULL) {
        printf("  FAIL  %s answered the interface it does not implement\n",
               expectedName);
        ++g_failures;
        // Not used and not released: whichever vtable slot Release would land
        // in is not Release. Leaked on purpose, in a run that has already
        // failed.
    } else {
        note("the interface it does not implement is declined");
    }

    GUID nonsense = CLSID_VdjPlugin8;
    nonsense.Data1 ^= 0xffffffffu;
    check(mod.create(nonsense) == NULL, "an unknown class id is declined");
}

// ---------------------------------------------------------------------------

//! Runs a buffer plug-in over one song, forward, in equal blocks.
std::vector<short> runBuffer(IVdjPluginBufferDsp8 * plugin, StubCallbacks & cb,
                             const std::vector<short> & song, int nb) {
    plugin->cb = &cb;
    plugin->hInstance = 0;
    plugin->OnLoad();
    plugin->SampleRate = (int)kRate;
    plugin->SongBpm = (int)(kRate / 2.0);
    plugin->SongPos = 0;
    plugin->SongPosBeats = 0.0;
    plugin->OnStart();

    const size_t frames = song.size() / vdj::kChannels;
    std::vector<short> out(frames * vdj::kChannels, 0);
    for (size_t pos = 0; pos + (size_t)nb <= frames; pos += (size_t)nb) {
        plugin->SongPos = (int)pos;
        const short * got = plugin->OnGetSongBuffer((int)pos, nb);
        if (got == NULL) {
            check(false, "OnGetSongBuffer returned NULL");
            break;
        }
        memcpy(&out[pos * vdj::kChannels], got,
               (size_t)nb * vdj::kChannels * sizeof(short));
    }

    plugin->OnStop();
    return out;
}

// ---------------------------------------------------------------------------

void reportInfo(IVdjPlugin8 * plugin, const char * expectedName,
                const char * expectedVersion) {
    TVdjPluginInfo8 info;
    memset(&info, 0, sizeof(info));
    if (plugin->OnGetPluginInfo(&info) != S_OK) {
        check(false, "OnGetPluginInfo failed");
        return;
    }
    check(info.PluginName != NULL && strcmp(info.PluginName, expectedName) == 0,
          "the module reports the name it is meant to");
    check(info.Author != NULL && info.Author[0] != 0, "it has an author");
    check(info.Description != NULL && info.Description[0] != 0, "it has a description");
    check(info.Version != NULL && strcmp(info.Version, expectedVersion) == 0,
          "it reports its own version, not the package's");
    printf("        \"%s\" %s - %.60s...\n",
           info.PluginName ? info.PluginName : "?",
           info.Version ? info.Version : "?",
           info.Description ? info.Description : "?");
}

//! One buffer plug-in, the same way. This is the more searching of the two: it
//! also exercises the callback going the other way, since the plug-in has to
//! call GetSongBuffer back through the pointer it was handed.
template<class Engine>
void verifyBuffer(const std::string & dir, const char * file, const char * name) {
    printf("  -- %s (buffer) --\n", file);
    Module mod;
    if (!mod.load(dir, file)) return;

    IVdjPluginBufferDsp8 * loaded =
        (IVdjPluginBufferDsp8 *)mod.create(IID_IVdjPluginBuffer8);
    if (loaded == NULL) {
        check(false, "the module did not hand back an IVdjPluginBufferDsp8");
        return;
    }
    checkIdentity(mod, name, true);
    reportInfo(loaded, name, Engine::pluginVersion());

    std::vector<short> song = music((size_t)(6.0 * kRate), kRate, 9090);
    injectClicks(song, kRate, 30.0);
    addTone(song, kRate, 41.3, 0.08);

    StubCallbacks cbLoaded, cbStatic;
    cbLoaded.song = &song;
    cbStatic.song = &song;

    const std::vector<short> got = runBuffer(loaded, cbLoaded, song, 512);
    loaded->Release();
    mod.unload();

    vdj::BufferDsp<Engine> * inproc = new vdj::BufferDsp<Engine>();
    const std::vector<short> want = runBuffer(inproc, cbStatic, song, 512);
    inproc->Release();

    check(cbLoaded.declared.size() == cbStatic.declared.size()
          && cbLoaded.declared.size() == 7,
          "the loaded module declares the same seven sliders");
    check(cbLoaded.songBufferCalls > 0,
          "the loaded module called GetSongBuffer back through the callbacks");
    checkf(cbLoaded.songBufferCalls == cbStatic.songBufferCalls,
           "the loaded module read the song %.0f times, the linked one %.0f",
           (double)cbLoaded.songBufferCalls, (double)cbStatic.songBufferCalls);

    const int64_t at = firstDifference(want, got);
    if (at >= 0) {
        printf("  FAIL  the loaded module diverges at frame %lld\n", (long long)at);
        ++g_failures;
    } else {
        note("through the ABI, bit-identical to the same class linked in");
    }

    // And it did something: an unprocessed buffer would also be identical to an
    // unprocessed buffer.
    check(firstDifference(song, got) >= 0, "the plug-in actually changed the audio");
}

} // anonymous namespace

int main(int argc, char ** argv) {
    printf("vdj_host_verify\n");
    if (argc < 2) {
        printf("usage: %s <directory holding the built plug-ins>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    printf("  modules from %s\n", dir.c_str());

    verifyBuffer<vdj::DeclickEngine>(dir, "Declick", "Declick");
    verifyBuffer<vdj::DehumEngine>(dir, "Dehum", "Dehum");

    return finish("vdj_host_verify");
}
