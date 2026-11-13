#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>
#include "TypingAnnouncement.h"

bool hs2port::announceTypingSetting(juce::Component& focus, const juce::String& text) {
    auto* peer = focus.getPeer();
    if (peer == nullptr) return false;
    // JUCE loads UI Automation. Resolve optional functions without importing newer Windows APIs.
    auto* handler = peer->getComponent().getAccessibilityHandler();
    if (handler == nullptr || handler->getNativeImplementation() == nullptr) return false;
    const auto module = GetModuleHandleW(L"UIAutomationCore.dll");
    if (module == nullptr) return false;
    const auto notify = reinterpret_cast<decltype(&UiaRaiseNotificationEvent)>(
        GetProcAddress(module, "UiaRaiseNotificationEvent"));
    const auto listening = reinterpret_cast<decltype(&UiaClientsAreListening)>(
        GetProcAddress(module, "UiaClientsAreListening"));
    if (notify == nullptr || listening == nullptr || !listening()) return false;
    // JUCE's pinned Win32 native handle has IRawElementProviderSimple as its
    // first COM base. The default HWND provider lacks JUCE's window identity.
    auto* native = reinterpret_cast<IRawElementProviderSimple*>(handler->getNativeImplementation());
    IRawElementProviderSimple* provider = nullptr;
    if (FAILED(native->QueryInterface(IID_PPV_ARGS(&provider))) || provider == nullptr) return false;
    const auto message = SysAllocString(text.toWideCharPointer());
    const auto activity = SysAllocString(L"HS2.MusicalKeyboard");
    const bool sent = message && activity && SUCCEEDED(notify(provider, NotificationKind_Other,
        NotificationProcessing_MostRecent, message, activity));
    SysFreeString(message);
    SysFreeString(activity);
    provider->Release();
    return sent;
}
