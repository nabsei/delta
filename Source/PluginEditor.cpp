#include "PluginEditor.h"
#include <cmath>

namespace
{
    constexpr double freqAxisMinHz = 40.0;

    double freqFromRow(int y, int imgH, double sampleRate)
    {
        double fMax = sampleRate * 0.5;
        double frac = imgH > 1 ? (double) (imgH - 1 - y) / (double) (imgH - 1) : 0.0;
        return freqAxisMinHz * std::pow(fMax / freqAxisMinHz, frac);
    }

    double rowFromFreq(double freqHz, int imgH, double sampleRate)
    {
        double fMax = sampleRate * 0.5;
        double frac = std::log(freqHz / freqAxisMinHz) / std::log(fMax / freqAxisMinHz);
        frac = juce::jlimit(0.0, 1.0, frac);
        return (double) (imgH - 1) * (1.0 - frac);
    }

    juce::String freqLabel(double f)
    {
        if (f >= 1000.0)
            return juce::String(f / 1000.0, f >= 10000.0 ? 0 : 1) + "k";
        return juce::String((int) f);
    }

    juce::String formatDb(float db)
    {
        if (db <= -99.5f) return "-inf dB";
        return juce::String(db, 1) + " dB";
    }

    juce::String statusFor(float db, bool sidechainPresent)
    {
        if (! sidechainPresent) return "NO SIDECHAIN";
        if (db <= -40.0f) return "DEEP NULL";
        if (db <= -18.0f) return "PARTIAL MATCH";
        if (db <= -6.0f) return "AUDIBLE DIFF";
        return "NO MATCH";
    }

    // Single-hue "thermal" heatmap: silence reads as black, energy climbs
    // through amber, and only the hottest peaks reach white.
    float magnitudeToIntensity(float db)
    {
        return juce::jlimit(0.0f, 1.0f, (db + 90.0f) / 80.0f); // -90..-10 -> 0..1
    }

    juce::Colour colourForIntensity(float t)
    {
        if (t < 0.7f)
            return DeltaLookAndFeel::bg.interpolatedWith(DeltaLookAndFeel::amber, t / 0.7f);
        return DeltaLookAndFeel::amber.interpolatedWith(juce::Colours::white, (t - 0.7f) / 0.3f);
    }

    constexpr double introDurationMs = 550.0;
}

DeltaEditor::DeltaEditor(DeltaProcessor& p)
    : juce::AudioProcessorEditor(&p), processorRef(p)
{
    setLookAndFeel(&lookAndFeel);

    testSignalButton.setClickingTogglesState(true);
    testSignalButton.setToggleState(processorRef.isTestSignalEnabled(), juce::dontSendNotification);
    testSignalButton.onClick = [this] { processorRef.setTestSignalEnabled(testSignalButton.getToggleState()); };
    addAndMakeVisible(testSignalButton);

    alignButton.onClick = [this] { processorRef.requestAlign(); };
    addAndMakeVisible(alignButton);

    loadAButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load source A...", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc)
                                  {
                                      auto file = fc.getResult();
                                      if (file.existsAsFile())
                                          processorRef.loadFileIntoA(file);
                                  });
    };
    addAndMakeVisible(loadAButton);

    loadBButton.onClick = [this]
    {
        fileChooser = std::make_unique<juce::FileChooser>(
            "Load source B...", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this](const juce::FileChooser& fc)
                                  {
                                      auto file = fc.getResult();
                                      if (file.existsAsFile())
                                          processorRef.loadFileIntoB(file);
                                  });
    };
    addAndMakeVisible(loadBButton);

    clearFilesButton.onClick = [this] { processorRef.clearFiles(); };
    addAndMakeVisible(clearFilesButton);

    setSize(640, 420);
    setResizable(true, true);
    setResizeLimits(520, 340, 1600, 1000);

    introStartMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz(30);
}

DeltaEditor::~DeltaEditor()
{
    setLookAndFeel(nullptr);
}

