#include "ui.hpp"
#include <fstream>
#include "diagnostics.hpp"

namespace nco {
// Uses the actual controls and Win32 edit/message paths. No capture hardware is simulated.
void UI::smokeControls() {
    const Config original = cfg_;
    std::ofstream report(std::filesystem::path(L"nco-self-test") / L"ui-results.txt");
    auto require = [](bool ok, const char* message) {
        if (!ok)
            throw std::runtime_error(message);
    };
    auto page = [&](int index) {
        page_ = index;
        scroll_ = 0;
        popup_.open = false;
        draw();
    };
    auto clickControl = [&](const std::wstring& id) {
        auto find = [&] {
            return std::find_if(controls_.begin(), controls_.end(),
                                [&](const auto& c) { return c.id == id && c.enabled; });
        };
        auto it = find();
        if (it == controls_.end())
            throw std::runtime_error("Test control missing or disabled: " + utf8(id));
        const bool chrome = id.rfind(L"nav", 0) == 0 || id == L"min" || id == L"max";
        if (!chrome && id.rfind(L"option", 0) != 0 &&
            (it->rect.top < 136 || it->rect.bottom > h_ - 38)) {
            scroll_ = std::clamp(scroll_ + int(it->rect.top - 160), 0, maxScroll_);
            draw();
            it = find();
            require(it != controls_.end(), "Test control disappeared after scrolling");
        }
        const int x = int((it->rect.left + it->rect.right) * dpi_ / 192);
        const int y = int((it->rect.top + it->rect.bottom) * dpi_ / 192);
        SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
        SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
        if (!IsIconic(hwnd_))
            draw();
    };
    auto toggle = [&](const wchar_t* id, bool Config::*member) {
        const bool before = cfg_.*member;
        clickControl(id);
        require(cfg_.*member != before && Config::load().*member == cfg_.*member,
                "Toggle did not change and persist its setting");
        clickControl(id);
        require(cfg_.*member == before, "Toggle could not restore its setting");
    };
    auto choose = [&](const wchar_t* id, int index) {
        clickControl(id);
        require(popup_.open, "Dropdown did not open");
        for (int attempt = 0; attempt < 12 &&
             (index < popup_.scroll || index >= popup_.scroll + popup_.visible); ++attempt) {
            const int delta = index < popup_.scroll ? WHEEL_DELTA : -WHEEL_DELTA;
            SendMessageW(hwnd_, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(delta)), 0);
            draw();
        }
        clickControl(L"option" + std::to_wstring(index));
        require(!popup_.open, "Dropdown did not close after selection");
    };
    auto number = [&](const wchar_t* id, int value, int Config::*member) {
        clickControl(id);
        require(edit_ != nullptr, "Numeric edit did not open");
        SetWindowTextW(edit_, std::to_wstring(value).c_str());
        SendMessageW(edit_, WM_KEYDOWN, VK_RETURN, 0);
        draw();
        require(!edit_ && cfg_.*member == value && Config::load().*member == value,
                "Numeric edit did not commit and persist");
    };
    try {
        {
            std::vector<Source> list(1);
            list[0].monitor = reinterpret_cast<HMONITOR>(uintptr_t(1));
            Source display = list[0];
            replaceRegion(list, display, {100, 80, 420, 320});
            replaceRegion(list, display, {40, 60, 840, 660});
            require(list.size() == 2 && list.back().region.right == 840 &&
                    list.back().region.left == 40, "Region reselection accumulated stale entries");
            report << "PASS region reselection replaces the previous source entry\n";
        }
        // Deterministic control tests; animation timing remains a separate visual check.
        cfg_.animations = false;
        cfg_.replay = false;
        save();
        page(0);
        toggle(L"mic", &Config::mic);
        toggle(L"desktop", &Config::desktop);
        require(std::none_of(controls_.begin(), controls_.end(), [](const auto& c) {
            return c.id == L"preview" || c.id == L"fps" || c.id == L"nav3" || c.id == L"nav5";
        }), "Removed dashboard controls or tabs are still visible");
        clickControl(L"nav1");
        require(page_ == 1, "Navigation button did not change page");
        // Test actual settings layout independently of unavailable CI capture hardware.
        const auto previousStats = stats_;
        stats_.recording = false;
        stats_.replay = true;
        controls_.clear();
        target_->BeginDraw();
        settings();
        check(target_->EndDraw(), "Replay settings regression rendering");
        require(std::any_of(controls_.begin(), controls_.end(), [](const auto& control) {
            return control.id == L"fps" && control.enabled;
        }), "Replay incorrectly locks Recording settings");
        stats_.recording = true;
        controls_.clear();
        target_->BeginDraw();
        settings();
        check(target_->EndDraw(), "Replay settings regression rendering");
        require(std::any_of(controls_.begin(), controls_.end(), [](const auto& control) {
            return control.id == L"fps" && !control.enabled;
        }), "Active recording must protect its encoder configuration");
        stats_ = previousStats;
        draw();
        report << "PASS replay allows Recording settings while active recording remains protected\n";
        {
            auto it = std::find_if(controls_.begin(), controls_.end(), [](const auto& control) {
                return control.id == L"fps";
            });
            require(it != controls_.end(), "FPS field missing");
            int x = int((it->rect.left + it->rect.right) * dpi_ / 192);
            int y = int((it->rect.top + it->rect.bottom) * dpi_ / 192);
            SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            require(!popup_.open, "Dropdown activated before mouse release");
            SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(x, y));
            draw();
        }
        require(popup_.open, "Dropdown did not open on release");
        clickControl(L"fps");
        require(!popup_.open, "Second click did not close the same dropdown");
        clickControl(L"fps");
        require(popup_.items.size() == 3, "Frame rate dropdown must offer only 15, 30 and 60 FPS");
        clickControl(L"fps");
        choose(L"fps", 0);
        require(cfg_.fps == 15, "FPS dropdown did not commit");
        choose(L"quality", 4);
        require(cfg_.quality == 4, "Quality dropdown did not commit");
        choose(L"resolution", 5);
        number(L"width", 1280, &Config::width);
        number(L"height", 720, &Config::height);
        number(L"bitrate", 8000, &Config::bitrateKbps);
        toggle(L"remux", &Config::remux);
        choose(L"format", 3);
        require(cfg_.format == "webm", "Container dropdown did not commit");
        choose(L"encoder", 0);
        require(cfg_.encoder == "auto", "Auto encoder selection did not commit");
        report << "PASS clean dashboard, mute controls, dropdown re-click dismissal, navigation, recording dropdowns, custom numeric "
                  "fields and remux persistence through real control handlers\n";
        page(2);
        choose(L"micdevice", 0);
        choose(L"sounddevice", 0);
        toggle(L"mic", &Config::mic);
        toggle(L"desktop", &Config::desktop);
        toggle(L"tracks", &Config::separateTracks);
        number(L"micvolume", 65, &Config::micVolume);
        number(L"desktopvolume", 70, &Config::desktopVolume);
        number(L"micgain", 125, &Config::micGain);
        number(L"offset", 35, &Config::audioOffsetMs);
        clickControl(L"refreshdevices");
        page(1);
        for (int i = 0; i < 3; ++i) {
            choose(L"duration", i);
            require(cfg_.replaySeconds == (i == 0 ? 15 : i == 1 ? 30 : 60),
                    "Replay duration dropdown did not commit");
        }
        report << "PASS audio settings controls and 15/30/60-second replay configuration\n";
        page(4);
        clickControl(L"key0");
        SendMessageW(hwnd_, WM_KEYDOWN, VK_F7, 0);
        require(cfg_.keys[0].key == VK_F7 && Config::load().keys[0].key == VK_F7,
                "Interactive hotkey capture did not persist");
        clickControl(L"key1");
        SendMessageW(hwnd_, WM_KEYDOWN, VK_F7, 0);
        require(!keyErrors_[0].empty() && !keyErrors_[1].empty(),
                "Duplicate shortcut warnings missing");
        draw();
        clickControl(L"clear1");
        require(!cfg_.keys[1].key, "Clear shortcut button failed");
        page(6);
        require(std::any_of(controls_.begin(), controls_.end(), [](const auto& control) {
            return control.id == L"manualRemux" && control.enabled;
        }), "Manual remux control missing");
        auto diagnostic = std::filesystem::path(L"nco-self-test") / L"diagnostic-export.txt";
        exportDiagnostics(diagnostic, cfg_, stats_, dpi_, encoders_);
        std::ifstream exported(diagnostic);
        std::string contents((std::istreambuf_iterator<char>(exported)), {});
        require(contents.find("prepare_ms=") != std::string::npos &&
                contents.find("Windows=") != std::string::npos &&
                contents.find("Recent timestamped log") != std::string::npos,
                "Full diagnostic export missing critical fields");
        SendMessageW(hwnd_, WM_ENTERSIZEMOVE, 0, 0);
        SendMessageW(hwnd_, WM_TIMER, 4, 0);
        SendMessageW(hwnd_, WM_EXITSIZEMOVE, 0, 0);
        toggle(L"logging", &Config::verbose);
        choose(L"captureApi", 2);
        require(cfg_.captureBackend == 2, "Capture compatibility dropdown failed");
        report << "PASS interactive shortcut capture/clear/duplicates and Settings controls, full diagnostic export and move-loop messages\n";
        if (exclusion_) {
            previewTimerActive_ = false;
            tweens_.clear();
            popup_.open = popup_.closing = false;
            navigationStart_ = hoverStart_ = clockUs() - 500000;
            toast(L"Recording started");
            toastStart_ = clockUs() - 500000;
            ValidateRect(hwnd_, nullptr);
            SendMessageW(hwnd_, WM_TIMER, 2, 0);
            require(toastVisible_, "Toast expired before one second");
            require(!GetUpdateRect(hwnd_, nullptr, FALSE),
                    "A static page was repainted for every toast animation tick");
            toastStart_ = clockUs() - 1100000;
            SendMessageW(hwnd_, WM_TIMER, 2, 0);
            require(!toastVisible_, "Toast did not expire after one second");
            report << "PASS toast lifetime and animation without repainting a static page\n";
        }
        page(0);
        clickControl(L"max");
        require(IsZoomed(hwnd_) != FALSE, "Maximize button failed");
        clickControl(L"max");
        require(!IsZoomed(hwnd_), "Restore button failed");
        clickControl(L"min");
        require(IsIconic(hwnd_) != FALSE, "Minimize button failed");
        ShowWindow(hwnd_, SW_RESTORE);
        report << "PASS native minimize, maximize and restore controls\n";
        cfg_.animations = true;
        pressed_ = L"popupRegression";
        dropdown({500, h_ - 110, 850, h_ - 70}, {L"One", L"Two", L"Three"}, 2, [](int) {});
        popup_.start = clockUs() + 1000000;
        controls_.clear();
        target_->BeginDraw();
        popup();
        check(target_->EndDraw(), "Popup reveal regression drawing");
        require(std::none_of(controls_.begin(), controls_.end(), [](const auto& control) {
            return control.enabled && control.id.rfind(L"option", 0) == 0;
        }), "Unrevealed popup rows remained clickable");
        popup_.start = clockUs() - 200000;
        controls_.clear();
        target_->BeginDraw();
        popup();
        check(target_->EndDraw(), "Completed popup reveal regression");
        int visibleOptions = 0;
        for (const auto& control : controls_)
            if (control.enabled && control.id.rfind(L"option", 0) == 0) {
                ++visibleOptions;
                require(control.rect.bottom <= popup_.anchor.top - 5,
                        "Upward popup did not stay attached above its field");
            }
        require(visibleOptions == 3, "Completed popup hid selectable rows");
        popup_.open = popup_.closing = false;
        report << "PASS upward dropdown anchors to field and clips unrevealed hit targets\n";
        report << "UNVERIFIED capture/audio/replay operation, folder dialogs, every hardware encoder, "
                  "notification exclusion, real DPI scaling and animation quality\n";
    } catch (const std::exception& error) {
        report << "FAIL " << error.what() << "\n";
        renderFailed_ = true;
    }
    finishEdit(false);
    hotkeyIndex_ = -1;
    cfg_ = original;
    keyErrors_ = hotkeys_->apply(cfg_.keys);
    save();
}
} // namespace nco
