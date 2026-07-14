#include "DeltaProcessor.h"
#include "PluginEditor.h"
#include <cmath>
#include <algorithm>

DeltaProcessor::DeltaProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", createLayout())
{
    formatManager.registerBasicFormats();
    alignWorker.owner = this;
    alignWorker.startThread();
}

DeltaProcessor::~DeltaProcessor()
{
    alignWorker.stopThread(2000);
}

juce::AudioProcessorValueTreeState::ParameterLayout DeltaProcessor::createLayout()
{
    return {};
}

bool DeltaProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    auto sidechain = layouts.getChannelSet(true, 1);
    if (! sidechain.isDisabled() && sidechain != juce::AudioChannelSet::stereo())
        return false;

    return true;
}

void DeltaProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    setRateAndBufferSizeDetails(sampleRate, samplesPerBlock);
    currentSampleRate = sampleRate;

    // The A path is always delayed by a fixed maxLagSamples before being
    // combined with the (variably-delayed) B path -- report that as the
    // plugin's processing latency so hosts can compensate for it (PDC).
    // Without this, a "sample-accurate" alignment tool would silently
    // introduce an unreported timing offset of its own.
    setLatencySamples(maxLagSamples);

    delayLineSize = juce::jmax(8192, 2 * maxLagSamples + samplesPerBlock * 4);
    delayLine.setSize(4, delayLineSize);
    delayLine.clear();
    delayWritePos = 0;

    // captureA/B, fftInputRing, columnStorage and alignSnapshotA/B are all
    // sized from compile-time constants, so they never need to change size
    // across repeated prepareToPlay() calls. Allocating them only once (and
    // just resetting the read/write position counters on every call) means
    // their underlying buffers never move -- which matters because
    // columnStorage is read by the UI thread and alignSnapshotA/B are read
    // by the background AlignWorker, neither of which is covered by JUCE's
    // processBlock/prepareToPlay mutual-exclusion guarantee. Reassigning
    // (and potentially reallocating) them here while either thread might
    // still be reading would be a use-after-free.
    if (captureA.empty()) captureA.assign((size_t) captureLength, 0.0f);
    if (captureB.empty()) captureB.assign((size_t) captureLength, 0.0f);
    captureWritePos = 0;
    captureFull = false;

    currentOffsetSamples.store(0);
    nullDepthDb.store(-100.0f);
    sidechainActive.store(false);

    if (fftInputRing.empty()) fftInputRing.assign((size_t) fftSize, 0.0f);
    fftWritePos = 0;
    samplesSinceHop = 0;
    if (columnStorage.empty())
        columnStorage.assign((size_t) columnFifoCapacity * (size_t) numBins, 0.0f);
    columnFifo.reset();

    testSampleCounter = 0;

    if (alignSnapshotA.empty()) alignSnapshotA.assign((size_t) captureLength, 0.0f);
    if (alignSnapshotB.empty()) alignSnapshotB.assign((size_t) captureLength, 0.0f);
    // Do NOT reset alignWorkerBusy here: if the worker is genuinely still
    // computing an alignment from before this prepareToPlay() call, forcing
    // it back to false would let the audio thread hand off a new snapshot
    // while the worker is still reading the old one.
}

void DeltaProcessor::releaseResources() {}

void DeltaProcessor::computeSpectrumColumn()
{
    std::vector<float> fftData((size_t) fftSize * 2, 0.0f);
    for (int i = 0; i < fftSize; ++i)
        fftData[(size_t) i] = fftInputRing[(size_t) ((fftWritePos + i) % fftSize)];

    fftWindow.multiplyWithWindowingTable(fftData.data(), (size_t) fftSize);
    fft.performFrequencyOnlyForwardTransform(fftData.data());

    int start1, size1, start2, size2;
    columnFifo.prepareToWrite(1, start1, size1, start2, size2);
    if (size1 > 0)
    {
        float* dest = columnStorage.data() + (size_t) start1 * (size_t) numBins;
        for (int b = 0; b < numBins; ++b)
        {
            float mag = fftData[(size_t) b] / (float) fftSize;
            dest[b] = juce::Decibels::gainToDecibels(mag, -100.0f);
        }
    }
    columnFifo.finishedWrite(size1); // if the FIFO is full, size1 is 0 and we just drop this column
}

