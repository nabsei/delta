#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

// Delta: a phase-cancellation null-test tool.
//
// Main input  = source A (e.g. the original bounce)
// Sidechain   = source B (e.g. a re-render, a different mix, another plugin's output)
// Output      = A + invert(delay-compensated B)
//
// If A and B are identical (down to sample-accurate timing), the output is
// silence -- a "perfect null". Any audible residual is the actual
// difference between the two sources. Because two renders are almost never
// sample-aligned (even a 1-sample offset destroys a null), Delta finds the
// best alignment automatically via cross-correlation when the user presses
// "Align", then keeps applying that fixed offset until re-triggered.
class DeltaProcessor : public juce::AudioProcessor
{
public:
    DeltaProcessor();
    ~DeltaProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Delta"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Call from the UI thread to request a realignment. Stays pending until
    // a sidechain is actually available and the capture window has filled
    // with real audio (~46ms), so it never runs against mostly-zero data.
    void requestAlign() { alignRequested.store(true); }

    // For the UI: current measured null depth in dB (very negative = deep
    // null = A and B are almost identical; near 0 = no cancellation).
    float getNullDepthDb() const { return nullDepthDb.load(); }
    int   getCurrentOffsetSamples() const { return currentOffsetSamples.load(); }
    bool  hasSidechainSignal() const { return sidechainActive.load(); }

    // True while the background worker is computing a new alignment --
    // the search runs off the audio thread, so this can lag a few
    // milliseconds behind an Align click/request.
    bool isAligning() const { return alignWorkerBusy.load(); }

    // Live spectrogram of the residual (post-alignment difference) signal.
    // The audio thread pushes one column per hop into a lock-free FIFO; the
    // UI thread drains it on a timer. This is the plugin's hero visual --
    // silence (deep null) reads as near-black, real differences light up as
    // colour at the frequencies/times where they actually occur.
    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder;   // 1024
    static constexpr int numBins = fftSize / 2;     // 512
    static constexpr int hopSize = 512;             // ~11.6ms at 44.1kHz, 50% overlap
    static constexpr int columnFifoCapacity = 128;

    // Pops up to maxColumns available spectrum columns (each numBins floats,
    // in dB) into destFlat, oldest first. Returns how many columns were read.
    int readSpectrumColumns(std::vector<float>& destFlat, int maxColumns);

    // Built-in demo signal: generates a synthetic A/B pair internally so the
    // plugin can be tried out (Standalone with no real sidechain routing, or
    // just a quick sanity check) without needing two real audio sources. B
    // is a delayed copy of A's tone plus an extra partial A never has, so
    // Align nulls the fundamental but the added partial stays visible --
    // demonstrating what the tool is actually for.
    void setTestSignalEnabled(bool shouldEnable) { testSignalEnabled.store(shouldEnable); }
    bool isTestSignalEnabled() const { return testSignalEnabled.load(); }

    // Offline file-based comparison: load two files and Delta plays them
    // back on a loop through the same pipeline as live audio (alignment,
    // spectrogram, null depth all work identically). Takes priority over a
    // live sidechain but not over the built-in test signal. Safe to call
    // from the message thread; the audio thread only ever try-locks, so
    // loading a file can never block or glitch audio.
    void loadFileIntoA(const juce::File& file);
    void loadFileIntoB(const juce::File& file);
    void clearFiles();
    bool isFileModeEnabled() const { return fileModeEnabled.load(); }
    juce::String getFileNameA() const { const juce::SpinLock::ScopedLockType sl(fileLock); return fileNameA; }
    juce::String getFileNameB() const { const juce::SpinLock::ScopedLockType sl(fileLock); return fileNameB; }
    juce::String getFilePathA() const { const juce::SpinLock::ScopedLockType sl(fileLock); return filePathA; }
    juce::String getFilePathB() const { const juce::SpinLock::ScopedLockType sl(fileLock); return filePathB; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Circular capture buffers used to run cross-correlation on demand.
    static constexpr int captureLength = 2048;   // ~46ms at 44.1kHz, per channel
    static constexpr int maxLagSamples = 256;    // ~5.8ms at 44.1kHz search range

    std::vector<float> captureA, captureB; // mono downmix capture, interleaved-free
    int captureWritePos = 0;
    bool captureFull = false;

    // Delay line used to time-align sidechain (B) to main input (A).
    juce::AudioBuffer<float> delayLine;
    int delayWritePos = 0;
    int delayLineSize = 0;

    std::atomic<bool> alignRequested { false };
    std::atomic<int> currentOffsetSamples { 0 };
    std::atomic<float> nullDepthDb { -100.0f };
    std::atomic<bool> sidechainActive { false };

    double currentSampleRate = 44100.0;

    std::atomic<bool> testSignalEnabled { false };
    juce::int64 testSampleCounter = 0;

    // The correlation search itself (up to ~(2*maxLagSamples+1) * captureLength
    // double multiply-adds, roughly 1M ops) is too expensive to run inline on
    // the audio thread without risking an audible glitch on whichever block
    // happens to consume the Align request. Instead the audio thread only
    // takes a cheap O(captureLength) snapshot and hands it to a background
    // thread; the search itself never blocks audio processing.
    class AlignWorker : public juce::Thread
    {
    public:
        AlignWorker() : juce::Thread("Delta Align") {}
        void run() override;
        DeltaProcessor* owner = nullptr;
    };
    friend class AlignWorker;

    AlignWorker alignWorker;
    std::atomic<bool> alignWorkerBusy { false };
    std::vector<float> alignSnapshotA, alignSnapshotB; // captureLength each

    void triggerAlignmentSnapshot();
    int computeBestLag(const std::vector<float>& linA, const std::vector<float>& linB) const;

    juce::AudioFormatManager formatManager;
    juce::SpinLock fileLock;
    juce::AudioBuffer<float> fileBufferA, fileBufferB;
    int filePlayheadA = 0, filePlayheadB = 0;
    bool fileALoaded = false, fileBLoaded = false;
    juce::String fileNameA, fileNameB;   // display name only
    juce::String filePathA, filePathB;   // full path, used to restore file mode across save/reload
    std::atomic<bool> fileModeEnabled { false };

    void loadFileInto(const juce::File& file, juce::AudioBuffer<float>& destBuffer,
                       bool& loadedFlag, juce::String& nameOut, juce::String& pathOut, int& playheadOut);

    // Spectrogram analysis state (audio thread only).
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> fftWindow { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> fftInputRing;
    int fftWritePos = 0;
    int samplesSinceHop = 0;

    // Audio-thread-to-UI-thread handoff for spectrogram columns.
    juce::AbstractFifo columnFifo { columnFifoCapacity };
    std::vector<float> columnStorage; // columnFifoCapacity * numBins, flat

    void pushResidualSample(float monoResidual);
    void computeSpectrumColumn();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeltaProcessor)
};