void DeltaEditor::pushSpectrumColumns()
{
    if (! spectrogramImage.isValid())
        return;

    int numCols = processorRef.readSpectrumColumns(spectrumColumnScratch, 32);
    if (numCols <= 0)
        return;

    double sampleRate = processorRef.getSampleRate();
    if (sampleRate <= 0.0) sampleRate = 44100.0;

    const int imgH = spectrogramImage.getHeight();
    const int imgW = spectrogramImage.getWidth();

    // Phosphor-style glow: each column is rendered sharp, then a small
    // vertical blur of its own intensity is added back additively as a
    // soft halo around bright pixels -- cheap (O(imgH) per column, done
    // once at write time) since it never touches the rest of the image.
    static constexpr float kernel[5] = { 0.06f, 0.25f, 0.38f, 0.25f, 0.06f };
    std::vector<float> tVals((size_t) imgH);
    std::vector<float> blurredT((size_t) imgH);

    juce::Image::BitmapData bitmap(spectrogramImage, juce::Image::BitmapData::writeOnly);

    for (int c = 0; c < numCols; ++c)
    {
        const float* col = spectrumColumnScratch.data() + (size_t) c * (size_t) DeltaProcessor::numBins;

        for (int y = 0; y < imgH; ++y)
        {
            double freq = freqFromRow(y, imgH, sampleRate);
            int bin = juce::jlimit(0, DeltaProcessor::numBins - 1,
                                    (int) (freq * (double) DeltaProcessor::fftSize / sampleRate));
            tVals[(size_t) y] = magnitudeToIntensity(col[(size_t) bin]);
        }

        for (int y = 0; y < imgH; ++y)
        {
            float acc = 0.0f, wsum = 0.0f;
            for (int k = -2; k <= 2; ++k)
            {
                int yy = y + k;
                if (yy < 0 || yy >= imgH) continue;
                acc += tVals[(size_t) yy] * kernel[k + 2];
                wsum += kernel[k + 2];
            }
            blurredT[(size_t) y] = wsum > 0.0f ? acc / wsum : 0.0f;
        }

        for (int y = 0; y < imgH; ++y)
        {
            juce::Colour core = colourForIntensity(tVals[(size_t) y]);
            // The glow strength, decay curve and colour grading used in the
            // shipped/tested build are tuned against reference material and
            // are not reproduced in this public version.
            float glowAmount = blurredT[(size_t) y] * 0.3f;

            float r = juce::jlimit(0.0f, 1.0f, core.getFloatRed() + DeltaLookAndFeel::amber.getFloatRed() * glowAmount);
            float g2 = juce::jlimit(0.0f, 1.0f, core.getFloatGreen() + DeltaLookAndFeel::amber.getFloatGreen() * glowAmount);
            float b = juce::jlimit(0.0f, 1.0f, core.getFloatBlue() + DeltaLookAndFeel::amber.getFloatBlue() * glowAmount);

            bitmap.setPixelColour(spectrogramWriteX, y, juce::Colour::fromFloatRGBA(r, g2, b, 1.0f));
        }

        spectrogramWriteX = (spectrogramWriteX + 1) % imgW;

        if (c == numCols - 1 && currentRowIntensity.size() == (size_t) imgH)
            currentRowIntensity = tVals;
    }
}

void DeltaEditor::timerCallback()
{
    currentSidechainPresent = processorRef.hasSidechainSignal();
    currentDb = processorRef.getNullDepthDb();
    currentOffsetSamples = processorRef.getCurrentOffsetSamples();
    currentFileModeEnabled = processorRef.isFileModeEnabled();
    if (currentFileModeEnabled)
    {
        currentFileNameA = processorRef.getFileNameA();
        currentFileNameB = processorRef.getFileNameB();
    }

    pushSpectrumColumns();

    // Slow-decaying peak-hold: each row remembers its recent maximum and
    // eases back down, ~1-2 seconds to fully decay at this tick rate.
    constexpr float peakDecayFactor = 0.93f;
    for (size_t y = 0; y < peakHoldT.size(); ++y)
    {
        float current = y < currentRowIntensity.size() ? currentRowIntensity[y] : 0.0f;
        peakHoldT[y] = juce::jmax(peakHoldT[y] * peakDecayFactor, current);
    }

    repaint();
}

