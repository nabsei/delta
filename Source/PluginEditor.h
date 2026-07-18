#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "DeltaProcessor.h"
#include "DeltaLookAndFeel.h"

class DeltaEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DeltaEditor(DeltaProcessor&);
    ~DeltaEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void pushSpectrumColumns();

    DeltaProcessor& processorRef;
    DeltaLookAndFeel lookAndFeel;

    // Family-standard header/brand chrome (same structure as Montagem/Yano:
    // title + subtitle at top, "@handle" bottom-right) -- everything else
    // below (spectrogram, buttons, legend, HUD) is Delta-specific and unchanged.
    juce::Label titleLabel;
    juce::Label subtitleLabel;
    juce::Label brandLabel;

    juce::TextButton alignButton { "ALIGN" };
    juce::TextButton testSignalButton { "TEST SIG" };
    juce::TextButton loadAButton { "LOAD A" };
    juce::TextButton loadBButton { "LOAD B" };
    juce::TextButton clearFilesButton { "CLEAR" };
    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::Rectangle<int> spectrogramBounds;
    juce::Rectangle<int> peakStripBounds;
    juce::Rectangle<int> legendBarBounds;
    juce::Rectangle<int> legendLabelBounds;
    juce::Rectangle<int> displayBounds; // spectrogram + peak strip + legend, as one bezel
    juce::Rectangle<int> bottomBar;
    juce::Rectangle<int> fileStatusTextArea, offsetTextArea, brandTextArea;

    juce::Image spectrogramImage;
    int spectrogramWriteX = 0;
    std::vector<float> spectrumColumnScratch;

    juce::Image crtNoise; // fixed grain texture, tiled over the display in paint()

    // Live per-row intensity (latest column) and its slow-decaying maximum,
    // drawn as a rotated bar-graph + peak-hold contour beside the spectrogram.
    std::vector<float> currentRowIntensity;
    std::vector<float> peakHoldT;

    // HUD text state, refreshed on the timer and drawn directly in paint().
    float currentDb = -100.0f;
    float displayDb = -100.0f; // ballistics-smoothed copy of currentDb, for the readout only
    bool currentSidechainPresent = false;
    int currentOffsetSamples = 0;
    bool currentFileModeEnabled = false;
    juce::String currentFileNameA, currentFileNameB;

    double introStartMs = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeltaEditor)
};
