#include "platform.hpp"
#include <ostream>

namespace nco {
namespace {
struct KeyEvents {
    int single = 0, combination = 0;
};
LRESULT CALLBACK testProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    auto state = reinterpret_cast<KeyEvents*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<KeyEvents*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == WM_HOTKEY && state) {
        if (w == 100)
            ++state->single;
        if (w == 101)
            ++state->combination;
        return 0;
    }
    return DefWindowProcW(h, message, w, l);
}
bool pumpUntil(const std::function<bool()>& done) {
    const auto until = GetTickCount64() + 1000;
    while (!done() && GetTickCount64() < until) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return done();
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void releaseTestKeys() {
    INPUT keys[3]{};
    const WORD codes[] = {VK_F7, VK_SHIFT, VK_CONTROL};
    for (int i = 0; i < 3; ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = codes[i];
        keys[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(3, keys, sizeof(INPUT));
}
} // namespace

void platformSelfTest(std::ostream& report) {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = testProc;
    wc.lpszClassName = L"NCOHotkeyIntegrationTest";
    RegisterClassW(&wc);
    KeyEvents events;
    HWND window =
        CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"NCO hotkey test", WS_OVERLAPPEDWINDOW,
                        0, 0, 200, 100, nullptr, nullptr, wc.hInstance, &events);
    HWND conflict = CreateWindowExW(0, L"STATIC", L"NCO shortcut owner", 0, 0, 0, 1, 1,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    require(window && conflict, "Cannot create hotkey test windows");
    try {
        {
            Hotkeys keys(window);
            auto duplicates = keys.apply({{VK_F7, 0}, {VK_F7, 0}});
            require(!duplicates[0].empty() && !duplicates[1].empty(),
                    "Duplicate shortcuts were accepted");
            const bool owned = RegisterHotKey(conflict, 999, MOD_NOREPEAT, VK_F6) != FALSE;
            auto conflicts = keys.apply({{VK_F6, 0}});
            if (owned)
                require(!conflicts[0].empty(), "Shortcut owned by another window was accepted");
            UnregisterHotKey(conflict, 999);
            auto valid = keys.apply({{VK_F7, 0}, {VK_F7, MOD_CONTROL | MOD_SHIFT}});
            require(valid[0].empty() && valid[1].empty(),
                    "Single-key or modifier shortcut registration failed");
            report << "PASS Windows global shortcut registration, single keys, combinations and "
                      "duplicate detection\n";
            report
                << (owned
                        ? "PASS owned-shortcut conflict warning\n"
                        : "UNVERIFIED controlled shortcut conflict: test key was already owned\n");
            ShowWindow(window, SW_SHOWMINNOACTIVE);
            require(IsIconic(window) != FALSE, "Hotkey test window did not minimize");
            INPUT input[6]{};
            auto set = [&](int i, WORD key, bool up) {
                input[i].type = INPUT_KEYBOARD;
                input[i].ki.wVk = key;
                input[i].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            };
            set(0, VK_F7, false);
            set(1, VK_F7, true);
            if (SendInput(2, input, sizeof(INPUT)) == 2) {
                require(pumpUntil([&] { return events.single > 0; }),
                        "Global single-key shortcut did not reach the minimized window");
                set(0, VK_CONTROL, false);
                set(1, VK_SHIFT, false);
                set(2, VK_F7, false);
                set(3, VK_F7, true);
                set(4, VK_SHIFT, true);
                set(5, VK_CONTROL, true);
                require(SendInput(6, input, sizeof(INPUT)) == 6,
                        "Cannot inject shortcut combination");
                require(pumpUntil([&] { return events.combination > 0; }),
                        "Global combination did not reach the minimized window");
                report << "PASS real global key and combination delivery to a minimized Win32 "
                          "window\n";
            } else
                report << "UNVERIFIED minimized shortcut delivery: desktop input injection "
                          "unavailable\n";
        }
        releaseTestKeys();
        DestroyWindow(conflict);
        DestroyWindow(window);
    } catch (...) {
        releaseTestKeys();
        UnregisterHotKey(conflict, 999);
        DestroyWindow(conflict);
        DestroyWindow(window);
        throw;
    }
}
} // namespace nco
