#include "Exporter.h"
#include "core/Timeline.h"
#include "core/RigModel.h"
#include "fx/RigBank.h"
#include "core/ProjectFile.h"
#include "GraphMixer.h"

namespace ff
{
namespace
{
    juce::Result writeFile (const juce::File& file, const juce::AudioBuffer<float>& audio, const ExportSettings& s)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> out (file.createOutputStream().release());
        if (out == nullptr)
            return juce::Result::fail ("Cannot create " + file.getFullPathName());

        std::unique_ptr<juce::AudioFormat> fmt;
        if (s.format == ExportSettings::Format::flac) fmt = std::make_unique<juce::FlacAudioFormat>();
        else fmt = std::make_unique<juce::WavAudioFormat>();

        const int bits = (s.format == ExportSettings::Format::flac) ? juce::jmin (24, s.bitDepth) : s.bitDepth;
        std::unique_ptr<juce::AudioFormatWriter> w (fmt->createWriterFor (out.get(), s.sampleRate, (unsigned int) audio.getNumChannels(),
                                                                          bits, {}, 0));
        if (w == nullptr)
            return juce::Result::fail ("The " + fmt->getFormatName() + " writer does not support " + juce::String (bits) + "-bit output.");
        out.release();
        if (! w->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
            return juce::Result::fail ("Writing " + file.getFileName() + " failed (disk full?).");
        return juce::Result::ok();
    }

    void ditherTo16 (juce::AudioBuffer<float>& b)
    {
        juce::Random rnd (0x5eed);
        const float lsb = 1.0f / 32768.0f;
        for (int c = 0; c < b.getNumChannels(); ++c)
        {
            auto* d = b.getWritePointer (c);
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] += (rnd.nextFloat() - rnd.nextFloat()) * lsb; // TPDF
        }
    }
}

