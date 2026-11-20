// =====================================================================================================  NAM amp + cabinet
// Included by BuiltinPlugins.cpp (inside its anonymous namespace, after Base).
//
// A plugin like the other built-ins: its knobs are parameters and the files it uses (the NAM capture and up to two
// cabinet impulse responses) are properties of its state, so they are saved and restored with the project exactly
// like any plugin's settings. The sound comes from an AmpRig running only its gate, amp, EQ and cabinet blocks.

namespace namamp
{
constexpr int numFiles = 3;   // 0 = NAM capture, 1 = cabinet, 2 = second cabinet
inline const char* fileProp (int k) { return k == 0 ? "namFile" : k == 1 ? "irFile" : "irFile2"; }

const std::array<int, 19>& exposed()
{
    using namespace ff::rig;
    static const std::array<int, 19> p { { inGain, gateOn, gateThresh, gateRelease, namNormalize, namCalibrate, calLevel, ampGain, ampLevel,
                                           bass, mid, treble, presence, cabLowCut, cabHighCut, cabBlend, cabPhase1, cabPhase2, outGain } };
    return p;
}

Layout makeLayout()
{
    Layout l;
    for (int p : exposed())
    {
        const auto& i = ff::rig::info (p);
        juce::String label (i.label);
        if (p == ff::rig::gateOn) label = "Noise gate";
        if (p == ff::rig::gateThresh) label = "Gate threshold";
        if (p == ff::rig::gateRelease) label = "Gate release";
        if (p == ff::rig::namNormalize) label = "Normalise";
        if (p == ff::rig::namCalibrate) label = "Calibrate";
        if (p == ff::rig::ampGain) label = "Gain (no capture)";
        if (p == ff::rig::ampLevel) label = "Amp level";
        if (p == ff::rig::cabLowCut) label = "Cab low cut";
        if (p == ff::rig::cabHighCut) label = "Cab high cut";
        if (p == ff::rig::cabPhase1) label = "Cab 1 phase";
        if (p == ff::rig::cabPhase2) label = "Cab 2 phase";
        if (ff::rig::isToggle (p)) l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { i.id, 1 }, label, i.def >= 0.5f));
        else l.add (param (i.id, label.toRawUTF8(), i.min, i.max, i.def, i.unit, i.centre));
    }
    return l;
}
} // namespace namamp

class NamEditor;

class NamAmp final : public Base, private juce::AsyncUpdater, public juce::ChangeBroadcaster
{
public:
    explicit NamAmp (const Info& i) : Base (i, namamp::makeLayout()), rig (&queue)
    {
        using namespace ff::rig;
        for (int b : { comp, drive, chorus, delay, reverb, plug1, plug2, plug3, plug4 }) rig.setParam (switchFor (b), 0.0f);
        for (int b : { amp, eq, cab }) rig.setParam (switchFor (b), 1.0f);
        for (size_t k = 0; k < namamp::exposed().size(); ++k) cached[k] = state.getParameter (info_ (namamp::exposed()[k]).id);
    }

    ~NamAmp() override
    {
        cancelPendingUpdate();
        pool.removeAllJobs (true, 10000);
    }

    // ---- files (message thread) ----
    struct FileStatus { juce::String path, name, error; bool loading = false, loaded = false; };

    /** Points the plugin at a file (an empty path clears it). Saved with the project. */
    void setFile (int kind, const juce::File& f) { setFilePath (kind, f == juce::File() ? juce::String() : f.getFullPathName()); }
    void setFilePath (int kind, const juce::String& path)
    {
        state.state.setProperty (namamp::fileProp (kind), path, nullptr);
        refreshFiles();
    }
    FileStatus fileStatus (int kind) const
    {
        const juce::ScopedLock sl (mx);
        return slots[(size_t) kind].status;
    }

    // ---- plugin state ----
    void setStateInformation (const void* data, int size) override
    {
        Base::setStateInformation (data, size);
        refreshFiles();
    }

    void prepareToPlay (double sr, int block) override
    {
        const int blk = juce::jmax (block, 512);
        rate = sr;
        maxBlock = blk;
        {
            const juce::ScopedLock sl (installLock);
            rig.prepare (sr, blk);
        }
        rightScratch.assign ((size_t) blk, 0.0f);
        // the capture and cabinets of the saved state are in place before the first audio block
        for (int waited = 0; waited < 5000 && pool.getNumJobs() > 0; waited += 2) juce::Thread::sleep (2);
        installReady();
        {
            const juce::ScopedLock sl (installLock);
            rig.finishLoadingOffline();
        }
    }

