// Windows 7 stand-ins for the newer functions JUCE 9 and the C++ runtime call that YY-Thunks 1.2.3 (linked in too,
// see plugin/CMakeLists.txt) does not cover; tools\dist\check_win7.ps1 finds any new one. Each calls the real function when Windows has it. Compiled into each Windows
// binary, ahead of the system's import libraries, so these definitions take the place of the imports, as YY-Thunks'
// own do. As in the user's TS Reborn (app/source/Win7Compat.cpp) and FastChat.
#ifdef _WIN32

#include <windows.h>

namespace {

FARPROC systemFunction(const wchar_t* dll, const char* name) {
    HMODULE module = GetModuleHandleW(dll);
    if (module == nullptr) module = LoadLibraryW(dll);
    return module != nullptr ? GetProcAddress(module, name) : nullptr;
}

}  // namespace

// UIAutomationCore, Windows 8 and later (JUCE's accessibility). Declared here rather than from the SDK header, which
// would make it an import.
extern "C" HRESULT WINAPI UiaDisconnectProvider(void* provider) {
    using Fn = HRESULT(WINAPI*)(void*);
    static const Fn real = reinterpret_cast<Fn>(systemFunction(L"uiautomationcore.dll", "UiaDisconnectProvider"));
    return real != nullptr ? real(provider) : S_OK;  // Windows 7: nothing is connected to disconnect
}
extern "C" const void* __imp_UiaDisconnectProvider = reinterpret_cast<const void*>(&UiaDisconnectProvider);

// user32, Windows 10 1607 and later. windows.h declares it as an import, so the stand-in has a name of its own and
// fills the import's slot. Windows 7 has no per-thread DPI awareness: null, the call's failure value, which callers
// take as "unchanged".
namespace {
DPI_AWARENESS_CONTEXT WINAPI setThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT context) {
    using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static const Fn real = reinterpret_cast<Fn>(systemFunction(L"user32.dll", "SetThreadDpiAwarenessContext"));
    return real != nullptr ? real(context) : nullptr;
}
}  // namespace
extern "C" const void* __imp_SetThreadDpiAwarenessContext =
    reinterpret_cast<const void*>(&setThreadDpiAwarenessContext);

// cfgmgr32, Windows 8 and later: JUCE 9's MIDI device change notifications (juce_Midi_windows.cpp), which keeps a
// registration only when the call returns CR_SUCCESS. Windows 7 has no such notifications: CR_CALL_NOT_IMPLEMENTED,
// so MIDI devices plugged in later are not noticed until the device list is read again. The cfgmgr32.h types are
// left out (they would declare the imports); a CONFIGRET is a DWORD, the handle a pointer.
namespace {
constexpr DWORD kCrSuccess = 0x00, kCrCallNotImplemented = 0x34;
DWORD WINAPI cmRegisterNotification(void* filter, void* context, void* callback, void** notification) {
    using Fn = DWORD(WINAPI*)(void*, void*, void*, void**);
    static const Fn real = reinterpret_cast<Fn>(systemFunction(L"cfgmgr32.dll", "CM_Register_Notification"));
    if (real != nullptr) return real(filter, context, callback, notification);
    if (notification != nullptr) *notification = nullptr;
    return kCrCallNotImplemented;
}
DWORD WINAPI cmUnregisterNotification(void* notification) {
    using Fn = DWORD(WINAPI*)(void*);
    static const Fn real = reinterpret_cast<Fn>(systemFunction(L"cfgmgr32.dll", "CM_Unregister_Notification"));
    return real != nullptr ? real(notification) : kCrSuccess;  // Windows 7: nothing was registered
}
}  // namespace
extern "C" const void* __imp_CM_Register_Notification = reinterpret_cast<const void*>(&cmRegisterNotification);
extern "C" const void* __imp_CM_Unregister_Notification = reinterpret_cast<const void*>(&cmUnregisterNotification);

#endif
