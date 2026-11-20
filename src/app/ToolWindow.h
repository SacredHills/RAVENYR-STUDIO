#pragma once
#include <atomic>
#include <juce_gui_basics/juce_gui_basics.h>
#include "StudioLookAndFeel.h"
#include "GlobalKeys.h"

namespace ff
{
/** Keeps a plugin / tool window above the main window while you click around in the DAW, without covering other programs:
    on Windows the window is *owned* by the main window (the OS keeps owned windows above their owner and minimises them
    with it); elsewhere it is raised again, without taking focus, whenever the main window is used. */
class FloatAboveApp
{
public:
    explicit FloatAboveApp (juce::DocumentWindow& w);
    ~FloatAboveApp();
    /** Call right before hiding the window (it is owned again when it is shown). */
    void aboutToHide();
    /** The main application window the others float above (set once by the main component). */
    static void setMainWindow (juce::Component* main);
    /** Set once the app is closing: nothing is re-owned, restored or raised any more, so no window can flash up again. */
    static inline std::atomic<bool> shuttingDown { false };
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

/** A floating window (like a plugin window) showing a component that lives elsewhere: closing only hides it,
    and its position and size are remembered in the settings under `key`. */
class ToolWindow : public juce::DocumentWindow
{
public:
    ToolWindow (const juce::String& title, juce::Component& content, int w, int h, juce::PropertiesFile& s, juce::String settingsKey)
        : juce::DocumentWindow (title, palette::bg, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          settings (s), key (std::move (settingsKey))
    {
        setUsingNativeTitleBar (true);
        setContentNonOwned (&content, false);
        setResizable (true, false);
        setResizeLimits (360, 240, 3000, 2000);
        const auto saved = settings.getValue (key);
        if (saved.isEmpty() || ! restoreWindowStateFromString (saved))
            centreWithSize (w, h);
    }
    ~ToolWindow() override { settings.setValue (key, getWindowStateAsString()); }

    void showFront()
    {
        if (FloatAboveApp::shuttingDown) return;
        setVisible (true);
        toFront (true);
    }
    void closeButtonPressed() override
    {
        settings.setValue (key, getWindowStateAsString());
        floating.aboutToHide();
        setVisible (false);
        if (onClosed) onClosed();
    }
    std::function<void()> onClosed;
    bool keyPressed (const juce::KeyPress& k) override { return globalkeys::forward (k) || juce::DocumentWindow::keyPressed (k); }

private:
    juce::PropertiesFile& settings;
    juce::String key;
    FloatAboveApp floating { *this };
};
} // namespace ff