    void process (float* L, float* R, int n) noexcept override
    {
        for (size_t k = 0; k < namamp::exposed().size(); ++k)
            if (auto* rp = cached[k]) rig.setParam (namamp::exposed()[k], rp->convertFrom0to1 (rp->getValue()));
        if (R != nullptr) { rig.process (L, R, n); return; }
        for (int done = 0; done < n;)
        {
            const int len = juce::jmin ((int) rightScratch.size(), n - done);
            if (len <= 0) return;
            std::copy (L + done, L + done + len, rightScratch.begin());
            rig.process (L + done, rightScratch.data(), len);
            done += len;
        }
    }

    juce::AudioProcessorEditor* createEditor() override;
    bool modelRunning() const noexcept { return rig.modelActive.load(); }
    float inputPeak() noexcept { return rig.inPeak.exchange (0.0f); }
    float outputPeak() noexcept { return rig.outPeak.exchange (0.0f); }

private:
    friend class NamEditor;

    struct Result
    {
        int kind = 0, gen = 0;
        bool ok = false;
        juce::String error;
        std::unique_ptr<ff::fx::NamModel> model;
        juce::AudioBuffer<float> ir;
        double irRate = 0.0;
    };
    struct Slot { FileStatus status; int gen = 0; };

    static const ff::rig::ParamInfo& info_ (int p) { return ff::rig::info (p); }

    void refreshFiles()
    {
        for (int k = 0; k < namamp::numFiles; ++k)
        {
            const auto path = state.state.getProperty (namamp::fileProp (k)).toString();
            int gen = 0;
            {
                const juce::ScopedLock sl (mx);
                auto& s = slots[(size_t) k];
                if (s.status.path == path) continue;
                s.status = {};
                s.status.path = path;
                s.status.name = path.isEmpty() ? juce::String() : juce::File (path).getFileNameWithoutExtension();
                s.status.loading = path.isNotEmpty();
                gen = ++s.gen;
            }
            if (path.isEmpty())
            {
                const juce::ScopedLock sl (installLock);
                if (k == 0) { rig.clearModel(); setLatencySamples (0); }
                else if (k == 1) rig.clearImpulseResponse();
                else rig.clearImpulseResponse2();
                rig.collectGarbage();
                sendChangeMessage();
                continue;
            }
            pool.addJob ([this, k, path, gen] { loadJob (k, path, gen); });
        }
        sendChangeMessage();
    }

