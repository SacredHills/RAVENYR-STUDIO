#include "RigBank.h"
#include "engine/AudioEngine.h"
#include "core/ProjectFile.h"
#include "core/MidiClip.h"
#include "core/Timeline.h"

namespace ff::fx
{
namespace
{
    const juce::String liveKey ("__live__");

    std::pair<double, int> engineFormat (const AudioEngine& e)
    {
        const double sr = e.getSampleRate() > 0 ? e.getSampleRate() : 48000.0;
        const int bs = e.getBlockSize() > 0 ? e.getBlockSize() : 512;
        return { sr, juce::jmax (bs, 512) };
    }
}

bool loadImpulseFile (const juce::File& f, juce::AudioBuffer<float>& out, double& sampleRate, juce::String& error, double maxSeconds)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr)
    {
        error = f.existsAsFile() ? f.getFileName() + " is not a readable audio file." : "Missing impulse response: " + f.getFullPathName();
        return false;
    }
    if (r->lengthInSamples <= 0 || r->sampleRate <= 0)
    {
        error = f.getFileName() + " is empty.";
        return false;
    }
    const int len = (int) juce::jmin (r->lengthInSamples, (juce::int64) (maxSeconds * r->sampleRate));
    juce::AudioBuffer<float> tmp ((int) juce::jmax (1u, r->numChannels), len);
    r->read (&tmp, 0, len, 0, true, true);
    out.setSize (1, len);
    out.clear();
    for (int c = 0; c < tmp.getNumChannels(); ++c)
        out.addFrom (0, 0, tmp, c, 0, len, 1.0f / (float) tmp.getNumChannels());
    sampleRate = r->sampleRate;
    if (out.getMagnitude (0, len) < 1.0e-6f)
    {
        error = f.getFileName() + " is silent.";
        return false;
    }
    return true;
}

std::shared_ptr<AmpRig> makeOfflineRig (const juce::ValueTree& t, double sr, int block, juce::StringArray& warnings)
{
    auto rig = std::make_shared<AmpRig>();
    rig->setParamsFrom (t);
    rig->prepare (sr, block);
    const auto nam = t[rig::ids::namFile].toString();
    if (nam.isNotEmpty())
    {
        juce::String err;
        if (auto m = NamModel::load (juce::File (nam), err))
        {
            m->prepare (sr, block);
            rig->setModel (std::move (m));
        }
        else warnings.add (err + " The built-in amp was used instead.");
    }
    const auto ir = t[rig::ids::irFile].toString();
    if (ir.isNotEmpty())
    {
        juce::AudioBuffer<float> b;
        double irRate = 0;
        juce::String err;
        if (loadImpulseFile (juce::File (ir), b, irRate, err))
        {
            rig->setImpulseResponse (std::move (b), irRate);
            rig->finishLoadingOffline();
        }
        else warnings.add (err + " The built-in cabinet filter was used instead.");
    }
    if (const auto ir2 = t[rig::ids::irFile2].toString(); ir2.isNotEmpty())
    {
        juce::AudioBuffer<float> b;
        double irRate = 0;
        juce::String err;
        if (loadImpulseFile (juce::File (ir2), b, irRate, err))
        {
            rig->setImpulseResponse2 (std::move (b), irRate);
            rig->finishLoadingOffline();
        }
        else warnings.add (err + " The second cabinet was left out.");
    }
    for (auto c : t)
    {
        if (! c.hasType (rig::ids::PLUGIN)) continue;
        const int slot = (int) c[rig::ids::slot];
        const auto name = c[rig::ids::name].toString();
        if (! juce::isPositiveAndBelow (slot, rig::numPluginSlots) || c[rig::ids::desc].toString().isEmpty()) continue;
        auto* host = pluginHost();
        if (host == nullptr) { warnings.add ("Plugin \"" + name + "\" was skipped (plugin hosting is not available here)."); continue; }
        juce::String err;
        auto inst = host->createBlocking (c[rig::ids::desc].toString(), sr, block, err);
        if (inst == nullptr) { warnings.add ("Plugin \"" + name + "\" could not be loaded" + (err.isNotEmpty() ? ": " + err : juce::String()) + "; it was skipped."); continue; }
        restorePluginState (*inst, c[rig::ids::state].toString());
        preparePlugin (*inst, sr, block);
        rig->setPlugin (slot, std::shared_ptr<juce::AudioPluginInstance> (inst.release()));
    }
    {
        // take over the model now, so the rig reports its latency before the first rendered block
        float z1[32] = {}, z2[32] = {};
        rig->process (z1, z2, 32);
    }
    rig->reset();
    return rig;
}

