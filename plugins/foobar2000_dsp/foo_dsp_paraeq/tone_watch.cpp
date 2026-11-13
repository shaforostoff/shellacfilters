/* ========================================
 *  foo_dsp_paraeq - track tone: measuring the playing track. See tone_watch.h.
 * ======================================== */

#include "stdafx.h"

#include "tone_watch.h"
#include "paraeq_tone.h"

#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace paraeq_tone_watch {

namespace {

// {6B0E5A51-2F7C-4C61-9E3B-8D21A7F4C0B2}
const GUID kEnabledGuid =
    { 0x6b0e5a51, 0x2f7c, 0x4c61, { 0x9e, 0x3b, 0x8d, 0x21, 0xa7, 0xf4, 0xc0, 0xb2 } };
cfg_bool g_enabled(kEnabledGuid, false);

bool isOn() { return (bool)g_enabled; }

enum Status { kIdle, kIneligible, kMeasuring, kReady, kFailed };

struct Job {
    abort_callback_impl abort;
};

Status                      g_status = kIdle;
paraeq_tone::Profile        g_profile;
std::string                 g_key;          //!< the track being shown
unsigned                    g_generation = 0;
std::shared_ptr<Job>        g_job;          //!< the measurement under way
std::thread                 g_thread;
std::map<std::string, paraeq_tone::Profile> g_cache;

struct Sub { void * ctx; Listener fn; };
std::vector<Sub> g_subs;

void notify() {
    const std::vector<Sub> subs = g_subs;
    for (size_t i = 0; i < subs.size(); i++) subs[i].fn(subs[i].ctx);
}

std::string keyOf(const metadb_handle_ptr & t) {
    pfc::string8 k;
    k << t->get_path() << "|" << t->get_subsong_index();
    return k.c_str();
}

void stopJob() {
    if (g_job) g_job->abort.abort();
    // The decoder checks the abort between chunks, so this is a few ms.
    if (g_thread.joinable()) g_thread.join();
    g_job.reset();
}

paraeq_tone::Profile measure(const std::string & path, t_uint32 subsong, abort_callback & abort) {
    paraeq_tone::Analyzer a;
    try {
        input_decoder::ptr dec;
        input_entry::g_open_for_decoding(dec, NULL, path.c_str(), abort);
        dec->initialize(subsong, input_flag_simpledecode, abort);
        audio_chunk_impl chunk;
        bool started = false;
        unsigned channels = 0;
        while (dec->run(chunk, abort)) {
            if (!started) {
                channels = chunk.get_channels();
                if (!a.start((double)chunk.get_sample_rate(), channels)) break;
                started = true;
            }
            // A mid-stream channel change is rare enough to end the
            // measurement there rather than handle.
            if (chunk.get_channels() != channels) break;
            a.feed(chunk.get_data(), chunk.get_sample_count());
        }
    } catch (const std::exception &) {
        return paraeq_tone::Profile();
    }
    return a.finish();
}

void startTrack(const metadb_handle_ptr & track) {
    stopJob();
    const unsigned gen = ++g_generation;
    g_profile = paraeq_tone::Profile();

    // Nobody to show it to: measure when a panel subscribes instead.
    if (!isOn() || !track.is_valid() || g_subs.empty()) {
        g_status = kIdle; g_key.clear(); notify(); return;
    }
    g_key = keyOf(track);

    const char * genre = NULL, * date = NULL;
    metadb_info_container::ptr info;
    pfc::string8 genreStr, dateStr;
    if (track->get_info_ref(info) && info.is_valid()) {
        const char * g = info->info().meta_get("GENRE", 0);
        const char * d = info->info().meta_get("DATE", 0);
        if (g) { genreStr = g; genre = genreStr.c_str(); }
        if (d) { dateStr = d;  date  = dateStr.c_str(); }
    }
    if (!paraeq_tone::eligible(genre, date)) {
        g_status = kIneligible; notify(); return;
    }

    const auto hit = g_cache.find(g_key);
    if (hit != g_cache.end()) {
        g_profile = hit->second;
        g_status = g_profile.valid ? kReady : kFailed;
        notify();
        return;
    }

    g_status = kMeasuring;
    notify();

    std::shared_ptr<Job> job(new Job);
    g_job = job;
    const std::string path = track->get_path();
    const t_uint32 subsong = track->get_subsong_index();
    const std::string key = g_key;
    g_thread = std::thread([job, path, subsong, key, gen] {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
        const paraeq_tone::Profile p = measure(path, subsong, job->abort);
        if (job->abort.is_aborting()) return;
        fb2k::inMainThread([p, key, gen] {
            g_cache[key] = p;
            if (gen != g_generation) return;
            g_profile = p;
            g_status = p.valid ? kReady : kFailed;
            notify();
        });
    });
}

void restartForNowPlaying() {
    metadb_handle_ptr now;
    playback_control::get()->get_now_playing(now);
    startTrack(now);
}

class play_watch : public play_callback_static {
public:
    unsigned get_flags() override {
        return flag_on_playback_new_track | flag_on_playback_stop;
    }
    void on_playback_new_track(metadb_handle_ptr track) override { startTrack(track); }
    void on_playback_stop(play_control::t_stop_reason reason) override {
        // Starting another track stops this one first; that is not the user
        // stopping, and clearing here would flash the hint off and on.
        if (reason == play_control::stop_reason_starting_another) return;
        startTrack(metadb_handle_ptr());
    }
    void on_playback_starting(play_control::t_track_command, bool) override {}
    void on_playback_seek(double) override {}
    void on_playback_pause(bool) override {}
    void on_playback_edited(metadb_handle_ptr) override {}
    void on_playback_dynamic_info(const file_info &) override {}
    void on_playback_dynamic_info_track(const file_info &) override {}
    void on_playback_time(double) override {}
    void on_volume_change(float) override {}
};

static play_callback_static_factory_t<play_watch> g_play_watch_factory;

class quit_watch : public initquit {
public:
    void on_init() override {}
    void on_quit() override { stopJob(); }
};

static initquit_factory_t<quit_watch> g_quit_watch_factory;

void appendDb(pfc::string8 & s, const char * name, float db) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %+.1f", name, db);
    if (!s.is_empty()) s << "  ";
    s << buf;
}

} // namespace

