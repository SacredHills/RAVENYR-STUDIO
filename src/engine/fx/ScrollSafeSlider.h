#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ff
{
/** A slider the mouse wheel only moves while Ctrl / Cmd is held: otherwise the wheel scrolls whatever it sits in (the
    mixer, a rack), so scrolling past a fader or knob never changes it by accident. */
class ScrollSafeSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown() || e.mods.isCommandDown()) juce::Slider::mouseWheelMove (e, w);
        else juce::Component::mouseWheelMove (e, w);   // passes it on to the parent
    }
};
} // namespace ff