std::shared_ptr<InsertChain> makeOfflineInserts (const juce::ValueTree& track, double sr, int block, juce::StringArray& warnings)
{
    const auto list = rig::findInserts (track);
    if (rig::numInserts (track) == 0) return nullptr;
    auto chain = std::make_shared<InsertChain>();
    chain->prepare (sr, block);
    std::array<int, InsertChain::maxSlots> order {};
    int count = 0;
    std::vector<juce::ValueTree> ordered;   // by slot number, like the live chain (gaps in the numbers leave nothing out)
    for (auto c : list)
        if (c.hasType (rig::ids::PLUGIN)) ordered.push_back (c);
    std::stable_sort (ordered.begin(), ordered.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) {
        return (int) a[rig::ids::slot] < (int) b[rig::ids::slot];
    });
    for (int slot = 0; slot < juce::jmin ((int) ordered.size(), (int) InsertChain::maxSlots); ++slot)
    {
        const auto c = ordered[(size_t) slot];
        const auto name = c[rig::ids::name].toString();
        auto* host = pluginHost();
        if (host == nullptr) { warnings.add ("Insert \"" + name + "\" was skipped (plugin hosting is not available here)."); continue; }
        juce::String err;
        auto inst = host->createBlocking (c[rig::ids::desc].toString(), sr, block, err);
        if (inst == nullptr) { warnings.add ("Insert \"" + name + "\" could not be loaded" + (err.isNotEmpty() ? ": " + err : juce::String()) + "; it was skipped."); continue; }
        restorePluginState (*inst, c[rig::ids::state].toString());
        preparePlugin (*inst, sr, block, true);
        chain->setPlugin (slot, std::shared_ptr<juce::AudioPluginInstance> (inst.release()));
        chain->bypass[(size_t) slot] = (bool) c[rig::ids::bypass];
        chain->sidechainId[(size_t) slot] = c[rig::ids::sidechain].toString();
        order[(size_t) count++] = slot;
    }
    chain->setOrder (order, count);
    return chain;
}

std::shared_ptr<InstrumentSlot> makeOfflineInstrument (const juce::ValueTree& track, double sr, int block, juce::StringArray& warnings)
{
    const auto node = track.getChildWithName (midiclip::INSTRUMENT);
    const auto desc = node[rig::ids::desc].toString();
    if (desc.isEmpty()) return nullptr;
    const auto name = node[rig::ids::name].toString();
    auto* host = pluginHost();
    if (host == nullptr) { warnings.add ("Instrument \"" + name + "\" was skipped (plugin hosting is not available here)."); return nullptr; }
    juce::String err;
    auto inst = host->createBlocking (desc, sr, block, err);
    if (inst == nullptr) { warnings.add ("Instrument \"" + name + "\" could not be loaded" + (err.isNotEmpty() ? ": " + err : juce::String()) + "."); return nullptr; }
    restorePluginState (*inst, node[rig::ids::state].toString());
    const int aux = juce::jlimit (0, 15, (int) node[rig::ids::auxOutputs]);
    preparePlugin (*inst, sr, block, false, aux);
    auto slot = std::make_shared<InstrumentSlot>();
    slot->auxOutputs = aux;
    slot->prepare (sr, block);
    slot->bypassed = (bool) node[rig::ids::bypass];
    slot->setPlugin (std::shared_ptr<juce::AudioPluginInstance> (inst.release()));
    return slot;
}

// ---------------------------------------------------------------------------------------------------------------

RigBank::RigBank (AudioEngine& e) : engine (e)
{
    live = rig::makeDefault();
    live.setProperty (rig::ids::enabled, false, nullptr);
    live.addListener (this);
    liveEntry.rig = makeRig();
    liveEntry.tree = live;
    syncEntry (liveEntry);
    engine.liveRig.store (liveEntry.rig.get());
    engine.setPrepareHook ([this] (double sr, int bs) { prepareAll (sr, bs); });
    startTimerHz (10);
}

RigBank::~RigBank()
{
    stopTimer();
    *alive = false;   // plugins still being created are dropped when they arrive
    releaseAllPlugins();
    engine.setPrepareHook (nullptr);
    engine.liveRigOn.store (false);
    engine.liveRig.store (nullptr);
    engine.waitForAudioCallback();
    loader.removeAllJobs (true, 5000);
    pluginPrep.removeAllJobs (true, 30000);
    root.removeListener (this);
    live.removeListener (this);
}

std::shared_ptr<AmpRig> RigBank::makeRig()
{
    auto r = std::make_shared<AmpRig> (&convQueue);
    const auto [sr, bs] = engineFormat (engine);
    r->prepare (sr, bs);
    return r;
}

void RigBank::prepareAll (double sr, int block)
{
    const juce::ScopedLock sl (lock);
    block = juce::jmax (block, 512);
    liveEntry.rig->prepare (sr, block);
    for (auto& [id, e] : tracks) e.rig->prepare (sr, block);
    for (auto& [id, e] : inserts) e.chain->prepare (sr, block);
    for (auto& [id, e] : instruments)
    {
        e.slot->prepare (sr, block);
        if (auto p = e.slot->getPlugin()) preparePlugin (*p, sr, block, false, e.slot->auxOutputs.load());
    }
}

void RigBank::rebind (juce::ValueTree projectRoot)
{
    root.removeListener (this);
    root = projectRoot;
    root.addListener (this);
    syncAll();
}

void RigBank::setLiveTree (juce::ValueTree t)
{
    if (! t.hasType (rig::ids::RIG)) return;
    live.copyPropertiesFrom (t, nullptr);   // keeps the same node (listeners, UI) and fires property changes
    live.removeAllChildren (nullptr);
    for (auto c : t) live.appendChild (c.createCopy(), nullptr);   // plugin slots
}

std::shared_ptr<AmpRig> RigBank::rigForTrack (const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    if (auto it = tracks.find (id); it != tracks.end()) return it->second.rig;
    return nullptr;
}

RigBank::Entry* RigBank::entryFor (const juce::ValueTree& t)
{
    if (t == live) return &liveEntry;
    for (auto& [id, e] : tracks)
        if (e.tree == t) return &e;
    return nullptr;
}

const RigBank::Entry* RigBank::entryFor (const juce::ValueTree& t) const
{
    return const_cast<RigBank*> (this)->entryFor (t);
}

