/* ========================================
 *  component_smoke
 *
 *  Loads the built foo_dsp_decrackle.dll the same way foobar2000 does -
 *  through foobar2000_get_interface() and the service factory list - then
 *  registers the DSP, instantiates it from a preset and pushes audio through
 *  it. Verifies the parts the pure-DSP harness cannot reach: service
 *  registration, preset round-tripping, dsp_v3 live reconfiguration, format
 *  changes and the dialog resource.
 *
 *  foobar2000 must be installed (for shared.dll) and must match this build's
 *  architecture. If it is not, the test reports SKIPPED and succeeds.
 *
 *    component_smoke <path-to-component.dll> [foobar2000-directory]
 *                    [--dialog <id>] [--slider <id>] [--label <id>]
 *                    [--editor <id>]
 *                    [--screenshot <out.bmp>]
 * ======================================== */

#include "stdafx_host.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

// Which resource ids to poke; the two components number theirs differently.
int g_dialogId = 101;
int g_sliderId = 1001;
int g_labelId  = 1011;
//! Non-zero for a component whose dialog is owner-drawn and so has no trackbar
//! to move. The probe types at that control instead - which is a stronger check
//! than moving a slider, since it goes through the same keyboard path a user
//! does rather than through a control the system implements.
int g_editorId = 0;

void check(bool ok, const char * what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++g_failures;
}

std::wstring findFoobarDir(const wchar_t * hint) {
    std::vector<std::wstring> candidates;
    if (hint && *hint) candidates.push_back(hint);
    const wchar_t * envNames[] = { L"FOOBAR2000_DIR", L"ProgramFiles", L"ProgramFiles(x86)" };
    for (size_t i = 0; i < 3; ++i) {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetEnvironmentVariableW(envNames[i], buf, (DWORD)(MAX_PATH * 2));
        if (n == 0 || n >= MAX_PATH * 2) continue;
        std::wstring p(buf);
        if (i != 0) p += L"\\foobar2000";
        candidates.push_back(p);
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        const std::wstring probe = candidates[i] + L"\\shared.dll";
        if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) return candidates[i];
    }
    return std::wstring();
}

//! Minimal stand-in for the host. The DSP path never reaches into it, but the
//! SDK wants a non-null pointer.
class stub_api : public foobar2000_api {
public:
    service_class_ref service_enum_find_class(const GUID &) override { return NULL; }
    bool service_enum_create(service_ptr_t<service_base> &, service_class_ref, t_size) override { return false; }
    t_size service_enum_get_count(service_class_ref) override { return 0; }
    fb2k::hwnd_t get_main_window() override { return NULL; }
    bool assert_main_thread() override { return true; }
    bool is_main_thread() override { return true; }
    bool is_shutting_down() override { return false; }
    const char * get_profile_path() override { return "file://."; }
    bool is_initializing() override { return false; }
    bool is_portable_mode_enabled() override { return true; }
    bool is_quiet_mode_enabled() override { return true; }
};

typedef foobar2000_client * (_cdecl * get_interface_t)(foobar2000_api *, HINSTANCE);

//! Fills a chunk with a quiet tone plus periodic clicks.
void fillChunk(audio_chunk & chunk, unsigned channels, unsigned rate,
               size_t frames, size_t phase) {
    chunk.set_sample_rate(rate);
    chunk.set_channels(channels, audio_chunk::g_guess_channel_config(channels));
    chunk.set_data_size(frames * channels);
    chunk.set_sample_count(frames);
    audio_sample * p = chunk.get_data();
    for (size_t n = 0; n < frames; ++n) {
        const size_t t = phase + n;
        double v = 0.2 * sin(6.2831853 * 440.0 * (double)t / (double)rate);
        if ((t % 1500) == 0) v += 0.75;
        for (unsigned c = 0; c < channels; ++c) {
            p[n * channels + c] = (audio_sample)(v * (c == 1 ? -1.0 : 1.0));
        }
    }
}