    void loadJob (int k, const juce::String& path, int gen)
    {
        Result r;
        r.kind = k;
        r.gen = gen;
        if (k == 0)
        {
            r.model = ff::fx::NamModel::load (juce::File (path), r.error);
            if (r.model != nullptr) { r.model->prepare (rate.load(), maxBlock.load()); r.ok = true; }
        }
        else r.ok = ff::fx::loadImpulseFile (juce::File (path), r.ir, r.irRate, r.error);
        {
            const juce::ScopedLock sl (mx);
            ready.push_back (std::move (r));
        }
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override { installReady(); }

    /** Hands finished loads to the rig (any thread, one at a time). */
    void installReady()
    {
        const juce::ScopedLock il (installLock);
        std::vector<Result> take;
        {
            const juce::ScopedLock sl (mx);
            take.swap (ready);
        }
        for (auto& r : take)
        {
            {
                const juce::ScopedLock sl (mx);
                auto& s = slots[(size_t) r.kind];
                if (r.gen != s.gen) continue;   // superseded by a newer request
                s.status.loading = false;
                s.status.loaded = r.ok;
                s.status.error = r.ok ? juce::String() : r.error;
            }
            if (r.kind == 0)
            {
                if (r.ok)
                {
                    if (! r.model->isPreparedFor (rate.load(), maxBlock.load())) r.model->prepare (rate.load(), maxBlock.load());
                    setLatencySamples (r.model->latencySamples());
                    rig.setModel (std::move (r.model));
                }
                else { rig.clearModel(); setLatencySamples (0); }
            }
            else if (r.ok)
            {
                if (r.kind == 1) rig.setImpulseResponse (std::move (r.ir), r.irRate);
                else rig.setImpulseResponse2 (std::move (r.ir), r.irRate);
            }
            else
            {
                if (r.kind == 1) rig.clearImpulseResponse(); else rig.clearImpulseResponse2();
            }
        }
        rig.collectGarbage();
        if (! take.empty()) sendChangeMessage();
    }

    juce::dsp::ConvolutionMessageQueue queue { 64 };
    ff::fx::AmpRig rig;
    std::array<juce::RangedAudioParameter*, 19> cached {};
    std::vector<float> rightScratch;
    std::atomic<double> rate { 48000.0 };
    std::atomic<int> maxBlock { 512 };

    mutable juce::CriticalSection mx, installLock;
    std::array<Slot, namamp::numFiles> slots;
    std::vector<Result> ready;
    juce::ThreadPool pool { 1 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (NamAmp)
};

/** The plugin window: what is loaded, buttons to load / browse / clear, and the knobs. */
class NamEditor final : public juce::AudioProcessorEditor, private juce::ChangeListener, private juce::Timer
{
public:
    explicit NamEditor (NamAmp& p) : juce::AudioProcessorEditor (p), amp (p), knobs (p)
    {
        addAndMakeVisible (knobs);
        static const char* titles[3] = { "Amp (NAM capture)", "Cabinet", "Cabinet 2" };
        for (int k = 0; k < 3; ++k)
        {
            auto& row = rows[(size_t) k];
            row.title.setText (titles[k], juce::dontSendNotification);
            row.title.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            row.title.setColour (juce::Label::textColourId, col::dim);
            row.name.setFont (juce::Font (juce::FontOptions (13.0f)));
            row.name.setMinimumHorizontalScale (0.7f);
            row.load.setButtonText ("Load...");
            row.browse.setButtonText ("TONE3000...");
            row.clear.setButtonText ("Clear");
            row.load.onClick = [this, k] { chooseFile (k); };
            row.browse.onClick = [this, k] { browseTone3000 (k); };
            row.clear.onClick = [this, k] { amp.setFilePath (k, {}); };
            for (auto* c : std::initializer_list<juce::Component*> { &row.title, &row.name, &row.load, &row.browse, &row.clear })
                addAndMakeVisible (c);
        }
        amp.addChangeListener (this);
        setSize (juce::jmax (640, knobs.getWidth()), top + knobs.getHeight());
        refresh();
        startTimerHz (4);
    }

    ~NamEditor() override { amp.removeChangeListener (this); }

    void paint (juce::Graphics& g) override { g.fillAll (col::bg); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto head = r.removeFromTop (top).reduced (12, 6);
        const int rowH = (head.getHeight() - 4) / 3;
        for (auto& row : rows)
        {
            auto line = head.removeFromTop (rowH);
            row.title.setBounds (line.removeFromLeft (150));
            row.clear.setBounds (line.removeFromRight (60));
            line.removeFromRight (6);
            row.browse.setBounds (line.removeFromRight (100));
            line.removeFromRight (6);
            row.load.setBounds (line.removeFromRight (80));
            line.removeFromRight (8);
            row.name.setBounds (line);
        }
        knobs.setBounds (r);
    }

private:
    struct Row { juce::Label title, name; juce::TextButton load, browse, clear; };

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override { refresh(); }

    void refresh()
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto s = amp.fileStatus (k);
            auto& n = rows[(size_t) k].name;
            if (s.path.isEmpty())
            {
                n.setText (k == 0 ? "None: using the built-in amp" : k == 1 ? "None: using a built-in speaker" : "None", juce::dontSendNotification);
                n.setColour (juce::Label::textColourId, col::dim);
            }
            else if (s.error.isNotEmpty())
            {
                n.setText (s.error, juce::dontSendNotification);
                n.setColour (juce::Label::textColourId, col::bad);
            }
            else
            {
                n.setText (s.name + (s.loading ? "  (loading...)" : ""), juce::dontSendNotification);
                n.setColour (juce::Label::textColourId, s.loading ? col::dim : col::text);
            }
            rows[(size_t) k].clear.setEnabled (s.path.isNotEmpty());
            rows[(size_t) k].browse.setVisible (static_cast<bool> (tone3000Browser()));
        }
    }

    void chooseFile (int k)
    {
        static juce::File folder = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
        chooser = std::make_shared<juce::FileChooser> (k == 0 ? "Choose a NAM capture" : "Choose a cabinet impulse response", folder,
                                                       k == 0 ? "*.nam" : "*.wav;*.aif;*.aiff;*.flac");
        juce::Component::SafePointer<NamEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, k] (const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File() || safe == nullptr) return;
            folder = f.getParentDirectory();
            safe->amp.setFile (k, f);
        });
    }

    void browseTone3000 (int k)
    {
        if (! tone3000Browser()) return;
        juce::WeakReference<NamAmp> target (&amp);
        tone3000Browser() (k != 0, [target, k] (const juce::File& f, bool isIr) {
            if (target != nullptr) target->setFile (isIr ? (k == 0 ? 1 : k) : 0, f);   // an IR from the amp row goes to the first cabinet
        });
    }

    NamAmp& amp;
    Editor knobs;
    static constexpr int top = 112;
    std::array<Row, 3> rows;
    std::shared_ptr<juce::FileChooser> chooser;
};

juce::AudioProcessorEditor* NamAmp::createEditor() { return new NamEditor (*this); }