ExportResult exportAudio (const juce::ValueTree& tracks, const juce::File& mediaBundle, AudioCache& cache,
                          const ExportSettings& s, std::function<bool (double)> progress)
{
    ExportResult res;
    if (s.endSec <= s.startSec)
    {
        res.result = juce::Result::fail ("Nothing to export: the range is empty (add or record some audio first).");
        return res;
    }
    if (s.sampleRate < 8000.0 || s.sampleRate > 384000.0)
    {
        res.result = juce::Result::fail ("Unsupported sample rate.");
        return res;
    }

    GraphBuildOptions opt;
    opt.sampleRate = s.sampleRate;
    opt.waitForAudio = true;
    juce::StringArray rigWarnings;
    opt.rigFor = [&] (const juce::ValueTree& t) -> std::shared_ptr<fx::AmpRig> {
        const auto r = rig::find (t);
        return rig::isEnabled (r) ? fx::makeOfflineRig (r, s.sampleRate, 4096, rigWarnings) : nullptr;
    };
    opt.insertsFor = [&] (const juce::ValueTree& t) { return fx::makeOfflineInserts (t, s.sampleRate, 4096, rigWarnings); };
    opt.instrumentFor = [&] (const juce::ValueTree& t) { return fx::makeOfflineInstrument (t, s.sampleRate, 4096, rigWarnings); };
    if (const auto master = s.master.isValid() ? s.master : tracks.getParent().getChildWithName (project::ids::MASTER); master.isValid())
        opt.masterInserts = fx::makeOfflineInserts (master, s.sampleRate, 4096, rigWarnings);
    opt.masterNode = s.master.isValid() ? s.master : tracks.getParent().getChildWithName (project::ids::MASTER);
    juce::ScopedNoDenormals noDenormals;
    auto graph = buildPlaybackGraph (tracks, mediaBundle, cache, opt);
    if (graph->missingClips > 0)
        res.warnings.add (juce::String (graph->missingClips) + " clip(s) could not be loaded and are silent in the export.");
    res.warnings.addArray (rigWarnings);

    if (progress && ! progress (0.0))   // cancelled while the plugins and audio were loading
    {
        res.result = juce::Result::fail ("Export cancelled.");
        return res;
    }

    const auto start = (juce::int64) std::llround (s.startSec * s.sampleRate);
    const auto total = (juce::int64) std::llround ((s.endSec - s.startSec) * s.sampleRate);
    if (total > (juce::int64) std::numeric_limits<int>::max())
    {
        res.result = juce::Result::fail ("Export range is too long.");
        return res;
    }

    // Which renders: one mix, or one per track.
    struct Job { juce::String name; std::vector<int> trackIndices; };
    std::vector<Job> jobs;
    if (s.stems)
    {
        int i = 0;
        for (const auto& t : tracks)
        {
            if (t.hasType (timeline::ids::TRACK) && i < (int) graph->tracks.size())
            {
                const auto& gt = graph->tracks[(size_t) i];
                if (gt.audible && ((gt.kind == timeline::TrackKind::audio && ! gt.pieces.empty())
                                   || (gt.kind == timeline::TrackKind::instrument && ! gt.midiEvents.empty())))
                    jobs.push_back ({ juce::File::createLegalFileName (t[timeline::ids::name].toString()), { i } });
                ++i;
            }
        }
        if (jobs.empty())
        {
            res.result = juce::Result::fail ("No audible tracks with audio to export as stems.");
            return res;
        }
        if (! s.destination.createDirectory())
        {
            res.result = juce::Result::fail ("Cannot create folder " + s.destination.getFullPathName());
            return res;
        }
    }
    else
    {
        Job j { s.baseName, {} };
        for (int i = 0; i < (int) graph->tracks.size(); ++i)
            if (graph->tracks[(size_t) i].audible)
                j.trackIndices.push_back (i);
        jobs.push_back (j);
    }

    const int block = 4096;
    std::vector<float> l ((size_t) block), r ((size_t) block);
    juce::StringArray usedNames;
    GraphMixer mixer;
    mixer.prepare (block, s.sampleRate);
    mixer.allowIdle = false;   // offline: always run the effects

    struct Source final : GraphMixer::Source
    {
        const std::vector<char>* include = nullptr;
        juce::int64 at = 0;
        double masterTime() const noexcept override { return (double) at / sampleRate; }
        double sampleRate = 48000.0;
        bool fill (int index, PlayTrack& t, float* L, float* R, int n) noexcept override
        {
            if (! t.lanes.empty() || t.instrument != nullptr)
            {
                BlockTiming bt;
                bt.from = at + readAhead (t);
                bt.rolling = true;
                bt.timeSec = (double) at / sampleRate;
                prepareTrackBlock (t, bt, n);
            }
            if (! t.audible) return false;
            if (! timeline::isSourceKind (t.kind)) return true;   // buses and FX returns play in every render
            if (include != nullptr && ! (*include)[(size_t) index]) return false;
            if (t.kind == timeline::TrackKind::instrument) return true;   // its instrument plays the MIDI
            renderTrackClips (t, at + readAhead (t), n, L, R);   // delay compensation
            return true;
        }
    } source;
    source.sampleRate = s.sampleRate;

    for (size_t ji = 0; ji < jobs.size(); ++ji)
    {
        const auto& job = jobs[ji];
        juce::AudioBuffer<float> out (2, (int) total);
        out.clear();
        std::vector<char> include (graph->tracks.size(), 0);
        for (int ti : job.trackIndices) include[(size_t) ti] = 1;
        source.include = &include;
        graph->masterPrevGain = -1.0f;
        graph->masterPrevPan = -2.0f;
        for (auto& l : graph->masterLanes) l.last = -1.0f;
        for (auto& t : graph->tracks)   // every render starts from silence
        {
            if (t.rig != nullptr) t.rig->reset();
            t.resetState();
        }

        // Run up to the start first, with the output thrown away: plugin instruments and effects that wake up slowly (or
        // report latency) would otherwise lose the first part of the render. Positions before 0 are simply silent.
        juce::int64 preroll = 0;
        bool anyInstrument = false;
        for (const auto& t : graph->tracks)
        {
            preroll = juce::jmax (preroll, (juce::int64) readAhead (t));
            anyInstrument = anyInstrument || t.instrument != nullptr;
        }
        if (graph->masterInserts != nullptr) preroll += graph->masterInserts->latency();
        if (anyInstrument) preroll += (juce::int64) std::llround (0.5 * s.sampleRate);
        const auto renderBlock = [&] (juce::int64 done, int n) {
            std::fill (l.begin(), l.begin() + n, 0.0f);
            std::fill (r.begin(), r.begin() + n, 0.0f);
            source.at = start + done;
            {
                fx::TransportPlayHead::Info ti;
                const double sec = juce::jmax (0.0, (double) (start + done) / s.sampleRate);
                ti.playing = true;
                ti.samples = juce::jmax ((juce::int64) 0, start + done);
                ti.sampleRate = s.sampleRate;
                ti.ppq = s.tempoMap.quarterAtSeconds (sec);
                ti.bpm = s.tempoMap.bpmAtSeconds (sec);
                ti.tsNum = s.tsNum;
                ti.tsDen = s.tsDen;
                fx::transportPlayHead().update (ti);
            }
            mixer.mix (*graph, source, l.data(), r.data(), n);
        };
        for (juce::int64 pos = -preroll; pos < 0;)
        {
            const int n = (int) juce::jmin ((juce::int64) block, -pos);
            renderBlock (pos, n);
            pos += n;
            if (progress && ! progress (0.0)) { res.result = juce::Result::fail ("Export cancelled."); return res; }
        }

        for (juce::int64 done = 0; done < total; done += block)
        {
            const int n = (int) juce::jmin ((juce::int64) block, total - done);
            renderBlock (done, n);
            out.addFrom (0, (int) done, l.data(), n);
            out.addFrom (1, (int) done, r.data(), n);
            if (progress && ! progress (((double) ji + (double) done / (double) total) / (double) jobs.size()))
            {
                res.result = juce::Result::fail ("Export cancelled.");
                return res;
            }
        }

        const float peak = out.getMagnitude (0, out.getNumSamples());
        if (s.normalize && peak > 0.0f)
            out.applyGain (juce::Decibels::decibelsToGain ((float) s.normalizeDb) / peak);
        else if (peak > 1.0f && s.bitDepth != 32)
            res.warnings.add (job.name + ": peaks reach " + juce::String (juce::Decibels::gainToDecibels (peak), 1)
                              + " dBFS and will clip. Enable normalisation or lower the faders.");
        if (s.bitDepth == 16)
            ditherTo16 (out);

        juce::File file;
        if (s.stems)
        {
            auto name = s.baseName + " - " + job.name;
            while (usedNames.contains (name)) name += "_";
            usedNames.add (name);
            file = s.destination.getChildFile (name + s.extension());
        }
        else
        {
            file = s.destination.withFileExtension (s.extension());
        }

        auto wr = writeFile (file, out, s);
        if (wr.failed())
        {
            res.result = wr;
            return res;
        }
        res.files.add (file);
    }
    if (progress) progress (1.0);
    return res;
}
} // namespace ff