juce::StringArray RigBank::loadingNames() const
{
    juce::StringArray out;
    const auto nameOf = [] (const juce::String& n) { return n.isNotEmpty() ? n : juce::String ("plugin"); };
    const auto onTrack = [] (const juce::ValueTree& track) {
        const auto n = track[timeline::ids::name].toString();
        return n.isNotEmpty() ? " (" + n + ")" : juce::String();
    };
    const auto rigLoading = [&] (const Entry& e, const juce::String& where) {
        for (auto& p : e.status.plugins)
            if (p.loading) out.add (nameOf (p.name) + where);
        if (e.status.modelLoading) out.add ("amp model " + juce::File (e.status.modelPath).getFileNameWithoutExtension() + where);
        if (e.status.irLoading) out.add ("cabinet " + juce::File (e.status.irPath).getFileNameWithoutExtension() + where);
    };
    rigLoading (liveEntry, " (live rig)");
    for (auto& [id, e] : tracks) rigLoading (e, onTrack (e.tree.getParent()));
    for (auto& [id, e] : inserts)
        for (auto& s : e.status)
            if (s.loading) out.add (nameOf (s.name) + onTrack (e.track));
    for (auto& [id, e] : instruments)
        if (e.status.loading) out.add (nameOf (e.status.name) + onTrack (e.node.getParent()));
    return out;
}

RigBank::Status RigBank::statusFor (const juce::ValueTree& t) const
{
    if (auto* e = entryFor (t)) return e->status;
    Status s;
    s.modelPath = t[rig::ids::namFile].toString();
    s.irPath = t[rig::ids::irFile].toString();
    s.irPath2 = t[rig::ids::irFile2].toString();
    return s;
}

AmpRig* RigBank::rigFor (const juce::ValueTree& t) const
{
    auto* e = entryFor (t);
    return e != nullptr ? e->rig.get() : nullptr;
}

void RigBank::syncAll()
{
    bool changed = false;
    std::map<juce::String, juce::ValueTree> wanted;
    for (const auto& t : root.getChildWithName (project::ids::TRACKS))
        if (t.hasType (timeline::ids::TRACK))
            if (auto r = rig::find (t); rig::isEnabled (r))
                wanted[t[timeline::ids::id].toString()] = r;

    {
        const juce::ScopedLock sl (lock);
        for (auto it = tracks.begin(); it != tracks.end();)
        {
            if (auto w = wanted.find (it->first); w == wanted.end() || w->second != it->second.tree)
            {
                for (int sl = 0; sl < rig::numPluginSlots; ++sl)   // editors close; the plugins die with the rig
                    if (auto p = it->second.rig->getPlugin (sl); p != nullptr && onPluginRemoved) onPluginRemoved (p.get());
                it = tracks.erase (it);   // the playback graph may still hold the rig; it dies with the graph
                changed = true;
            }
            else ++it;
        }
    }
    for (auto& [id, t] : wanted)
    {
        if (tracks.count (id) > 0) { syncEntry (tracks[id]); continue; }
        Entry e;
        e.rig = makeRig();
        e.tree = t;
        {
            const juce::ScopedLock sl (lock);
            tracks[id] = std::move (e);
        }
        syncEntry (tracks[id]);
        changed = true;
    }

    // ---- inserts ----
    std::map<juce::String, juce::ValueTree> wantedInserts;
    for (const auto& t : root.getChildWithName (project::ids::TRACKS))
        if (t.hasType (timeline::ids::TRACK) && rig::numInserts (t) > 0)
            wantedInserts[t[timeline::ids::id].toString()] = t;
    if (const auto master = root.getChildWithName (project::ids::MASTER); master.isValid() && rig::numInserts (master) > 0)
        wantedInserts[project::masterId] = master;
    for (auto it = inserts.begin(); it != inserts.end();)
    {
        if (auto w = wantedInserts.find (it->first); w == wantedInserts.end() || w->second != it->second.track)
        {
            // Take the plugins out of the chain the way a single removal does (editor closed, the audio thread let go, then
            // shut down here on the message thread): the playback graph may still hold the chain for a moment, and it must
            // never be what ends up destroying a running plugin.
            releaseInsertPlugins (it->second);
            const juce::ScopedLock sl (lock);
            it = inserts.erase (it);
            changed = true;
        }
        else ++it;
    }
    for (auto& [id, t] : wantedInserts)
    {
        if (inserts.count (id) == 0)
        {
            InsertEntry e;
            e.chain = std::make_shared<InsertChain>();
            const auto [sr, bs] = engineFormat (engine);
            e.chain->prepare (sr, bs);
            e.track = t;
            {
                const juce::ScopedLock sl (lock);
                inserts[id] = std::move (e);
            }
            changed = true;
        }
        syncInserts (inserts[id]);
    }
    syncInstruments (changed);
    if (changed && onRigsChanged) onRigsChanged();
}

void RigBank::syncInstruments (bool& changed)
{
    std::map<juce::String, juce::ValueTree> wanted;
    for (auto t : root.getChildWithName (project::ids::TRACKS))
        if (timeline::isInstrumentTrack (t))
            if (auto n = t.getChildWithName (midiclip::INSTRUMENT); n[rig::ids::desc].toString().isNotEmpty())
                wanted[t[timeline::ids::id].toString()] = n;
    for (auto it = instruments.begin(); it != instruments.end();)
    {
        if (auto w = wanted.find (it->first); w == wanted.end() || w->second != it->second.node)
        {
            if (auto p = it->second.slot->getPlugin(); p != nullptr && onPluginRemoved) onPluginRemoved (p.get());
            const juce::ScopedLock sl (lock);
            it = instruments.erase (it);
            changed = true;
        }
        else ++it;
    }
    for (auto& [id, n] : wanted)
    {
        if (instruments.count (id) == 0)
        {
            InstrumentEntry e;
            e.slot = std::make_shared<InstrumentSlot>();
            const auto [sr, bs] = engineFormat (engine);
            e.slot->prepare (sr, juce::jmax (bs, 512));
            e.node = n;
            {
                const juce::ScopedLock sl (lock);
                instruments[id] = std::move (e);
            }
            changed = true;
        }
        auto& e = instruments[id];
        e.slot->bypassed = (bool) n[rig::ids::bypass];
        const int aux = juce::jlimit (0, 15, (int) n[rig::ids::auxOutputs]);
        e.slot->auxOutputs = aux;
        if (e.loadedDesc != n[rig::ids::desc].toString() || e.loadedAux != aux) loadInstrument (id);
    }
}

