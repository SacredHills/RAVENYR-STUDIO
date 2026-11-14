#include "ProjectDocument.h"
#include "core/MidiClip.h"
#include "core/Timeline.h"

namespace ff
{
using namespace project;

juce::File ProjectDocument::appDataDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("RAVENYR");
    d.createDirectory();
    return d;
}

ProjectDocument::ProjectDocument (EngineHost& h, juce::PropertiesFile& s) : host (h), settings (s)
{
    recent.setMaxNumberOfItems (12);
    recent.restoreFromString (settings.getValue ("recentProjects"));
    autosaveSeconds = settings.getIntValue ("autosaveSeconds", 120);
    newUntitled();
    startTimer (5000);
}

ProjectDocument::~ProjectDocument()
{
    stopTimer();
    alive->store (false);
    waitForAutosave();
    root.removeListener (this);
}

juce::String ProjectDocument::displayName() const
{
    return root[ids::name].toString() + (dirty ? " *" : "");
}

void ProjectDocument::replaceTree (juce::ValueTree newTree, const juce::File& newBundle, bool markAsDirty)
{
    root.removeListener (this);
    root = newTree;
    bundle = newBundle;
    undoManager.clearUndoHistory();
    root.addListener (this);
    dirty = markAsDirty;
    autosaveDirty = markAsDirty;
    lastMap = tempoMap();
    haveLastMap = true;
    pushAllToEngine();
    writeSessionLock();
    if (onTreeReplaced) onTreeReplaced();
    if (onStateChanged) onStateChanged();
}

void ProjectDocument::wipeUntitledMedia()
{
    // The untitled scratch bundle holds media for unsaved projects; only clear it once the
    // user has discarded that project (never at startup, before crash recovery is offered).
    if (sessionStarted)
        untitledRecoveryBundle().getChildFile ("Audio").deleteRecursively();
}

juce::File ProjectDocument::mediaBundle() const
{
    return hasLocation() ? bundle : untitledRecoveryBundle();
}

void ProjectDocument::newUntitled()
{
    wipeUntitledMedia();
    replaceTree (createDefault ("Untitled"), {}, false);
    juce::Logger::writeToLog ("New untitled project");
}

juce::Result ProjectDocument::openBundle (const juce::File& dir)
{
    auto r = Bundle::load (dir);
    if (r.result.failed())
    {
        juce::Logger::writeToLog ("Open failed: " + dir.getFullPathName() + " - " + r.result.getErrorMessage());
        return r.result;
    }
    for (auto& w : r.warnings)
        juce::Logger::writeToLog ("Project repair: " + w);

    replaceTree (r.tree, dir, ! r.warnings.isEmpty());
    addToRecent (dir);
    juce::Logger::writeToLog ("Opened project " + dir.getFullPathName());
    return r.warnings.isEmpty() ? juce::Result::ok()
                                : juce::Result::fail ("The project was repaired while loading:\n" + r.warnings.joinIntoString ("\n"));
}

juce::Result ProjectDocument::saveTo (const juce::File& dirIn)
{
    const auto dir = Bundle::normaliseBundlePath (dirIn);
    const bool renaming = dir != bundle;
    if (renaming && root[ids::name].toString() == "Untitled")
        root.setProperty (ids::name, dir.getFileNameWithoutExtension(), nullptr);

    auto r = Bundle::createStructure (dir);
    if (r.wasOk() && dir != mediaBundle())
        r = copyMediaTo (dir);
    if (r.wasOk())
        r = Bundle::save (root, dir, settings.getIntValue ("backupsToKeep", 10));
    if (r.failed())
    {
        juce::Logger::writeToLog ("Save failed: " + r.getErrorMessage());
        return r;
    }

    if (! hasLocation())
    {
        Bundle::discardAutosave (untitledRecoveryBundle());
        untitledRecoveryBundle().getChildFile ("Audio").deleteRecursively();
    }

    bundle = dir;
    dirty = false;
    autosaveDirty = false;
    addToRecent (dir);
    writeSessionLock();
    juce::Logger::writeToLog ("Saved project " + dir.getFullPathName());
    if (onStateChanged) onStateChanged();
    return r;
}