void DeltaEditor::paint(juce::Graphics& g)
{
    g.fillAll(DeltaLookAndFeel::bg);

    double sampleRate = processorRef.getSampleRate();
    if (sampleRate <= 0.0) sampleRate = 44100.0;

    // Spectrogram, full-bleed, drawn as a scrolling ring-buffer image split
    // at the current write cursor so the newest column is always at the
    // right edge.
    if (spectrogramImage.isValid())
    {
        const int w = spectrogramImage.getWidth();
        const int leftWidth = w - spectrogramWriteX;

        if (leftWidth > 0)
            g.drawImage(spectrogramImage,
                        spectrogramBounds.getX(), spectrogramBounds.getY(), leftWidth, spectrogramBounds.getHeight(),
                        spectrogramWriteX, 0, leftWidth, spectrogramImage.getHeight());

        if (spectrogramWriteX > 0)
            g.drawImage(spectrogramImage,
                        spectrogramBounds.getX() + leftWidth, spectrogramBounds.getY(),
                        spectrogramWriteX, spectrogramBounds.getHeight(),
                        0, 0, spectrogramWriteX, spectrogramImage.getHeight());
    }

    // Frequency axis: hairline grid + labels overlaid directly on the data,
    // like a real analyzer -- not boxed off in its own panel.
    const int imgH = spectrogramBounds.getHeight();
    const double freqs[] = { 100.0, 1000.0, 5000.0, 10000.0, 20000.0 };
    g.setFont(DeltaLookAndFeel::monoFont(10.0f));
    for (double f : freqs)
    {
        if (f >= sampleRate * 0.5) continue;
        int y = spectrogramBounds.getY() + (int) rowFromFreq(f, imgH, sampleRate);
        g.setColour(DeltaLookAndFeel::grid);
        g.drawHorizontalLine(y, (float) spectrogramBounds.getX(), (float) spectrogramBounds.getRight());
        g.setColour(DeltaLookAndFeel::amberDim);
        g.drawText(freqLabel(f), spectrogramBounds.getX() + 4, y - 12, 40, 12,
                   juce::Justification::centredLeft);
    }

    // Vignette: a soft radial darkening toward the corners, so the data
    // area reads as a lit screen rather than a flat rectangle.
    {
        juce::ColourGradient vignette(juce::Colours::transparentBlack,
                                       (float) spectrogramBounds.getCentreX(), (float) spectrogramBounds.getCentreY(),
                                       juce::Colours::black.withAlpha(0.4f),
                                       (float) spectrogramBounds.getX(), (float) spectrogramBounds.getY(), true);
        g.setGradientFill(vignette);
        g.fillRect(spectrogramBounds);
    }

    // Peak strip: a live per-row bar-graph (rotated spectrum) beside the
    // spectrogram, with a slow-decaying peak-hold contour traced over it.
    {
        auto ps = peakStripBounds;
        for (size_t y = 0; y < currentRowIntensity.size(); ++y)
        {
            float t = currentRowIntensity[y];
            if (t <= 0.01f) continue;
            float w = t * (float) ps.getWidth();
            int py = ps.getY() + (int) y;
            g.setColour(colourForIntensity(t).withAlpha(0.85f));
            g.drawHorizontalLine(py, (float) ps.getX(), (float) ps.getX() + w);
        }

        if (peakHoldT.size() > 1)
        {
            juce::Path peakPath;
            for (size_t y = 0; y < peakHoldT.size(); ++y)
            {
                float x = (float) ps.getX() + peakHoldT[y] * (float) ps.getWidth();
                float yy = (float) ps.getY() + (float) y;
                if (y == 0) peakPath.startNewSubPath(x, yy);
                else peakPath.lineTo(x, yy);
            }
            g.setColour(DeltaLookAndFeel::amber.withAlpha(0.75f));
            g.strokePath(peakPath, juce::PathStrokeType(1.0f));
        }

        g.setColour(DeltaLookAndFeel::amberDim.withAlpha(0.6f));
        g.drawRect(ps, 1);
    }

    // Colour legend: the same heatmap gradient with dB reference labels.
    {
        auto lg = legendBarBounds;
        for (int y = 0; y < lg.getHeight(); ++y)
        {
            float t = 1.0f - (float) y / (float) juce::jmax(1, lg.getHeight() - 1);
            g.setColour(colourForIntensity(t));
            g.drawHorizontalLine(lg.getY() + y, (float) lg.getX(), (float) lg.getRight());
        }
        g.setColour(DeltaLookAndFeel::amberDim);
        g.drawRect(lg, 1);

        g.setFont(DeltaLookAndFeel::monoFont(9.0f));
        const float dbTicks[] = { -10.0f, -30.0f, -50.0f, -70.0f, -90.0f };
        for (float db : dbTicks)
        {
            float t = magnitudeToIntensity(db);
            int y = lg.getY() + (int) ((1.0f - t) * (float) (lg.getHeight() - 1));
            g.setColour(DeltaLookAndFeel::textDim);
            g.drawText(juce::String((int) db), legendLabelBounds.getX(), y - 5,
                       legendLabelBounds.getWidth(), 10, juce::Justification::centredLeft);
        }
    }

    // Top HUD, overlaid directly on the spectrogram -- no card, no title bar.
    auto topHud = spectrogramBounds.withHeight(18).reduced(6, 2);

    // Delta (Delta) mark: the plugin's literal namesake as a compact
    // triangle logotype, instead of relying on plain text alone.
    auto markArea = topHud.removeFromLeft(14).toFloat().reduced(0.0f, 1.5f);
    juce::Path deltaMark;
    deltaMark.addTriangle(markArea.getCentreX(), markArea.getY(),
                          markArea.getX(), markArea.getBottom(),
                          markArea.getRight(), markArea.getBottom());
    g.setColour(DeltaLookAndFeel::amber.withAlpha(0.15f));
    g.fillPath(deltaMark);
    g.setColour(DeltaLookAndFeel::amber);
    g.strokePath(deltaMark, juce::PathStrokeType(1.3f));
    topHud.removeFromLeft(4);

    g.setColour(DeltaLookAndFeel::textDim);
    g.setFont(DeltaLookAndFeel::monoFont(12.0f, true));
    g.drawText("DELTA / NULL TEST", topHud, juce::Justification::centredLeft);

    auto readoutColour = currentSidechainPresent ? DeltaLookAndFeel::amber : DeltaLookAndFeel::textDim;
    g.setColour(readoutColour);
    juce::String hudRight = (currentSidechainPresent ? formatDb(currentDb) : juce::String("-inf dB"))
                                + "  " + statusFor(currentDb, currentSidechainPresent);
    g.drawText(hudRight, topHud, juce::Justification::centredRight);

    // Bottom bar.
    g.setColour(DeltaLookAndFeel::amberDim);
    g.drawHorizontalLine(bottomBar.getY(), 0.0f, (float) getWidth());

    g.setFont(DeltaLookAndFeel::monoFont(11.0f));
    g.setColour(currentFileModeEnabled ? DeltaLookAndFeel::amber : DeltaLookAndFeel::textDim);
    juce::String fileStatus = currentFileModeEnabled
                                   ? ("FILES  A: " + currentFileNameA + "   B: " + currentFileNameB)
                                   : juce::String("LIVE SIDECHAIN MODE (load files to compare offline)");
    g.drawText(fileStatus, fileStatusTextArea, juce::Justification::centredLeft);

    g.setColour(DeltaLookAndFeel::textDim);
    double ms = (double) currentOffsetSamples / sampleRate * 1000.0;
    juce::String offsetText = "OFFSET " + juce::String(currentOffsetSamples) + " smp ("
                                   + juce::String(ms, 2) + " ms)";
    g.drawText(offsetText, offsetTextArea, juce::Justification::centredLeft);

    // Free during the beta test period -- licensing/paid gating comes later,
    // so the footer signals status rather than just a casual name credit.
    g.setColour(DeltaLookAndFeel::amberDim);
    g.drawText("UNLICENSED", brandTextArea, juce::Justification::centredRight);

    // A brief CRT power-on sweep on load -- a small flourish, not a gimmick.
    double elapsed = juce::Time::getMillisecondCounterHiRes() - introStartMs;
    if (elapsed < introDurationMs)
    {
        float progress = (float) (elapsed / introDurationMs);
        float fade = 1.0f - progress;
        int sweepY = spectrogramBounds.getY() + (int) (progress * (float) spectrogramBounds.getHeight());

        g.setColour(DeltaLookAndFeel::amber.withAlpha(0.12f * fade));
        g.fillRect(spectrogramBounds.getX(), spectrogramBounds.getY(),
                   spectrogramBounds.getWidth(), sweepY - spectrogramBounds.getY());

        g.setColour(DeltaLookAndFeel::amber.withAlpha(0.55f * fade));
        g.fillRect(spectrogramBounds.getX(), sweepY - 1, spectrogramBounds.getWidth(), 2);
    }
}