bool chunkIsFinite(const audio_chunk & chunk) {
    const audio_sample * p = chunk.get_data();
    const t_size n = chunk.get_used_size();
    for (t_size i = 0; i < n; ++i) {
        const double v = (double)p[i];
        if (!(v > -1.0e30 && v < 1.0e30)) return false;
    }
    return true;
}

double chunkPeak(const audio_chunk & chunk) {
    const audio_sample * p = chunk.get_data();
    const t_size n = chunk.get_used_size();
    double peak = 0.0;
    for (t_size i = 0; i < n; ++i) {
        const double v = fabs((double)p[i]);
        if (v > peak) peak = v;
    }
    return peak;
}

//! Runs `count` chunks of the given format through the DSP.
bool pump(dsp::ptr d, unsigned channels, unsigned rate, size_t frames, size_t count,
          double * outPeak) {
    double peak = 0.0;
    for (size_t i = 0; i < count; ++i) {
        dsp_chunk_list_impl list;
        audio_chunk * c = list.add_item(frames * channels);
        fillChunk(*c, channels, rate, frames, i * frames);
        d->run_abortable(&list, dsp_track_t(), 0, fb2k::noAbort);
        for (t_size k = 0; k < list.get_count(); ++k) {
            if (!chunkIsFinite(*list.get_item(k))) return false;
            const double p = chunkPeak(*list.get_item(k));
            if (p > peak) peak = p;
        }
    }
    if (outPeak) *outPeak = peak;
    return true;
}

// -- configuration dialog ----------------------------------------------------

//! Records what the dialog pushes back at the host.
class recording_callback : public dsp_preset_edit_callback {
public:
    void on_preset_changed(const dsp_preset & p) override {
        m_last = p;
        ++m_count;
    }
    dsp_preset_impl m_last;
    int             m_count = 0;
};

struct probe_args {
    DWORD mainThread;
    bool  accept;       // true: press OK, false: press Cancel
    bool  foundDialog;
    bool  foundSlider;
    bool  labelUpdated;
    bool  foundEditor;
    int   sliderPos;
    const wchar_t * screenshot;   // optional .bmp to write, for eyeballing the layout
};

//! Dumps a window to a 24 bit BMP. Only used by --screenshot.
void saveWindowBmp(HWND wnd, const wchar_t * path) {
    RECT rc;
    if (!GetWindowRect(wnd, &rc)) return;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    const HDC screen = GetDC(NULL);
    const HDC mem = CreateCompatibleDC(screen);
    const HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    const HGDIOBJ prev = SelectObject(mem, bmp);
    PrintWindow(wnd, mem, 0);

    BITMAPINFOHEADER bi = {};
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;           // top-down
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;

    const int stride = ((w * 3) + 3) & ~3;
    std::vector<unsigned char> pixels((size_t)stride * (size_t)h);
    GetDIBits(mem, bmp, 0, h, &pixels[0], (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    SelectObject(mem, prev);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);

    BITMAPFILEHEADER fh = {};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + (DWORD)pixels.size();
    bi.biHeight = h;            // bottom-up for the file
    // Flip the rows now that the header says bottom-up.
    std::vector<unsigned char> flipped(pixels.size());
    for (int y = 0; y < h; ++y) {
        memcpy(&flipped[(size_t)(h - 1 - y) * stride], &pixels[(size_t)y * stride], stride);
    }

    FILE * f = NULL;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == NULL) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi, sizeof(bi), 1, f);
    fwrite(&flipped[0], 1, flipped.size(), f);
    fclose(f);
}

BOOL CALLBACK findDialogProc(HWND wnd, LPARAM lp) {
    wchar_t cls[64];
    if (GetClassNameW(wnd, cls, 64) == 0) return TRUE;
    if (wcscmp(cls, L"#32770") != 0) return TRUE;      // the dialog window class
    if (!IsWindowVisible(wnd)) return TRUE;
    *(HWND *)lp = wnd;
    return FALSE;
}