juce::Result ProjectDocument::copyMediaTo (const juce::File& newBundle)
{
    // Every clip file referenced by the project is copied so the new bundle is self-contained.
    juce::StringArray done, files;
    const auto from = mediaBundle();
    for (const auto& track : root.getChildWithName (ids::TRACKS))
        for (const auto& clip : track)
            files.add (clip["file"].toString());
    // drum kit samples (DRUMS/KIT sampleN properties)
    const auto kit = root.getChildWithName ("DRUMS").getChildWithName ("KIT");
    for (int i = 0; i < kit.getNumProperties(); ++i)
        files.add (kit[kit.getPropertyName (i)].toString());
    for (const auto& rel0 : files)
        {
            const auto rel = rel0;
            if (rel.isEmpty() || done.contains (rel))
                continue;
            done.add (rel);
            const auto src = Bundle::fromPortablePath (from, rel);
            const auto dst = Bundle::fromPortablePath (newBundle, rel);
            if (src == juce::File() || dst == juce::File() || ! src.existsAsFile() || dst.existsAsFile())
                continue;
            dst.getParentDirectory().createDirectory();
            bool copied = src.copyFileTo (dst);
            if (! copied)
            {
                // fall back to a plain stream copy (a locked or oddly-shared source can fail CopyFile)
                dst.deleteFile();
                juce::FileInputStream in (src);
                juce::FileOutputStream out (dst);
                if (in.openedOk() && out.openedOk())
                {
                    out.setPosition (0);
                    out.truncate();
                    copied = out.writeFromInputStream (in, -1) >= 0;
                    out.flush();
                }
            }
            if (! copied)
                return juce::Result::fail ("Could not copy " + src.getFullPathName() + " into the new project folder.");
        }
    return juce::Result::ok();
}

juce::Result ProjectDocument::save()
{
    jassert (hasLocation());
    return saveTo (bundle);
}

void ProjectDocument::set (juce::ValueTree node, const juce::Identifier& id, const juce::var& value, bool undoable)
{
    node.setProperty (id, value, undoable ? &undoManager : nullptr);
}

void ProjectDocument::setTempo (double bpm)
{
    set (transport(), ids::bpm, juce::jlimit (10.0, 500.0, std::round (bpm * 100.0) / 100.0));
}

TempoMap ProjectDocument::tempoMap()
{
    std::vector<TempoMap::Point> pts;
    for (auto c : transport())
        if (c.hasType (ids::TEMPO)) pts.push_back ({ (double) c[ids::beat], (double) c[ids::bpm] });
    TempoMap m;
    m.set (tempo(), std::move (pts));
    return m;
}

void ProjectDocument::addTempoPoint (double quarter, double bpm)
{
    bpm = juce::jlimit (10.0, 500.0, std::round (bpm * 100.0) / 100.0);
    quarter = juce::jmax (0.0, std::round (quarter * 1000.0) / 1000.0);
    if (quarter < 1.0e-6) { setTempo (bpm); return; }   // at the very start it is the project tempo
    undoManager.beginNewTransaction ("Add tempo point");
    juce::ValueTree n (ids::TEMPO);
    n.setProperty (ids::beat, quarter, nullptr);
    n.setProperty (ids::bpm, bpm, nullptr);
    transport().appendChild (n, &undoManager);   // a second point at the same position makes an instant change
}

void ProjectDocument::removeTempoPoint (juce::ValueTree point)
{
    if (! point.isValid() || point.getParent() != transport()) return;
    undoManager.beginNewTransaction ("Delete tempo point");
    transport().removeChild (point, &undoManager);
}

void ProjectDocument::setTimeSignature (int num, int den)
{
    undoManager.beginNewTransaction ("Time signature");
    set (transport(), ids::tsNum, juce::jlimit (1, 16, num));
    set (transport(), ids::tsDen, den);
}