void DeltaProcessor::pushResidualSample(float monoResidual)
{
    fftInputRing[(size_t) fftWritePos] = monoResidual;
    fftWritePos = (fftWritePos + 1) % fftSize;
    if (++samplesSinceHop >= hopSize)
    {
        samplesSinceHop -= hopSize;
        computeSpectrumColumn();
    }
}

int DeltaProcessor::readSpectrumColumns(std::vector<float>& destFlat, int maxColumns)
{
    int available = columnFifo.getNumReady();
    int toRead = juce::jmin(available, maxColumns);
    if (toRead <= 0)
        return 0;

    int start1, size1, start2, size2;
    columnFifo.prepareToRead(toRead, start1, size1, start2, size2);

    using diff_t = std::vector<float>::difference_type;
    destFlat.resize((size_t) toRead * (size_t) numBins);
    if (size1 > 0)
        std::copy(columnStorage.begin() + (diff_t) start1 * (diff_t) numBins,
                   columnStorage.begin() + (diff_t) (start1 + size1) * (diff_t) numBins,
                   destFlat.begin());
    if (size2 > 0)
        std::copy(columnStorage.begin() + (diff_t) start2 * (diff_t) numBins,
                   columnStorage.begin() + (diff_t) (start2 + size2) * (diff_t) numBins,
                   destFlat.begin() + (diff_t) size1 * (diff_t) numBins);

    columnFifo.finishedRead(size1 + size2);
    return toRead;
}

// Pure search: given two linearized (chronological-order) windows, find the
// lag that maximizes normalized cross-correlation. No side effects, safe to
// call from any thread -- this is the expensive O(maxLag * captureLength)
// part that must never run on the audio thread.
int DeltaProcessor::computeBestLag(const std::vector<float>& linA, const std::vector<float>& linB) const
{
    double bestScore = -1.0e30;
    int bestLag = 0;

    // For shift s: compare linA[n] against linB[n - s] over their overlap.
    // s > 0 means B arrived earlier than A (B needs more delay).
    // s < 0 means B arrived later than A (A effectively needs more delay,
    // which we achieve by giving B less delay than the fixed baseline).
    for (int s = -maxLagSamples; s <= maxLagSamples; ++s)
    {
        int start = juce::jmax(0, s);
        int end = juce::jmin(captureLength, captureLength + s);
        if (end - start < captureLength / 2)
            continue;

        double sumProd = 0.0, sumA2 = 0.0, sumB2 = 0.0;
        for (int n = start; n < end; ++n)
        {
            double av = linA[(size_t) n];
            double bv = linB[(size_t) (n - s)];
            sumProd += av * bv;
            sumA2 += av * av;
            sumB2 += bv * bv;
        }

        double denom = std::sqrt(sumA2 * sumB2) + 1.0e-12;
        double score = sumProd / denom;

        if (score > bestScore)
        {
            bestScore = score;
            bestLag = s;
        }
    }

    return bestLag;
}

// Audio thread: cheap O(captureLength) linearize-and-copy into the snapshot
// buffers, then hand off to the background worker. Never runs the actual
// search inline.
void DeltaProcessor::triggerAlignmentSnapshot()
{
    for (int i = 0; i < captureLength; ++i)
    {
        alignSnapshotA[(size_t) i] = captureA[(size_t) ((captureWritePos + i) % captureLength)];
        alignSnapshotB[(size_t) i] = captureB[(size_t) ((captureWritePos + i) % captureLength)];
    }
    alignWorkerBusy.store(true);
    alignWorker.notify();
}

void DeltaProcessor::AlignWorker::run()
{
    while (! threadShouldExit())
    {
        wait(-1);
        if (threadShouldExit())
            break;
        if (owner == nullptr)
            continue;

        int bestLag = owner->computeBestLag(owner->alignSnapshotA, owner->alignSnapshotB);
        owner->currentOffsetSamples.store(bestLag);
        owner->alignWorkerBusy.store(false);
    }
}

