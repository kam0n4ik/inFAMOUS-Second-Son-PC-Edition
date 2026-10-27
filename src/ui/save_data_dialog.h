// SecondSonPC — the system's save data dialog over the game (runtime/savedata/savedata_dialog.h): the list of saves
// to save into / load from, and the system's messages about them (overwrite?, no data, corrupted, errors), with the
// keyboard, a controller or the mouse. A model like the settings menu: it knows nothing of SDL, Win32 or the GPU; the
// host (ui/menu_controller.*) feeds it input, renders its DrawList with the menu overlay and gives its answer to the
// runtime. Main thread only. Own code.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input/device_tracker.h"
#include "runtime/savedata/savedata_dialog.h"
#include "ui/draw_list.h"
#include "ui/settings_menu.h"

namespace SecondSon::Ui {

class SaveDataDialogView {
public:
    using Request = SaveData::Dialog::Request;
    using Answer = SaveData::Dialog::Answer;

    struct Env {
        float width = 1920.0f; // client area in pixels
        float height = 1080.0f;
        float dpi_scale = 1.0f;
        bool ru = true;
        Input::PromptStyle prompts = Input::PromptStyle::Keyboard;
        bool confirm_is_circle = false;
    };

    /// `icons`: Request::items' pictures decoded (the same order; nullptr: none).
    void Open(Request request, std::vector<std::shared_ptr<const Image>> icons);
    void Close();
    bool IsOpen() const {
        return open_;
    }
    u64 Id() const {
        return request_.id;
    }
    const Request& Current() const {
        return request_;
    }

    void SetEnv(const Env& env) {
        env_ = env;
    }
    void OnNav(Nav nav);
    void OnMouseMove(float x, float y);
    void OnMouseButton(float x, float y, bool down);
    void OnWheel(float notches);
    void Build(TextMeasure& measure, DrawList& out);
    /// The user's answer, taken once by the host.
    std::optional<Answer> TakeAnswer();

    size_t FocusedItem() const {
        return focus_;
    }
    size_t FocusedChoice() const {
        return choice_;
    }

private:
    struct Hit {
        enum class Kind : u8 { Item, Choice, Back };
        Kind kind;
        Rect rect;
        size_t index = 0;
    };

    bool IsList() const {
        return request_.mode == SaveData::Dialog::Mode::List;
    }
    std::string_view T(std::string_view en, std::string_view ru) const {
        return env_.ru ? ru : en;
    }
    std::string Title() const;
    std::string MessageText() const;
    std::string ChoiceLabel(SaveData::Dialog::Choice c) const;
    std::string HintText() const;
    float Scale() const;
    void DrawItem(DrawList& out, Rect r, size_t index, bool focused, bool hovered) const;
    void Choose(); // Accept
    void MoveFocus(int delta);

    Env env_;
    bool open_ = false;
    Request request_;
    std::vector<std::shared_ptr<const Image>> icons_;
    std::vector<SaveData::Dialog::Choice> choices_;
    size_t focus_ = 0;  // the list's item
    size_t choice_ = 0; // a message's button
    float scroll_ = 0;  // the list's first visible pixel
    bool scroll_to_focus_ = true;
    float row_h_ = 100, view_h_ = 400; // the list's row pitch and visible height (last Build, pixels)
    int hover_ = -1;    // hits_ index under the pointer
    bool pressed_ = false;
    float mouse_x_ = -1, mouse_y_ = -1;
    std::vector<Hit> hits_;
    std::optional<Answer> answer_;
};

/// Self-test (--selftest; SSPC_SELFTEST_ONLY=savedata alone): the game's Save / Load call sequences (eboot.bin
/// 0x1e5720..0x1e6497) against SecondSonPC's libSceSaveData + libSceSaveDataDialog in a scratch save folder
/// (`scratch`: emptied first), answered through this view as a user would. SaveData::Configure and the dialog's host
/// are changed for the test and put back at the end: no game may run in the process.
using SaveDialogCheckFn = std::function<void(std::string_view what, bool ok, const std::string& detail)>;
void RunSaveDataDialogChecks(const SaveDialogCheckFn& check, const std::filesystem::path& scratch);

} // namespace SecondSon::Ui