void RigBank::loadInstrument (const juce::String& key)
{
    auto& e = instruments[key];
    e.loadedDesc = e.node[rig::ids::desc].toString();
    e.loadedAux = e.slot->auxOutputs.load();
    e.status = {};
    e.status.desc = e.loadedDesc;
    e.status.name = e.node[rig::ids::name].toString();
    const int gen = e.generation = ++generationCounter;
    auto* host = pluginHost();
    if (host == nullptr) { e.status.error = "Plugin hosting is not available."; return; }
    e.status.loading = true;
    const auto [sr, bs] = engineFormat (engine);
    std::weak_ptr<bool> token = alive;
    host->createAsync (e.loadedDesc, sr, bs, [this, token, key, gen] (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String err) {
        if (token.expired()) return;
        auto it = instruments.find (key);
        if (it == instruments.end() || it->second.generation != gen) return;
        auto& en = it->second;
        if (inst == nullptr)
        {
            en.status.loading = false;
            en.status.error = err.isNotEmpty() ? err : juce::String ("The instrument could not be loaded.");
            notifyStatus();
            return;
        }
        const auto [r0, b0] = engineFormat (engine);
        const double rate0 = en.slot->getPreparedRate() > 0 ? en.slot->getPreparedRate() : r0;
        prepareAsync (std::move (inst), en.node[rig::ids::state].toString(), rate0, juce::jmax (b0, 512), false, en.slot->auxOutputs.load(),
                      [this, key, gen] (std::unique_ptr<juce::AudioPluginInstance> ready) {
            auto it2 = instruments.find (key);
            if (it2 == instruments.end() || it2->second.generation != gen) return;
            auto& en2 = it2->second;
            en2.status.loading = false;
            const auto [r, b] = engineFormat (engine);
            std::shared_ptr<juce::AudioPluginInstance> old;
            {
                const juce::ScopedLock lk (lock);
                preparePlugin (*ready, en2.slot->getPreparedRate() > 0 ? en2.slot->getPreparedRate() : r, juce::jmax (b, 512), false, en2.slot->auxOutputs.load());
                en2.status.latency = ready->getLatencySamples();
                old = en2.slot->setPlugin (std::shared_ptr<juce::AudioPluginInstance> (ready.release()));
            }
            en2.status.loaded = true;
            if (old != nullptr && onPluginRemoved) onPluginRemoved (old.get());
            releasePlugin (std::move (old));
            notifyStatus();
            if (onRigsChanged) onRigsChanged();   // latency may have changed
        });
    });
}

std::shared_ptr<InstrumentSlot> RigBank::instrumentForTrack (const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    if (auto it = instruments.find (id); it != instruments.end()) return it->second.slot;
    return nullptr;
}

RigBank::Status::Plugin RigBank::instrumentStatus (const juce::ValueTree& track) const
{
    if (auto it = instruments.find (track[timeline::ids::id].toString()); it != instruments.end()) return it->second.status;
    return {};
}

std::shared_ptr<juce::AudioPluginInstance> RigBank::instrumentPlugin (const juce::ValueTree& track) const
{
    if (auto it = instruments.find (track[timeline::ids::id].toString()); it != instruments.end()) return it->second.slot->getPlugin();
    return nullptr;
}

void RigBank::syncEntry (Entry& e)
{
    e.rig->setParamsFrom (e.tree);
    if (&e == &liveEntry)
    {
        engine.liveRigOn.store (rig::isEnabled (live));
        engine.liveInput.store ((int) live.getProperty (rig::ids::input, -1));
    }
    if (e.tree[rig::ids::namFile].toString() != e.status.modelPath) requestModel (e, false);
    if (e.tree[rig::ids::irFile].toString() != e.status.irPath) requestIr (e, false);
    if (e.tree[rig::ids::irFile2].toString() != e.status.irPath2) requestIr (e, true);
    for (int sl = 0; sl < rig::numPluginSlots; ++sl)
    {
        const auto node = rig::pluginNode (e.tree, sl);
        if (node[rig::ids::desc].toString() != e.status.plugins[(size_t) sl].desc) requestPlugin (e, sl);
        else e.status.plugins[(size_t) sl].name = node[rig::ids::name].toString();
    }
}

juce::String RigBank::keyOf (const Entry& e) const
{
    return (&e == &liveEntry) ? liveKey : e.tree.getParent()[timeline::ids::id].toString();
}

RigBank::Entry* RigBank::entryByKey (const juce::String& key)
{
    if (key == liveKey) return &liveEntry;
    if (auto it = tracks.find (key); it != tracks.end()) return &it->second;
    return nullptr;
}

void RigBank::releasePlugin (std::shared_ptr<juce::AudioPluginInstance> old)
{
    if (old == nullptr) return;
    if (onPluginRemoved) onPluginRemoved (old.get());
    engine.waitForAudioCallback();   // the audio thread may still be inside it
    unregisterPluginInstance (old.get());
    {
        PluginActivity activity (*old, "shutting down");
        old->releaseResources();
        old.reset();
    }
}