void DeltaProcessor::loadFileInto(const juce::File& file, juce::AudioBuffer<float>& destBuffer,
                                   bool& loadedFlag, juce::String& nameOut, juce::String& pathOut, int& playheadOut)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(file));
    if (reader == nullptr)
        return;

    // Validate everything the arithmetic below depends on before touching
    // it -- a corrupted/malformed file could otherwise report a zero or
    // negative sample rate (division by zero -> +/-inf -> casting that to
    // int is undefined behaviour) or zero channels (out-of-bounds channel
    // access on copyFrom).
    if (reader->sampleRate <= 0.0 || reader->numChannels < 1)
        return;

    // Cap how much audio we'll pull into memory for file mode -- a user
    // accidentally picking a multi-hour file shouldn't be able to exhaust
    // memory. 30 minutes at any reasonable sample rate is far more than
    // this tool is meant for (it loops the file, it doesn't need the whole
    // thing to make its point).
    constexpr juce::int64 maxSamples = (juce::int64) (30 * 60 * 192000);
    juce::int64 lengthSamples = juce::jmin(reader->lengthInSamples, maxSamples);
    const int numSamples = (int) lengthSamples;
    if (numSamples <= 0)
        return;

    juce::AudioBuffer<float> raw((int) reader->numChannels, numSamples);
    reader->read(&raw, 0, numSamples, 0, true, true);

    juce::AudioBuffer<float> stereo(2, numSamples);
    if (raw.getNumChannels() >= 2)
    {
        stereo.copyFrom(0, 0, raw, 0, 0, numSamples);
        stereo.copyFrom(1, 0, raw, 1, 0, numSamples);
    }
    else
    {
        stereo.copyFrom(0, 0, raw, 0, 0, numSamples);
        stereo.copyFrom(1, 0, raw, 0, 0, numSamples);
    }

    // Resample to the current project rate if needed, so file-mode content
    // aligns with the same time axis the rest of the pipeline assumes.
    if (currentSampleRate > 0.0 && std::abs(reader->sampleRate - currentSampleRate) > 0.5)
    {
        double ratio = reader->sampleRate / currentSampleRate;
        int outLen = juce::jmax(1, (int) std::ceil((double) numSamples / ratio));
        juce::AudioBuffer<float> resampled(2, outLen);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.reset();
            interp.process(ratio, stereo.getReadPointer(ch), resampled.getWritePointer(ch), outLen);
        }
        stereo = std::move(resampled);
    }

    {
        // Buffer, flag, name and playhead are all swapped in together under
        // the same lock -- the audio thread's try-locked read in
        // processBlock() must never see a new buffer paired with a stale
        // (out-of-range) playhead from the previous file, or vice versa.
        const juce::SpinLock::ScopedLockType sl(fileLock);
        destBuffer = std::move(stereo);
        loadedFlag = true;
        nameOut = file.getFileName();
        pathOut = file.getFullPathName();
        playheadOut = 0;
    }
}

void DeltaProcessor::loadFileIntoA(const juce::File& file)
{
    loadFileInto(file, fileBufferA, fileALoaded, fileNameA, filePathA, filePlayheadA);
    fileModeEnabled.store(fileALoaded && fileBLoaded);
}

void DeltaProcessor::loadFileIntoB(const juce::File& file)
{
    loadFileInto(file, fileBufferB, fileBLoaded, fileNameB, filePathB, filePlayheadB);
    fileModeEnabled.store(fileALoaded && fileBLoaded);
}

void DeltaProcessor::clearFiles()
{
    fileModeEnabled.store(false);
    const juce::SpinLock::ScopedLockType sl(fileLock);
    fileALoaded = false;
    fileBLoaded = false;
    fileNameA = {};
    fileNameB = {};
    filePathA = {};
    filePathB = {};
}

void DeltaProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();

    auto mainIn = getBusBuffer(buffer, true, 0);
    auto mainOut = getBusBuffer(buffer, false, 0);

    bool sideAvailable = false;
    juce::AudioBuffer<float> sideIn;
    if (auto* scBus = getBus(true, 1))
    {
        if (scBus->isEnabled())
        {
            auto view = getBusBuffer(buffer, true, 1);
            if (view.getNumChannels() >= 2)
            {
                sideIn = view;
                sideAvailable = true;
            }
        }
    }
    // Snapshot A and B locally before we start writing to the output bus,
    // since the main output bus aliases the main input channels in-place.
    juce::AudioBuffer<float> localA(2, numSamples);
    juce::AudioBuffer<float> localB(2, numSamples);
    localA.copyFrom(0, 0, mainIn, 0, 0, numSamples);
    localA.copyFrom(1, 0, mainIn, 1, 0, numSamples);
    if (sideAvailable)
    {
        localB.copyFrom(0, 0, sideIn, 0, 0, numSamples);
        localB.copyFrom(1, 0, sideIn, 1, 0, numSamples);
    }
    else
    {
        localB.clear();
    }

    // Offline file comparison: takes priority over a live sidechain (loading
    // files is a deliberate action), but the built-in test signal still
    // wins if both happen to be active. The audio thread only ever
    // try-locks fileLock, so a file load in progress on the message thread
    // can never block or glitch audio -- it just skips file mode for that
    // one block and picks it up again next block.
    if (fileModeEnabled.load())
    {
        const juce::SpinLock::ScopedTryLockType stl(fileLock);
        if (stl.isLocked() && fileALoaded && fileBLoaded
            && fileBufferA.getNumSamples() > 0 && fileBufferB.getNumSamples() > 0)
        {
            sideAvailable = true;
            for (int i = 0; i < numSamples; ++i)
            {
                localA.setSample(0, i, fileBufferA.getSample(0, filePlayheadA));
                localA.setSample(1, i, fileBufferA.getSample(1, filePlayheadA));
                localB.setSample(0, i, fileBufferB.getSample(0, filePlayheadB));
                localB.setSample(1, i, fileBufferB.getSample(1, filePlayheadB));
                filePlayheadA = (filePlayheadA + 1) % fileBufferA.getNumSamples();
                filePlayheadB = (filePlayheadB + 1) % fileBufferB.getNumSamples();
            }
        }
    }

    if (testSignalEnabled.load())
    {
        sideAvailable = true;
        constexpr double freqA = 440.0;
        constexpr double freqExtra = 3300.0;
        constexpr int testDelay = 13;
        constexpr double twoPi = juce::MathConstants<double>::twoPi;

        for (int i = 0; i < numSamples; ++i)
        {
            juce::int64 n = testSampleCounter + i;
            double a = 0.4 * std::sin(twoPi * freqA * (double) n / currentSampleRate);

            juce::int64 nB = n - testDelay;
            double bFund = nB >= 0 ? 0.4 * std::sin(twoPi * freqA * (double) nB / currentSampleRate) : 0.0;
            double bExtra = 0.15 * std::sin(twoPi * freqExtra * (double) n / currentSampleRate);

            float af = (float) a;
            float bf = (float) (bFund + bExtra);
            localA.setSample(0, i, af);
            localA.setSample(1, i, af);
            localB.setSample(0, i, bf);
            localB.setSample(1, i, bf);
        }
        testSampleCounter += numSamples;
    }

    sidechainActive.store(sideAvailable);

    // Feed the mono downmix capture ring buffers (used only for alignment).
    for (int i = 0; i < numSamples; ++i)
    {
        float aMono = 0.5f * (localA.getSample(0, i) + localA.getSample(1, i));
        float bMono = 0.5f * (localB.getSample(0, i) + localB.getSample(1, i));
        captureA[(size_t) captureWritePos] = aMono;
        captureB[(size_t) captureWritePos] = bMono;
        captureWritePos = (captureWritePos + 1) % captureLength;
        if (captureWritePos == 0)
            captureFull = true;
    }

    // Only hand off the Align request once there's a sidechain, the capture
    // buffers hold a full window of real audio, and the background worker
    // isn't already mid-search (so we never overwrite its snapshot while
    // it's reading it). The actual correlation search runs on AlignWorker,
    // never inline here.
    if (alignRequested.load() && sideAvailable && captureFull && ! alignWorkerBusy.load())
    {
        alignRequested.store(false);
        triggerAlignmentSnapshot();
    }

    const int baseLatency = maxLagSamples;
    int bDelay = juce::jlimit(0, delayLineSize - 1, baseLatency + currentOffsetSamples.load());

    double sumSqResidual = 0.0, sumSqA = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        delayLine.setSample(0, delayWritePos, localA.getSample(0, i));
        delayLine.setSample(1, delayWritePos, localA.getSample(1, i));
        delayLine.setSample(2, delayWritePos, localB.getSample(0, i));
        delayLine.setSample(3, delayWritePos, localB.getSample(1, i));

        int readA = (delayWritePos - baseLatency + delayLineSize) % delayLineSize;
        int readB = (delayWritePos - bDelay + delayLineSize) % delayLineSize;

        float aL = delayLine.getSample(0, readA);
        float aR = delayLine.getSample(1, readA);
        float bL = delayLine.getSample(2, readB);
        float bR = delayLine.getSample(3, readB);

        float outL = aL - bL;
        float outR = aR - bR;

        if (mainOut.getNumChannels() > 0) mainOut.setSample(0, i, outL);
        if (mainOut.getNumChannels() > 1) mainOut.setSample(1, i, outR);

        sumSqResidual += (double) outL * outL + (double) outR * outR;
        sumSqA += (double) aL * aL + (double) aR * aR;

        // Only feed the spectrogram when there's an actual comparison
        // happening -- otherwise it would light up with the main input's
        // own content while the HUD still reads "NO SIDECHAIN / -inf dB",
        // which looks like a contradiction.
        pushResidualSample(sideAvailable ? 0.5f * (outL + outR) : 0.0f);

        delayWritePos = (delayWritePos + 1) % delayLineSize;
    }

    if (sideAvailable)
    {
        double rmsResidual = std::sqrt(sumSqResidual / (2.0 * numSamples) + 1.0e-24);
        double rmsA = std::sqrt(sumSqA / (2.0 * numSamples) + 1.0e-24);
        double ratio = rmsResidual / (rmsA + 1.0e-12);
        float instDb = (float) (20.0 * std::log10(ratio + 1.0e-12));
        instDb = juce::jlimit(-100.0f, 12.0f, instDb);

        // One-pole smoothing so the readout doesn't flicker block-to-block.
        float prev = nullDepthDb.load();
        float smoothed = prev + 0.15f * (instDb - prev);
        nullDepthDb.store(smoothed);
    }
    else
    {
        nullDepthDb.store(-100.0f);
    }
}