void ProjectDocument::tempoEdited()
{
    const auto now = tempoMap();
    if (haveLastMap && ! undoManager.isPerformingUndoRedo() && ! now.sameAs (lastMap))
        retimeMidi (lastMap, now);   // MIDI clips keep their musical positions, so they follow every tempo change
    lastMap = now;
    haveLastMap = true;
    pushAllToEngine();
}

void ProjectDocument::retimeMidi (const TempoMap& from, const TempoMap& to)
{
    const double fb = from.baseBpm(), tb = to.baseBpm();
    auto f = [&] (double t) { return t < 0.0 ? t * fb / tb : to.secondsAtQuarter (from.quarterAtSeconds (t)); };
    for (auto track : tracks())
        for (auto c : track)
        {
            if (! midiclip::isMidiClip (c)) continue;
            const double cs = (double) c[timeline::ids::start], len = (double) c[timeline::ids::length], off = (double) c[timeline::ids::offset];
            const double ncs = f (cs);
            auto ns = midiclip::notesOf (c);
            auto pd = midiclip::pedalOf (c);
            for (auto& n : ns)
            {
                const double a = cs + n.start - off;
                n.start = f (a) - ncs + off;
                n.length = juce::jmax (1.0e-4, f (a + n.length) - f (a));
            }
            for (auto& p : pd) p.time = f (cs + p.time - off) - ncs + off;
            midiclip::setNotes (c, ns, pd, &undoManager);
            c.setProperty (timeline::ids::start, ncs, &undoManager);
            c.setProperty (timeline::ids::length, f (cs + len) - ncs, &undoManager);
        }
}

void ProjectDocument::pushAllToEngine()
{
    auto& e = host.engine;
    e.setTempo (tempo());
    e.setTimeSignature (tsNum(), tsDen());
    {
        // tempo changes: the click follows the tempo map (beat times for the first two hours of the song)
        const auto map = tempoMap();
        if (map.isConstant()) e.submitClickMap (nullptr);
        else
        {
            auto clicks = std::make_unique<std::vector<TabSequence::Click>>();
            const double beatQ = 4.0 / juce::jmax (1, tsDen());
            const int perBar = juce::jmax (1, tsNum());
            for (int k = 0; k < 40000; ++k)
            {
                const double sec = map.secondsAtQuarter (k * beatQ);
                if (sec > 7200.0) break;
                clicks->push_back ({ sec, k % perBar == 0 });
            }
            e.submitClickMap (std::move (clicks));
        }
    }

    auto m = metronome();
    e.clickWhilePlaying = (bool) m[ids::enabled];
    e.metronome.volume = (float) (double) m[ids::volume];
    e.metronome.subdivision = (int) m[ids::subdivision];
    e.metronome.sound = (int) m[ids::sound];
    e.metronome.accentsEnabled = ! m.hasProperty (ids::accentsOn) || (bool) m[ids::accentsOn];
    auto t = transport();
    e.loopEnabled = (bool) t[ids::loopEnabled];
    e.punchEnabled = (bool) t[ids::punchEnabled];
    e.loopStartSec = (double) t[ids::loopStart];
    e.loopEndSec = (double) t[ids::loopEnd];

    const auto accents = m[ids::accents].toString();
    for (int i = 0; i < MetronomeDsp::maxBeats; ++i)
    {
        const auto c = i < accents.length() ? accents[i] : '1';
        e.metronome.beatLevels[(size_t) i] = juce::jlimit (0, 2, (int) (c - '0'));
    }
}

void ProjectDocument::markDirty()
{
    autosaveDirty = true;
    if (! dirty)
    {
        dirty = true;
        if (onStateChanged) onStateChanged();
    }
}