void RigBank::releaseAllPlugins()
{
    std::vector<std::shared_ptr<juce::AudioPluginInstance>> all;
    {
        const juce::ScopedLock lk (lock);
        auto takeRig = [&all] (Entry& e) {
            for (int sl = 0; sl < rig::numPluginSlots; ++sl)
                if (auto p = e.rig->setPlugin (sl, nullptr)) all.push_back (std::move (p));
        };
        takeRig (liveEntry);
        for (auto& [k, e] : tracks) takeRig (e);
        for (auto& [k, e] : inserts)
            for (int i = 0; i < InsertChain::maxSlots; ++i)
                if (auto p = e.chain->setPlugin (i, nullptr)) all.push_back (std::move (p));
        for (auto& [k, e] : instruments)
            if (auto p = e.slot->setPlugin (nullptr)) all.push_back (std::move (p));
    }
    for (auto& p : all)
        if (onPluginRemoved) onPluginRemoved (p.get());   // editor windows close first
    engine.waitForAudioCallback();   // once for all of them: the audio thread may still be inside one
    for (auto& p : all)
    {
        unregisterPluginInstance (p.get());
        PluginActivity activity (*p, "shutting down");
        p->releaseResources();
        p.reset();
    }
}

void RigBank::releaseEntryPlugins (Entry& e)
{
    for (int sl = 0; sl < rig::numPluginSlots; ++sl)
    {
        std::shared_ptr<juce::AudioPluginInstance> old;
        {
            const juce::ScopedLock lk (lock);
            old = e.rig->setPlugin (sl, nullptr);
        }
        releasePlugin (std::move (old));
    }
}

void RigBank::prepareAsync (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String state, double rate, int block, bool sidechain, int aux,
                            std::function<void (std::unique_ptr<juce::AudioPluginInstance>)> then)
{
    auto holder = std::make_shared<std::unique_ptr<juce::AudioPluginInstance>> (std::move (inst));
    std::weak_ptr<bool> token = alive;
    ++prepJobs;
    pluginPrep.addJob ([this, holder, token, state = std::move (state), rate, block, sidechain, aux, then = std::move (then)] {
        restorePluginState (**holder, state);
        {
            const std::lock_guard<std::mutex> lk (readyMutex);
            readyQueue.push_back ([holder, then] { then (std::move (*holder)); });
        }
        --prepJobs;
        juce::MessageManager::callAsync ([this, token] { if (! token.expired()) drainReady(); });
    });
}

void RigBank::drainReady()
{
    std::vector<std::function<void()>> run;
    {
        const std::lock_guard<std::mutex> lk (readyMutex);
        run.swap (readyQueue);
    }
    for (auto& f : run) f();
}

void RigBank::finishLoads()
{
    while (prepJobs.load() > 0) juce::Thread::sleep (2);
    drainReady();
}

void RigBank::requestPlugin (Entry& e, int slot)
{
    auto& st = e.status.plugins[(size_t) slot];
    const auto node = rig::pluginNode (e.tree, slot);
    st = {};
    st.desc = node[rig::ids::desc].toString();
    st.name = node[rig::ids::name].toString();
    const int gen = ++generationCounter;
    e.pluginGeneration[(size_t) slot] = gen;
    if (st.desc.isEmpty())
    {
        std::shared_ptr<juce::AudioPluginInstance> old;
        {
            const juce::ScopedLock lk (lock);
            old = e.rig->setPlugin (slot, nullptr);
        }
        releasePlugin (std::move (old));
        notifyStatus();
        return;
    }
    auto* host = pluginHost();
    if (host == nullptr)
    {
        st.error = "Plugin hosting is not available.";
        notifyStatus();
        return;
    }
    st.loading = true;
    const auto [sr, bs] = engineFormat (engine);
    std::weak_ptr<bool> token = alive;
    const auto key = keyOf (e);
    host->createAsync (st.desc, sr, bs, [this, token, key, slot, gen] (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String err) {
        if (token.expired()) return;
        auto* en = entryByKey (key);
        if (en == nullptr || en->pluginGeneration[(size_t) slot] != gen) return;   // superseded; inst is freed here
        auto& s2 = en->status.plugins[(size_t) slot];
        if (inst == nullptr)
        {
            s2.loading = false;
            s2.error = err.isNotEmpty() ? err : juce::String ("The plugin could not be loaded.");
            notifyStatus();
            return;
        }
        const auto [r0, b0] = engineFormat (engine);
        const double rate0 = en->rig->getPreparedRate() > 0 ? en->rig->getPreparedRate() : r0;
        prepareAsync (std::move (inst), rig::pluginNode (en->tree, slot)[rig::ids::state].toString(), rate0, juce::jmax (b0, 512), false, 0,
                      [this, key, slot, gen] (std::unique_ptr<juce::AudioPluginInstance> ready) {
            auto* en2 = entryByKey (key);
            if (en2 == nullptr || en2->pluginGeneration[(size_t) slot] != gen) return;   // superseded meanwhile; `ready` is freed here
            auto& s3 = en2->status.plugins[(size_t) slot];
            s3.loading = false;
            const auto [r, b] = engineFormat (engine);
            std::shared_ptr<juce::AudioPluginInstance> old;
            {
                const juce::ScopedLock lk (lock);   // not while the device re-prepares the rigs
                preparePlugin (*ready, en2->rig->getPreparedRate() > 0 ? en2->rig->getPreparedRate() : r, juce::jmax (b, 512));   // quick when it is already prepared like this
                s3.latency = ready->getLatencySamples();
                old = en2->rig->setPlugin (slot, std::shared_ptr<juce::AudioPluginInstance> (ready.release()));
            }
            s3.loaded = true;
            releasePlugin (std::move (old));
            notifyStatus();
        });
    });
    notifyStatus();
}