DWORD WINAPI dialogProbe(LPVOID param) {
    probe_args * a = (probe_args *)param;

    HWND dlg = NULL;
    for (int i = 0; i < 200 && dlg == NULL; ++i) {   // up to 10 s
        EnumThreadWindows(a->mainThread, findDialogProc, (LPARAM)&dlg);
        if (dlg == NULL) Sleep(50);
    }
    if (dlg == NULL) return 0;
    a->foundDialog = true;

    if (g_editorId != 0) {
        const HWND editor = GetDlgItem(dlg, g_editorId);
        if (editor != NULL) {
            a->foundEditor = true;
            // SendMessage rather than SetFocus: this runs on a different thread
            // from the dialog, where SetFocus does nothing, while SendMessage
            // marshals across and is handled on the dialog's own thread exactly
            // as a real key press would be.
            SendMessageW(editor, WM_KEYDOWN, VK_UP, 0);
            SendMessageW(editor, WM_KEYUP,   VK_UP, 0);
        }
    }

    const HWND slider = GetDlgItem(dlg, g_sliderId);
    if (g_editorId == 0 && slider != NULL) {
        a->foundSlider = true;
        // Drive the control the way the user would, then tell the dialog about
        // it exactly as the trackbar does.
        SendMessageW(slider, TBM_SETPOS, TRUE, (LPARAM)a->sliderPos);
        SendMessageW(dlg, WM_HSCROLL,
                     MAKEWPARAM(SB_THUMBPOSITION, a->sliderPos), (LPARAM)slider);

        wchar_t label[64] = { 0 };
        GetDlgItemTextW(dlg, g_labelId, label, 64);
        wchar_t expected[64];
        _snwprintf_s(expected, 64, _TRUNCATE, L"%.3f",
                     (double)a->sliderPos / 1000.0);
        a->labelUpdated = (wcscmp(label, expected) == 0);
    }

    if (a->screenshot != NULL) {
        Sleep(250);   // let the theme engine finish painting
        saveWindowBmp(dlg, a->screenshot);
    }

    const int id = a->accept ? IDOK : IDCANCEL;
    PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)GetDlgItem(dlg, id));
    return 0;
}

//! Opens the config dialog, pokes a slider, then presses OK or Cancel.
bool driveDialog(dsp_entry::ptr entryService, const dsp_preset & start,
                 bool accept, int sliderPos, recording_callback & cb,
                 probe_args & probe, const wchar_t * screenshot = NULL) {
    dsp_entry_v2::ptr v2;
    if (!(v2 &= entryService)) return false;

    probe.mainThread   = GetCurrentThreadId();
    probe.accept       = accept;
    probe.foundDialog  = false;
    probe.foundSlider  = false;
    probe.labelUpdated = false;
    probe.sliderPos    = sliderPos;
    probe.screenshot   = screenshot;

    const HANDLE th = CreateThread(NULL, 0, dialogProbe, &probe, 0, NULL);
    if (th == NULL) return false;

    v2->show_config_popup_v2(start, NULL, cb);   // blocks until the probe closes it

    WaitForSingleObject(th, 15000);
    CloseHandle(th);
    return true;
}

} // anonymous namespace