void DeltaEditor::resized()
{
    auto full = getLocalBounds();
    bottomBar = full.removeFromBottom(64);

    full.removeFromRight(6);
    legendLabelBounds = full.removeFromRight(26);
    legendBarBounds = full.removeFromRight(14);
    full.removeFromRight(10);
    peakStripBounds = full.removeFromRight(60);
    full.removeFromRight(10);
    spectrogramBounds = full;

    if (spectrogramImage.getWidth() != juce::jmax(1, spectrogramBounds.getWidth())
        || spectrogramImage.getHeight() != juce::jmax(1, spectrogramBounds.getHeight()))
    {
        int w = juce::jmax(1, spectrogramBounds.getWidth());
        int h = juce::jmax(1, spectrogramBounds.getHeight());
        spectrogramImage = juce::Image(juce::Image::RGB, w, h, true);
        spectrogramImage.clear(spectrogramImage.getBounds(), DeltaLookAndFeel::bg);
        spectrogramWriteX = 0;

        currentRowIntensity.assign((size_t) h, 0.0f);
        peakHoldT.assign((size_t) h, 0.0f);
    }

    auto buttonRow = bottomBar.removeFromTop(40).reduced(8, 6);
    loadAButton.setBounds(buttonRow.removeFromLeft(70));
    buttonRow.removeFromLeft(6);
    loadBButton.setBounds(buttonRow.removeFromLeft(70));
    buttonRow.removeFromLeft(6);
    clearFilesButton.setBounds(buttonRow.removeFromLeft(60));
    buttonRow.removeFromLeft(20);
    testSignalButton.setBounds(buttonRow.removeFromLeft(90));
    buttonRow.removeFromLeft(8);
    alignButton.setBounds(buttonRow.removeFromLeft(70));

    auto textRow = bottomBar.reduced(8, 2);
    brandTextArea = textRow.removeFromRight(90);
    offsetTextArea = textRow.removeFromRight(170);
    fileStatusTextArea = textRow;
}
