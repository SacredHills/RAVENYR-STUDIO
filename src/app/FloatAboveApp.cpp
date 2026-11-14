#include "ToolWindow.h"
#if defined (_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#endif

namespace ff
{
static juce::Component::SafePointer<juce::Component>& mainWin()
{
    static juce::Component::SafePointer<juce::Component> p;
    return p;
}

static std::vector<juce::DocumentWindow*>& floaters()
{
    static std::vector<juce::DocumentWindow*> v;
    return v;
}

struct FloatAboveApp::Impl final : private juce::ComponentListener, private juce::FocusChangeListener
{
    explicit Impl (juce::DocumentWindow& w) : win (w)
    {
        floaters().push_back (&w);
        w.addComponentListener (this);
        juce::Desktop::getInstance().addFocusChangeListener (this);
    }
    ~Impl() override
    {
        juce::Desktop::getInstance().removeFocusChangeListener (this);
        win.removeComponentListener (this);
        auto& v = floaters();
        v.erase (std::remove (v.begin(), v.end(), &win), v.end());
    }

    void attachOwner()
    {
#if defined (_WIN32)
        auto* main = mainWin().getComponent();
        if (main == nullptr || ! win.isVisible()) return;
        auto *p = win.getPeer(), *mp = main->getPeer();
        if (p == nullptr || mp == nullptr) return;
        SetWindowLongPtrW ((HWND) p->getNativeHandle(), GWLP_HWNDPARENT, (LONG_PTR) mp->getNativeHandle());
#endif
    }
    void componentVisibilityChanged (juce::Component&) override
    {
        attachOwner();
        juce::Component::SafePointer<juce::DocumentWindow> sp (&win);   // the peer may not exist yet: once more when the window is up
        juce::MessageManager::callAsync ([this, sp] { if (sp != nullptr) attachOwner(); });
    }
    void componentParentHierarchyChanged (juce::Component&) override { attachOwner(); }

    void globalFocusChanged (juce::Component* focused) override
    {
#if ! defined (_WIN32)
        auto* main = mainWin().getComponent();
        if (main == nullptr || focused == nullptr || ! win.isVisible()) return;
        if (focused->getTopLevelComponent() == main->getTopLevelComponent())
            win.toFront (false);   // above the DAW again, without taking the keyboard
#endif
    }
    juce::DocumentWindow& win;
};

FloatAboveApp::FloatAboveApp (juce::DocumentWindow& w) : impl (std::make_unique<Impl> (w)) {}
FloatAboveApp::~FloatAboveApp() = default;

void FloatAboveApp::setMainWindow (juce::Component* main)
{
    mainWin() = main;
    for (auto* w : floaters())
        w->setVisible (w->isVisible());   // windows already open pick the owner up through the visibility callback
}
}