bool enabled() { return isOn(); }

void setEnabled(bool on) {
    if (on == isOn()) return;
    g_enabled = on;
    restartForNowPlaying();
}

void hints(const paraeq::Params & p, double sampleRate, paraeq_editor::ToneHints & out) {
    out = paraeq_editor::ToneHints();
    pfc::string8 text;
    switch (g_status) {
    case kIdle:       text = isOn() ? "nothing playing" : ""; break;
    case kIneligible: text = "only for tango, vals, milonga before 1954"; break;
    case kMeasuring:  text = "measuring..."; break;
    case kFailed:     text = "could not measure this track"; break;
    case kReady: {
        const paraeq_tone::Deviation d = paraeq_tone::deviation(g_profile);
        const paraeq_tone::Suggestion s = paraeq_tone::suggest(d, p, sampleRate);
        appendDb(text, "LF",  d.db[paraeq_tone::kBass]);
        appendDb(text, "HMF", d.db[paraeq_tone::kBrilliance]);
        appendDb(text, "HF",  d.db[paraeq_tone::kHiss]);
        text << " dB";
        for (int m = 0; m < paraeq_tone::kMeasures; m++) {
            out.show[m]   = s.valid && s.show[m];
            out.gainDb[m] = s.gainDb[m];
        }
        break;
    }
    }
    pfc::stringcvt::convert_utf8_to_wide(out.status, _countof(out.status), text.c_str(), text.length());
}

void subscribe(void * ctx, Listener fn) {
    g_subs.push_back(Sub{ ctx, fn });
    // The first panel to appear finds out about a track that was already
    // playing when foobar2000 started or when the panel was added.
    if (g_status == kIdle && isOn()) restartForNowPlaying();
}

void unsubscribe(void * ctx) {
    for (size_t i = 0; i < g_subs.size(); i++) {
        if (g_subs[i].ctx == ctx) { g_subs.erase(g_subs.begin() + (ptrdiff_t)i); break; }
    }
}

} // namespace paraeq_tone_watch