int wmain(int argc, wchar_t ** argv) {
    if (argc < 2) {
        std::printf("usage: component_smoke <foo_dsp_decrackle.dll> [foobar2000-dir]\n");
        return 2;
    }
    const std::wstring dllPath = argv[1];
    std::wstring fb2kHint, screenshot;
    for (int i = 2; i < argc; ++i) {
        if (wcscmp(argv[i], L"--screenshot") == 0 && i + 1 < argc) {
            screenshot = argv[++i];
        } else if (wcscmp(argv[i], L"--dialog") == 0 && i + 1 < argc) {
            g_dialogId = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--slider") == 0 && i + 1 < argc) {
            g_sliderId = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--label") == 0 && i + 1 < argc) {
            g_labelId = _wtoi(argv[++i]);
        } else if (wcscmp(argv[i], L"--editor") == 0 && i + 1 < argc) {
            g_editorId = _wtoi(argv[++i]);
        } else if (fb2kHint.empty()) {
            fb2kHint = argv[i];
        }
    }
    const std::wstring fb2kDir = findFoobarDir(fb2kHint.empty() ? NULL : fb2kHint.c_str());

    if (fb2kDir.empty()) {
        std::printf("SKIPPED: no foobar2000 installation found (shared.dll is needed).\n");
        return 0;
    }
    std::printf("foobar2000 directory: %ls\n", fb2kDir.c_str());

    SetDllDirectoryW(fb2kDir.c_str());
    const HMODULE mod = LoadLibraryExW(dllPath.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (mod == NULL) {
        const DWORD err = GetLastError();
        if (err == ERROR_BAD_EXE_FORMAT) {
            std::printf("SKIPPED: %ls does not match the installed foobar2000's architecture.\n",
                        dllPath.c_str());
            return 0;
        }
        std::printf("FAILED: LoadLibrary(%ls) -> error %lu\n", dllPath.c_str(), (unsigned long)err);
        return 1;
    }
    std::printf("loaded %ls\n\n", dllPath.c_str());

    static stub_api api;
    // Point this process's own copy of the SDK at the stub too.
    g_foobar2000_api = &api;

    const get_interface_t entry =
        (get_interface_t)GetProcAddress(mod, "foobar2000_get_interface");
    check(entry != NULL, "exports foobar2000_get_interface");
    if (entry == NULL) return 1;

    foobar2000_client * client = entry(&api, mod);
    check(client != NULL, "foobar2000_get_interface returns a client");
    if (client == NULL) return 1;

    check(client->get_version() == FOOBAR2000_TARGET_VERSION,
          "client reports the expected API level (80 = fb2k 1.5+)");

    check(FindResourceW(mod, MAKEINTRESOURCEW(g_dialogId), (LPCWSTR)RT_DIALOG) != NULL,
          "configuration dialog resource is present");

    // --- find our dsp_entry in the factory list -----------------------------
    dsp_entry::ptr entryService;
    size_t factoryCount = 0;
    for (service_factory_base * f = client->get_service_list(); f != NULL; f = f->__internal__next) {
        ++factoryCount;
        if (f->get_class_guid() != dsp_entry::class_guid) continue;
        service_ptr_t<service_base> svc;
        f->instance_create(svc);
        dsp_entry::ptr e;
        if (e &= svc) entryService = e;
    }
    std::printf("  (%u service factories registered)\n", (unsigned)factoryCount);
    check(entryService.is_valid(), "registers a dsp_entry service");
    if (!entryService.is_valid()) return 1;

    pfc::string8 name;
    entryService->get_name(name);
    std::printf("  DSP name: \"%s\"\n", name.get_ptr());
    check(name.length() > 0, "dsp_entry reports a name");
    check(entryService->have_config_popup(), "dsp_entry advertises a config popup");

    const GUID dspGuid = entryService->get_guid();
    check(dspGuid != pfc::guid_null, "dsp_entry reports a non-null GUID");

    // --- default preset round trip -----------------------------------------
    dsp_preset_impl preset;
    check(entryService->get_default_preset(preset), "produces a default preset");
    check(preset.get_owner() == dspGuid, "default preset is owned by the DSP GUID");
    check(preset.get_data_size() > 0, "default preset carries data");

    dsp::ptr instance;
    check(entryService->instantiate(instance, preset), "instantiates from the default preset");
    if (!instance.is_valid()) return 1;

    // --- audio ---------------------------------------------------------------
    double peak = 0.0;
    check(pump(instance, 2, 44100, 1024, 40, &peak), "stereo 44.1 kHz output stays finite");
    check(peak > 0.01, "stereo 44.1 kHz output is not silence");
    std::printf("  (peak after processing: %.3f)\n", peak);

    check(pump(instance, 1, 44100, 1024, 10, NULL),  "handles a switch to mono mid-stream");
    check(pump(instance, 6, 48000, 512,  10, NULL),  "handles a switch to 5.1 at 48 kHz");
    check(pump(instance, 2, 192000, 4096, 5, NULL),  "handles a switch to stereo at 192 kHz");

    instance->flush();
    check(pump(instance, 2, 44100, 1024, 10, NULL),  "keeps working after flush()");

    const double latency = instance->get_latency();
    std::printf("  (reported latency: %.2f ms)\n", latency * 1000.0);
    check(latency >= 0.0 && latency < 1.0, "reports a sane latency");
    check(!instance->need_track_change_mark(), "does not request track change marks");

    // --- dsp_v3 live reconfiguration ----------------------------------------
    dsp_preset_impl altered;
    {
        // Same GUID, different payload: Surface off, half wet.
        dsp_preset_builder b;
        b << (t_uint32)1;
        b << 0.25f << 0.80f << 0.30f << 0.0f << 0.5f;
        b.finish(dspGuid, altered);
    }
    check(instance->apply_preset_(altered), "accepts a live preset change (dsp_v3)");
    check(pump(instance, 2, 44100, 1024, 10, NULL), "still finite after a live preset change");

    // --- robustness against a corrupt stored preset -------------------------
    dsp_preset_impl corrupt;
    corrupt.set_owner(dspGuid);
    const unsigned char junk[3] = { 0xFF, 0x00, 0x7F };
    corrupt.set_data(junk, sizeof(junk));
    dsp::ptr fromCorrupt;
    check(entryService->instantiate(fromCorrupt, corrupt), "instantiates from a truncated preset");
    check(fromCorrupt.is_valid() && pump(fromCorrupt, 2, 44100, 1024, 5, NULL),
          "a truncated preset falls back to something that works");

    dsp_preset_impl empty;
    empty.set_owner(dspGuid);
    empty.set_data(NULL, 0);
    dsp::ptr fromEmpty;
    check(entryService->instantiate(fromEmpty, empty), "instantiates from an empty preset");
    check(fromEmpty.is_valid() && pump(fromEmpty, 2, 44100, 1024, 5, NULL),
          "an empty preset falls back to something that works");

    // --- configuration dialog ------------------------------------------------
    {
        recording_callback cb;
        probe_args probe = {};
        const bool ran = driveDialog(entryService, preset, true, 250, cb, probe,
                                     screenshot.empty() ? NULL : screenshot.c_str());
        if (!screenshot.empty()) std::printf("  wrote %ls\n", screenshot.c_str());
        check(ran, "dsp_entry_v2::show_config_popup_v2 is available");
        check(probe.foundDialog, "config dialog opens");
        if (g_editorId != 0) {
            check(probe.foundEditor, "config dialog hosts its editor control");
            check(cb.m_count > 0, "a key press pushes a live preset update");
            check(cb.m_count > 0 && !(cb.m_last == preset),
                  "and the preset it pushes differs from the one it started with");
        } else {
            check(probe.foundSlider, "config dialog has the Filter slider");
            check(probe.labelUpdated, "moving a slider refreshes its value label");
            check(cb.m_count > 0, "moving a slider pushes a live preset update");
        }
        if (cb.m_count > 0) {
            dsp::ptr fromDialog;
            check(entryService->instantiate(fromDialog, cb.m_last),
                  "the preset the dialog produced is instantiable");
            check(fromDialog.is_valid() && pump(fromDialog, 2, 44100, 1024, 5, NULL),
                  "the preset the dialog produced processes audio");
            fromDialog.release();
        }
    }
    {
        // Cancel has to hand the caller back exactly what it was given.
        recording_callback cb;
        probe_args probe = {};
        driveDialog(entryService, preset, false, 800, cb, probe);
        check(probe.foundDialog, "config dialog opens a second time");
        check(cb.m_count > 0 && cb.m_last == preset,
              "Cancel restores the preset the dialog started with");
    }

    // Release everything before the module goes away.
    instance.release();
    fromCorrupt.release();
    fromEmpty.release();
    entryService.release();
    client->services_init(false);

    if (g_failures == 0) {
        std::printf("\nAll component checks passed.\n");
        return 0;
    }
    std::printf("\n%d component check(s) FAILED.\n", g_failures);
    return 1;
}