int RigBank::physicalOf (const InsertEntry& e, const juce::ValueTree& node) const
{
    for (int i = 0; i < InsertChain::maxSlots; ++i)
        if (e.nodes[(size_t) i].isValid() && e.nodes[(size_t) i] == node) return i;
    return -1;
}

void RigBank::syncInserts (InsertEntry& e)
{
    const auto list = rig::findInserts (e.track);
    // slots whose node is gone (removed / replaced)
    for (int i = 0; i < InsertChain::maxSlots; ++i)
    {
        auto& n = e.nodes[(size_t) i];
        if (! n.isValid()) continue;
        if (! n.getParent().isValid() || n.getParent() != list)
        {
            n = {};
            e.status[(size_t) i] = {};
            e.generation[(size_t) i] = ++generationCounter;
            std::shared_ptr<juce::AudioPluginInstance> old;
            {
                const juce::ScopedLock lk (lock);
                old = e.chain->setPlugin (i, nullptr);
            }
            releasePlugin (std::move (old));
        }
    }
    // new nodes get a free physical slot
    for (auto c : list)
    {
        if (! c.hasType (rig::ids::PLUGIN) || physicalOf (e, c) >= 0) continue;
        for (int i = 0; i < InsertChain::maxSlots; ++i)
            if (! e.nodes[(size_t) i].isValid()) { e.nodes[(size_t) i] = c; loadInsert (e, i); break; }
    }
    // play order and bypass: by slot number (list position breaks ties). Gaps or repeated numbers in a project file
    // must never leave plugins out of the chain: every plugin in the list plays.
    std::vector<juce::ValueTree> ordered;
    for (auto c : list)
        if (c.hasType (rig::ids::PLUGIN)) ordered.push_back (c);
    std::stable_sort (ordered.begin(), ordered.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) {
        return (int) a[rig::ids::slot] < (int) b[rig::ids::slot];
    });
    std::array<int, InsertChain::maxSlots> order {};
    int count = 0;
    for (auto& node : ordered)
    {
        const int phys = physicalOf (e, node);
        if (phys < 0 || count >= InsertChain::maxSlots) continue;
        order[(size_t) count++] = phys;
        e.chain->bypass[(size_t) phys] = (bool) node[rig::ids::bypass];
        e.chain->sidechainId[(size_t) phys] = node[rig::ids::sidechain].toString();
        e.status[(size_t) phys].name = node[rig::ids::name].toString();
    }
    e.chain->setOrder (order, count);
    notifyStatus();
}

void RigBank::loadInsert (InsertEntry& e, int phys)
{
    auto& st = e.status[(size_t) phys];
    const auto node = e.nodes[(size_t) phys];
    st = {};
    st.desc = node[rig::ids::desc].toString();
    st.name = node[rig::ids::name].toString();
    const int gen = ++generationCounter;
    e.generation[(size_t) phys] = gen;
    auto* host = pluginHost();
    if (host == nullptr) { st.error = "Plugin hosting is not available."; return; }
    st.loading = true;
    const auto [sr, bs] = engineFormat (engine);
    std::weak_ptr<bool> token = alive;
    const auto key = e.track[timeline::ids::id].toString();
    host->createAsync (st.desc, sr, bs, [this, token, key, phys, gen] (std::unique_ptr<juce::AudioPluginInstance> inst, juce::String err) {
        if (token.expired()) return;
        auto it = inserts.find (key);
        if (it == inserts.end() || it->second.generation[(size_t) phys] != gen) return;
        auto& en = it->second;
        auto& s2 = en.status[(size_t) phys];
        if (inst == nullptr)
        {
            s2.loading = false;
            s2.error = err.isNotEmpty() ? err : juce::String ("The plugin could not be loaded.");
            notifyStatus();
            return;
        }
        const auto [r0, b0] = engineFormat (engine);
        const double rate0 = en.chain->getPreparedRate() > 0 ? en.chain->getPreparedRate() : r0;
        prepareAsync (std::move (inst), en.nodes[(size_t) phys][rig::ids::state].toString(), rate0, juce::jmax (b0, 512), true, 0,
                      [this, key, phys, gen] (std::unique_ptr<juce::AudioPluginInstance> ready) {
            auto it2 = inserts.find (key);
            if (it2 == inserts.end() || it2->second.generation[(size_t) phys] != gen) return;
            auto& en2 = it2->second;
            auto& s3 = en2.status[(size_t) phys];
            s3.loading = false;
            const auto [r, b] = engineFormat (engine);
            std::shared_ptr<juce::AudioPluginInstance> old;
            {
                const juce::ScopedLock lk (lock);
                preparePlugin (*ready, en2.chain->getPreparedRate() > 0 ? en2.chain->getPreparedRate() : r, juce::jmax (b, 512), true);
                s3.latency = ready->getLatencySamples();
                old = en2.chain->setPlugin (phys, std::shared_ptr<juce::AudioPluginInstance> (ready.release()));
            }
            s3.loaded = true;
            releasePlugin (std::move (old));
            notifyStatus();
        });
    });
}