void ProjectDocument::valueTreePropertyChanged (juce::ValueTree& node, const juce::Identifier& id)
{
    if (node.getType() == ids::TRANSPORT || node.getType() == ids::TEMPO)
        tempoEdited();
    else if (node.getType() == ids::METRONOME)
        pushAllToEngine();
    if (id == ids::name && onStateChanged)
        onStateChanged();
    markDirty();
}

// ------------------------------------------------------------------ autosave / recovery

void ProjectDocument::setAutosaveIntervalSeconds (int seconds)
{
    autosaveSeconds = seconds;
    settings.setValue ("autosaveSeconds", seconds);
}

juce::File ProjectDocument::untitledRecoveryBundle() const
{
    return appDataDir().getChildFile ("Recovery").getChildFile ("Untitled.ravenyr");
}

juce::File ProjectDocument::sessionLockFile() const
{
    return appDataDir().getChildFile ("session.lock");
}

void ProjectDocument::timerCallback()
{
    if (autosaveSeconds <= 0 || ! autosaveDirty)
        return;
    if (lastAutosaveTime != juce::Time() && (juce::Time::getCurrentTime() - lastAutosaveTime).inSeconds() < autosaveSeconds)
        return;

    if (autosaveRunning.load()) return;
    const auto target = hasLocation() ? bundle : untitledRecoveryBundle();
    auto snapshot = root.createCopy();    // a few ms even for big projects; serialising happens off the UI thread
    autosaveDirty = false;
    lastAutosaveTime = juce::Time::getCurrentTime();
    autosaveRunning = true;
    auto token = alive;
    autosaver.addJob ([this, token, snapshot, target]
    {
        auto r = Bundle::createStructure (target);
        if (r.wasOk())
            r = Bundle::writeAutosave (snapshot, target);
        juce::MessageManager::callAsync ([this, token, r]
        {
            if (! token->load()) return;
            autosaveRunning = false;
            if (r.wasOk())
                juce::Logger::writeToLog ("Autosaved");
            else
            {
                autosaveDirty = true;   // try again next time
                juce::Logger::writeToLog ("Autosave failed: " + r.getErrorMessage());
            }
            if (onStateChanged) onStateChanged();
        });
    });
}

void ProjectDocument::writeSessionLock()
{
    if (! sessionStarted)
        return;
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    o->setProperty ("bundle", hasLocation() ? bundle.getFullPathName() : juce::String());
    o->setProperty ("started", juce::Time::getCurrentTime().toISO8601 (true));
    writeFileAtomically (sessionLockFile(), juce::JSON::toString (juce::var (o.get())));
}

void ProjectDocument::endSession()
{
    waitForAutosave();
    wipeUntitledMedia();
    sessionStarted = false;
    Bundle::discardAutosave (untitledRecoveryBundle());
    sessionLockFile().deleteFile();
}

std::optional<ProjectDocument::Recovery> ProjectDocument::findCrashRecovery()
{
    const auto lock = sessionLockFile();
    if (! lock.existsAsFile())
        return std::nullopt;

    const auto v = juce::JSON::parse (lock.loadFileAsString());
    const auto path = v.getProperty ("bundle", {}).toString();

    if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
    {
        const juce::File b (path);
        if (Bundle::hasNewerAutosave (b))
            return Recovery { b, false };
    }
    else if (Bundle::autosaveFile (untitledRecoveryBundle()).existsAsFile())
    {
        return Recovery { untitledRecoveryBundle(), true };
    }
    return std::nullopt;
}

juce::Result ProjectDocument::recoverFrom (const Recovery& rec)
{
    auto r = Bundle::loadAutosave (rec.bundle);
    if (r.result.failed())
        return r.result;
    replaceTree (r.tree, rec.untitled ? juce::File() : rec.bundle, true);
    juce::Logger::writeToLog ("Recovered project from autosave in " + rec.bundle.getFullPathName());
    return juce::Result::ok();
}

void ProjectDocument::addToRecent (const juce::File& f)
{
    recent.addFile (f);
    settings.setValue ("recentProjects", recent.toString());
    settings.saveIfNeeded();
}
} // namespace ff
