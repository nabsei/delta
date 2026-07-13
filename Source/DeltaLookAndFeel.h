#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

// A completely different visual language from the Montagem family: no
// rounded corners, no red/green palette, no centred title-card layout, no
// knob. Delta reads like a lab instrument -- pure black, a single amber
// phosphor accent, monospace type, hairline grid, sharp-cornered controls.
class DeltaLookAndFeel : public juce::LookAndFeel_V4
{
public:
    inline static const juce::Colour bg        { 0xff000000 };
    inline static const juce::Colour amber     { 0xffffb000 };
    inline static const juce::Colour amberDim  { 0xff6b4a00 };
    inline static const juce::Colour grid      { 0xff2a2410 };
    inline static const juce::Colour textDim   { 0xff707070 };

    DeltaLookAndFeel()
    {
        setColour(juce::TextButton::buttonColourId, bg);
        setColour(juce::TextButton::buttonOnColourId, amberDim);
        setColour(juce::TextButton::textColourOffId, amberDim.brighter(0.6f));
        setColour(juce::TextButton::textColourOnId, amber);
    }

    static juce::Font monoFont(float height, bool bold = false)
    {
        return juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(),
                                             height, bold ? juce::Font::bold : juce::Font::plain));
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
        auto fill = button.getToggleState() ? amberDim.withAlpha(0.5f) : juce::Colours::transparentBlack;
        if (shouldDrawButtonAsDown) fill = amber.withAlpha(0.45f);
        else if (shouldDrawButtonAsHighlighted) fill = amber.withAlpha(0.16f);

        g.setColour(fill);
        g.fillRect(bounds);

        bool lit = button.getToggleState() || shouldDrawButtonAsHighlighted;
        g.setColour(lit ? amber : amberDim);
        g.drawRect(bounds, shouldDrawButtonAsHighlighted ? 1.5f : 1.0f);
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