void RigBank::releaseInsertPlugins (InsertEntry& e)
{
    for (int i = 0; i < InsertChain::maxSlots; ++i)
    {
        std::shared_ptr<juce::AudioPluginInstance> old;
        {
            const juce::ScopedLock lk (lock);
            old = e.chain->setPlugin (i, nullptr);
        }
        releasePlugin (std::move (old));
    }
}

std::shared_ptr<InsertChain> RigBank::insertsForTrack (const juce::String& id) const
{
    const juce::ScopedLock sl (lock);
    if (auto it = inserts.find (id); it != inserts.end()) return it->second.chain;
    return nullptr;
}

RigBank::Status::Plugin RigBank::insertStatus (const juce::ValueTree& track, int slot) const
{
    if (auto it = inserts.find (track[timeline::ids::id].toString()); it != inserts.end())
        if (const int phys = physicalOf (it->second, rig::insertNode (track, slot)); phys >= 0) return it->second.status[(size_t) phys];
    return {};
}

std::shared_ptr<juce::AudioPluginInstance> RigBank::insertPlugin (const juce::ValueTree& track, int slot) const
{
    if (auto it = inserts.find (track[timeline::ids::id].toString()); it != inserts.end())
        if (const int phys = physicalOf (it->second, rig::insertNode (track, slot)); phys >= 0) return it->second.chain->getPlugin (phys);
    return nullptr;
}

std::shared_ptr<juce::AudioPluginInstance> RigBank::pluginFor (const juce::ValueTree& t, int slot) const
{
    auto* e = entryFor (t);
    return e != nullptr ? e->rig->getPlugin (slot) : nullptr;
}

void RigBank::retryFailedPlugins()
{
    auto retryRig = [this] (Entry& e) {
        for (int sl = 0; sl < rig::numPluginSlots; ++sl)
            if (e.status.plugins[(size_t) sl].error.isNotEmpty()) requestPlugin (e, sl);
    };
    retryRig (liveEntry);
    for (auto& [k, e] : tracks) retryRig (e);
    for (auto& [k, e] : inserts)
        for (int i = 0; i < InsertChain::maxSlots; ++i)
            if (e.nodes[(size_t) i].isValid() && e.status[(size_t) i].error.isNotEmpty()) loadInsert (e, i);
    juce::StringArray failed;
    for (auto& [k, e] : instruments) if (e.status.error.isNotEmpty()) failed.add (k);
    for (auto& k : failed) loadInstrument (k);
    notifyStatus();
}

void RigBank::captureAllPluginStates (bool onlyChanged)
{
    auto captureOne = [onlyChanged] (juce::AudioPluginInstance& p, juce::ValueTree node) {
        if (! node.isValid()) return;
        if (onlyChanged && ! pluginStateNeedsCapture (p)) return;
        const auto st = capturePluginState (p);
        const bool differs = st != node[rig::ids::state].toString();
        if (differs) node.setProperty (rig::ids::state, st, nullptr);
        pluginStateCaptured (p, differs);
    };
    auto capture = [&captureOne] (Entry& e) {
        for (int sl = 0; sl < rig::numPluginSlots; ++sl)
            if (auto p = e.rig->getPlugin (sl)) captureOne (*p, rig::pluginNode (e.tree, sl));
    };
    capture (liveEntry);
    for (auto& [k, e] : tracks) capture (e);
    for (auto& [k, e] : instruments)
        if (auto p = e.slot->getPlugin()) captureOne (*p, e.node);
    for (auto& [k, e] : inserts)
        for (int i = 0; i < InsertChain::maxSlots; ++i)
            if (auto p = e.chain->getPlugin (i)) captureOne (*p, e.nodes[(size_t) i]);
}

void RigBank::requestModel (Entry& e, bool)
{
    const auto path = e.tree[rig::ids::namFile].toString();
    const auto key = (&e == &liveEntry) ? liveKey : e.tree.getParent()[timeline::ids::id].toString();
    e.status.modelPath = path;
    e.status.modelError = {};
    e.status.modelLoaded = false;
    e.status.modelInfo = {};
    const int gen = ++generationCounter;
    e.generation = gen;
    if (path.isEmpty())
    {
        const juce::ScopedLock sl (lock);
        e.rig->clearModel();
        e.status.modelLoading = false;
        notifyStatus();
        return;
    }
    e.status.modelLoading = true;
    const auto [sr, bs] = engineFormat (engine);
    std::weak_ptr<bool> token = alive;
    loader.addJob ([this, token, key, gen, path, sr = sr, bs = bs] {
        juce::String err;
        auto m = NamModel::load (juce::File (path), err);
        if (m != nullptr) m->prepare (sr, bs);
        auto holder = std::make_shared<std::unique_ptr<NamModel>> (std::move (m));
        juce::MessageManager::callAsync ([this, token, key, gen, holder, err] {
            if (token.expired()) return;
            Entry* e = key == liveKey ? &liveEntry : nullptr;
            if (e == nullptr)
                if (auto it = tracks.find (key); it != tracks.end()) e = &it->second;
            if (e == nullptr || e->generation != gen) return;   // superseded or the track's rig is gone
            e->status.modelLoading = false;
            if (*holder != nullptr)
            {
                e->status.modelInfo = (*holder)->info();
                e->status.modelLoaded = true;
                const juce::ScopedLock sl (lock);
                // the device may have restarted at another rate while loading: the rig flags it and we reload
                e->rig->setModel (std::move (*holder));
            }
            else
            {
                e->status.modelError = err;
                const juce::ScopedLock sl (lock);
                e->rig->clearModel();
            }
            notifyStatus();
        });
    });
    notifyStatus();
}

