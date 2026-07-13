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

    delayLineSize = juce::jmax(8192, 2 * maxLagSamples + samplesPerBlock * 4);
    delayLine.setSize(4, delayLineSize);
    delayLine.clear();
    delayWritePos = 0;

    captureA.assign((size_t) captureLength, 0.0f);
    captureB.assign((size_t) captureLength, 0.0f);
    captureWritePos = 0;
    captureFull = false;

    currentOffsetSamples.store(0);
    nullDepthDb.store(-100.0f);
    sidechainActive.store(false);

    fftInputRing.assign((size_t) fftSize, 0.0f);
    fftWritePos = 0;
    samplesSinceHop = 0;
    columnStorage.assign((size_t) columnFifoCapacity * (size_t) numBins, 0.0f);
    columnFifo.reset();

    testSampleCounter = 0;
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

    destFlat.resize((size_t) toRead * (size_t) numBins);
    if (size1 > 0)
        std::copy(columnStorage.begin() + (size_t) start1 * (size_t) numBins,
                   columnStorage.begin() + (size_t) (start1 + size1) * (size_t) numBins,
                   destFlat.begin());
    if (size2 > 0)
        std::copy(columnStorage.begin() + (size_t) start2 * (size_t) numBins,
                   columnStorage.begin() + (size_t) (start2 + size2) * (size_t) numBins,
                   destFlat.begin() + (size_t) size1 * (size_t) numBins);

    columnFifo.finishedRead(size1 + size2);
    return toRead;
}

void DeltaProcessor::runAlignment()
{
    // Linearize the two circular capture buffers into chronological order.
    std::vector<float> linA((size_t) captureLength), linB((size_t) captureLength);
    for (int i = 0; i < captureLength; ++i)
    {
        linA[(size_t) i] = captureA[(size_t) ((captureWritePos + i) % captureLength)];
        linB[(size_t) i] = captureB[(size_t) ((captureWritePos + i) % captureLength)];
    }

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

    currentOffsetSamples.store(bestLag);
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

    if (alignRequested.exchange(false) && sideAvailable)
        runAlignment();

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

        pushResidualSample(0.5f * (outL + outR));

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
        apvts.replaceState(state);
    }
}
