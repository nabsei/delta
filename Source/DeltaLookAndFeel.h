#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// Delta shares the Bumpin Audio umbrella brand's cyan/magenta identity
// (same palette as the KVR/Bumpin Audio logo) instead of its own amber-
// phosphor scheme -- COMPANY_NAME moved from "nabsei" to "Bumpin Audio"
// alongside this change. The monospace instrument-readout type (frequency
// axis, dB readout, file status) and the lab-instrument layout stay as-is;
// only the colour language changed.
//
// The spectrogram/legend -- a large filled surface you stare at
// continuously -- uses a continuous multi-stop gradient (deep blue -> cyan
// -> violet -> magenta -> white) rather than either a single flat hue or
// two blocks of colour fighting each other. Each intensity level gets its
// own distinct hue, but the transition is smooth and luminosity still does
// the heavy lifting for readability, so it stays calm at a glance while
// carrying more information than a single-hue ramp. Magenta/cyan are also
// used as small deliberate UI accents (engaged buttons, rim-light, glow)
// outside the spectrogram itself.
class DeltaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    inline static const juce::Colour bg        { 0xff0a0912 }; // dark navy/indigo, same "night sky" as the Bumpin Audio mark
    inline static const juce::Colour deepBlue  { 0xff1a1a4a }; // spectrogram ramp, low end
    inline static const juce::Colour cyan      { 0xff29c2d6 }; // primary accent + spectrogram ramp, low-mid
    inline static const juce::Colour cyanDim   { 0xff123a40 };
    inline static const juce::Colour violet    { 0xff7a5ad6 }; // spectrogram ramp, mid-high
    inline static const juce::Colour magenta   { 0xffe0479c }; // pop accent + spectrogram ramp, high end
    inline static const juce::Colour grid      { 0xff141228 };
    inline static const juce::Colour textDim   { 0xff7a7a95 };

    static constexpr float familyCornerSize = 6.0f; // same rounding convention as Montagem/Yano chrome

    DeltaLookAndFeel()
    {
        setColour(juce::TextButton::buttonColourId, bg);
        setColour(juce::TextButton::buttonOnColourId, magenta.withAlpha(0.5f));
        setColour(juce::TextButton::textColourOffId, cyanDim.brighter(0.6f));
        setColour(juce::TextButton::textColourOnId, magenta);
    }

    static juce::Font monoFont(float height, bool bold = false)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                             height, bold ? juce::Font::bold : juce::Font::plain));
    }

    // Same default (non-monospace) typeface Montagem/Yano use for their
    // title/subtitle/footer labels -- shared "family" typography, kept
    // separate from monoFont() which stays reserved for instrument-readout
    // text (frequency axis, dB readout, file status).
    static juce::Font familyFont(float height, bool bold = false)
    {
        return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
    }

    // Same heavy geometric sans used in the Bumpin Audio mark's "B", for the
    // one piece of text that should read as branded rather than generic --
    // the title. Falls back to the platform default bold if "Avenir Next"
    // isn't installed (e.g. Windows CI runners), which is an acceptable
    // degrade, not a build break.
    static juce::Font brandTitleFont(float height)
    {
        return juce::Font(juce::FontOptions(juce::String("Avenir Next"), height, juce::Font::bold));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        // Engaged (toggled-on / pressed) reads magenta, idle/hover reads
        // cyan -- uses both brand accent colours meaningfully instead of
        // just swapping amber for a single new hue.
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        bool engaged = button.getToggleState() || shouldDrawButtonAsDown;

        float baseAlpha = engaged ? 0.5f : (shouldDrawButtonAsHighlighted ? 0.16f : 0.0f);
        if (baseAlpha > 0.0f)
        {
            juce::Colour base = engaged ? magenta : cyan;
            juce::ColourGradient fillGrad(base.withAlpha(baseAlpha * 1.3f), bounds.getCentreX(), bounds.getY(),
                                           base.withAlpha(baseAlpha * 0.55f), bounds.getCentreX(), bounds.getBottom(), false);
            g.setGradientFill(fillGrad);
            g.fillRoundedRectangle(bounds, familyCornerSize * 0.6f);
        }

        // Glossy highlight -- soft light band across the top third, same
        // touch as the Bumpin Audio logo's glass-button treatment.
        {
            auto gloss = bounds.withHeight(bounds.getHeight() * 0.45f).reduced(1.0f, 0.0f);
            juce::ColourGradient glossGrad(juce::Colours::white.withAlpha(0.09f), gloss.getCentreX(), gloss.getY(),
                                            juce::Colours::white.withAlpha(0.0f), gloss.getCentreX(), gloss.getBottom(), false);
            g.setGradientFill(glossGrad);
            g.fillRoundedRectangle(gloss, familyCornerSize * 0.5f);
        }

        g.setColour(engaged ? magenta : (shouldDrawButtonAsHighlighted ? cyan : cyanDim));
        g.drawRoundedRectangle(bounds, familyCornerSize * 0.6f, shouldDrawButtonAsHighlighted ? 1.5f : 1.0f);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int height) override
    {
        return monoFont((float) height * 0.42f, true);
    }

    juce::Font getLabelFont(juce::Label&) override
    {
        return monoFont(13.0f);
    }
};