juce::AudioProcessorEditor* DeltaProcessor::createEditor()
{
    return new DeltaEditor(*this);
}

void DeltaProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty("offset", currentOffsetSamples.load(), nullptr);
    state.setProperty("testSignal", testSignalEnabled.load(), nullptr);

    if (fileModeEnabled.load())
    {
        const juce::SpinLock::ScopedLockType sl(fileLock);
        state.setProperty("filePathA", filePathA, nullptr);
        state.setProperty("filePathB", filePathB, nullptr);
    }

    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void DeltaProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(apvts.state.getType()))
    {
        auto state = juce::ValueTree::fromXml(*xml);
        currentOffsetSamples.store((int) state.getProperty("offset", 0));
        testSignalEnabled.store((bool) state.getProperty("testSignal", false));
        apvts.replaceState(state);

        // Restore file-comparison mode if both files this session was
        // saved with still exist -- otherwise silently fall back to live
        // mode rather than fail the whole state load.
        juce::String savedPathA = state.getProperty("filePathA", "");
        juce::String savedPathB = state.getProperty("filePathB", "");
        if (savedPathA.isNotEmpty() && savedPathB.isNotEmpty())
        {
            juce::File fa(savedPathA), fb(savedPathB);
            if (fa.existsAsFile()) loadFileIntoA(fa);
            if (fb.existsAsFile()) loadFileIntoB(fb);
        }
    }
}