void RigBank::requestIr (Entry& e, bool second)
{
    const auto path = e.tree[second ? rig::ids::irFile2 : rig::ids::irFile].toString();
    const auto key = (&e == &liveEntry) ? liveKey : e.tree.getParent()[timeline::ids::id].toString();
    auto& st = e.status;
    auto& statusPath = second ? st.irPath2 : st.irPath;
    auto& error = second ? st.irError2 : st.irError;
    auto& loaded = second ? st.irLoaded2 : st.irLoaded;
    auto& loading = second ? st.irLoading2 : st.irLoading;
    auto& name = second ? st.irName2 : st.irName;
    statusPath = path;
    error = {};
    loaded = false;
    name = {};
    if (path.isEmpty())
    {
        if (second) e.rig->clearImpulseResponse2(); else e.rig->clearImpulseResponse();
        loading = false;
        notifyStatus();
        return;
    }
    loading = true;
    std::weak_ptr<bool> token = alive;
    loader.addJob ([this, token, key, path, second] {
        auto buf = std::make_shared<juce::AudioBuffer<float>>();
        double rate = 0;
        juce::String err;
        const bool ok = loadImpulseFile (juce::File (path), *buf, rate, err);
        juce::MessageManager::callAsync ([this, token, key, path, buf, rate, err, ok, second] {
            if (token.expired()) return;
            Entry* e = key == liveKey ? &liveEntry : nullptr;
            if (e == nullptr)
                if (auto it = tracks.find (key); it != tracks.end()) e = &it->second;
            if (e == nullptr) return;
            auto& st = e->status;
            if ((second ? st.irPath2 : st.irPath) != path) return;
            (second ? st.irLoading2 : st.irLoading) = false;
            if (ok)
            {
                (second ? st.irLoaded2 : st.irLoaded) = true;
                (second ? st.irName2 : st.irName) = juce::File (path).getFileNameWithoutExtension();
                if (! second) st.irSeconds = buf->getNumSamples() / rate;
                const juce::ScopedLock sl (lock);
                if (second) e->rig->setImpulseResponse2 (std::move (*buf), rate);
                else e->rig->setImpulseResponse (std::move (*buf), rate);
            }
            else
            {
                (second ? st.irError2 : st.irError) = err;
                if (second) e->rig->clearImpulseResponse2(); else e->rig->clearImpulseResponse();
            }
            notifyStatus();
        });
    });
    notifyStatus();
}

void RigBank::timerCallback()
{
    auto service = [this] (Entry& e) {
        e.rig->collectGarbage();
        if (e.rig->modelNeedsPrepare.load() && ! e.status.modelLoading && e.status.modelPath.isNotEmpty())
        {
            e.rig->modelNeedsPrepare = false;
            requestModel (e, true);   // reload for the new device rate
        }
    };
    service (liveEntry);
    for (auto& [id, e] : tracks) service (e);
    if (++ticks % 20 == 0) captureAllPluginStates (true);   // only plugins that reported a change
}

void RigBank::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& prop)
{
    if (node.hasType (midiclip::INSTRUMENT))
    {
        if (prop == rig::ids::desc || prop == rig::ids::bypass || prop == rig::ids::auxOutputs) { bool changed = false; syncInstruments (changed); if (changed && onRigsChanged) onRigsChanged(); }
        return;
    }
    if (node.hasType (timeline::ids::TRACK) && prop == timeline::ids::kind) { syncAll(); return; }
    if (node.hasType (rig::ids::PLUGIN))
    {
        if (node.getParent() == live && onLiveChanged) onLiveChanged();   // keep the live rig's plugin state saved
        if (node.getParent().hasType (rig::ids::INSERTS) && (prop == rig::ids::slot || prop == rig::ids::bypass || prop == rig::ids::name || prop == rig::ids::sidechain))
            if (auto it = inserts.find (node.getParent().getParent()[timeline::ids::id].toString()); it != inserts.end())
                syncInserts (it->second);
        return;
    }
    if (! node.hasType (rig::ids::RIG)) return;
    if (node == live)
    {
        syncEntry (liveEntry);
        if (onLiveChanged) onLiveChanged();
        return;
    }
    if (prop == rig::ids::enabled) { syncAll(); return; }
    if (auto* e = entryFor (node)) syncEntry (*e);
}

void RigBank::valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child)
{
    if (child.hasType (midiclip::INSTRUMENT)) { syncAll(); return; }
    if (parent.hasType (rig::ids::INSERTS) || child.hasType (rig::ids::INSERTS)) { syncAll(); return; }
    if (child.hasType (rig::ids::PLUGIN))
    {
        if (parent == live) { syncEntry (liveEntry); if (onLiveChanged) onLiveChanged(); }
        else if (auto* e = entryFor (parent)) syncEntry (*e);
        return;
    }
    if (child.hasType (rig::ids::RIG) || child.hasType (timeline::ids::TRACK) || child.hasType (project::ids::TRACKS)) syncAll();
}

void RigBank::valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (child.hasType (midiclip::INSTRUMENT)) { syncAll(); return; }
    if (parent.hasType (rig::ids::INSERTS) || child.hasType (rig::ids::INSERTS)) { syncAll(); return; }
    if (child.hasType (rig::ids::PLUGIN))
    {
        if (parent == live) { syncEntry (liveEntry); if (onLiveChanged) onLiveChanged(); }
        else if (auto* e = entryFor (parent)) syncEntry (*e);
        return;
    }
    if (child.hasType (rig::ids::RIG) || child.hasType (timeline::ids::TRACK) || child.hasType (project::ids::TRACKS)) syncAll();
}
} // namespace ff::fx
