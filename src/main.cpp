#include "ui.hpp"
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool self = false, smoke = false;
        for (int i = 1; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--self-test")
                self = true;
            if (std::wstring(argv[i]) == L"--ui-smoke-test")
                smoke = true;
        }
        LocalFree(argv);
        if (self)
            result = nco::mediaSelfTest();
        else {
            HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\NCORecorder");
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                HWND existing = FindWindowW(L"NCOWindow", nullptr);
                if (existing) {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                }
                result = 0;
            } else {
                nco::UI app(instance, smoke);
                result = app.run(show);
            }
            if (mutex)
                CloseHandle(mutex);
        }
    } catch (const std::exception& e) {
        nco::Log::get().write(e.what());
        MessageBoxW(nullptr, nco::wide(e.what()).c_str(), L"NCO could not start",
                    MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    return result;
}
