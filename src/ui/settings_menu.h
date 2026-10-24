// SecondSonPC — the PC settings window as a model (docs/PC_SETTINGS_UI.md): pages built from the settings registry,
// focus and navigation from the keyboard, a controller or the mouse, editing (toggles, drop-down lists, sliders, key
// capture), confirmation of display changes, conflicts, and the layout it draws into a DrawList. It knows nothing of
// SDL, Win32 or the GPU: the host (ui/menu_controller.*) feeds it input and renders the list. Main thread only.
// Own code.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "input/device_tracker.h"
#include "input/settings.h"
#include "settings/settings_service.h"
#include "ui/draw_list.h"

namespace SecondSon::Ui {

enum class Nav : u8 {
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,    // closes a list, a dialog or the window
    PrevTab, // previous page
    NextTab,
    Reset,   // the focused setting (or key) back to its default
    Clear,   // a key binding slot emptied
    PageUp,
    PageDown,
};

/// The window's pages (the navigation on its left). Each lists settings of one or more registry categories.
enum class Page : u8 { Graphics, Display, Controls, Keys, Audio, Game, Advanced, Experiments, Count };
constexpr size_t kPageCount = static_cast<size_t>(Page::Count);
std::string_view PageLabel(Page page, bool ru);
/// The page a registry category is shown on.
Page PageOf(Settings::Category category);

/// What the host knows that the menu shows; set before input and Build.
struct MenuEnv {
    float width = 1920.0f; // client area in pixels
    float height = 1080.0f;
    float dpi_scale = 1.0f; // the window's DPI / 96
    bool ru = true;
    Input::PromptStyle prompts = Input::PromptStyle::Keyboard;
    bool confirm_is_circle = false;         // [System] enterButton = circle: ○ confirms, ✕ goes back
    std::string device_label;               // the active input device
    std::string backend_label;              // the GPU backend of this run
    std::vector<std::string> displays;      // display names
    std::vector<std::string> audio_devices; // playback device names
    std::vector<std::pair<u32, u32>> window_sizes;
    std::string save_note; // changes are not saved / the last save failed
    std::string settings_file; // where changes are saved ("" : not saved in this run)
    // P4.14: the shader cache's statistics (empty: no native backend's cache in this run)
    std::string shader_cache_size, shader_cache_content, shader_cache_run;
    // upscaling (native backend): the scene's render size now, and why an upscaler / frame generation is unavailable
    std::string render_label;
    std::string dlss_why, fsr_why, fsr_fg_why;
    std::string upscale_reason; // an upscaler is chosen but none runs: why
    u64 now_ns = 0;
};

class SettingsMenu {
public:
    static constexpr u64 kConfirmNs = 15'000'000'000; // display changes are reverted unless kept in this time
    // the window's size at scale 1 (a 1080-pixel tall window at 96 DPI); the same for every page
    static constexpr float kWindowW = 900.0f, kWindowH = 700.0f;

    explicit SettingsMenu(Settings::Service& service);

    void Open();
    void Close();
    bool IsOpen() const {
        return open_;
    }
    /// The user asked to close (Back at the top, the close buttons): taken once by the host.
    bool TakeCloseRequest();

    void SetEnv(MenuEnv env);
    void OnNav(Nav nav);
    void OnMouseMove(float x, float y);
    void OnMouseButton(float x, float y, bool down);
    void OnWheel(float notches);
    /// Waiting for a key or mouse button to bind: the host sends them to OnCaptureInput (Esc: Nav::Back cancels).
    bool Capturing() const {
        return capture_.has_value();
    }
    void OnCaptureInput(Input::InputCode input);
    void Tick(u64 now_ns);
    bool NeedsTicks() const; // a countdown is running
    void Build(TextMeasure& measure, DrawList& out);

    Page CurrentPage() const {
        return page_;
    }
    void SelectPage(Page page);
    /// The page of `category` (tests, the host).
    void SelectCategory(Settings::Category category) {
        SelectPage(PageOf(category));
    }
    /// The focused row: a setting id, or "window.size", "reset.page", "info.*", "cmd.*".
    std::string FocusedId() const;
    int FocusedSlot() const {
        return slot_;
    }
    bool ConfirmPending() const {
        return modal_ == Modal::ConfirmDisplay;
    }
    bool ResetPending() const {
        return modal_ == Modal::ConfirmReset;
    }
    bool ListOpen() const {
        return modal_ == Modal::List;
    }
    const std::string& Status() const {
        return status_;
    }
    /// Focus a row by id on the current page (tests, the host).
    bool Focus(std::string_view id);
    /// A command row confirmed (activated twice): its tag ("cmd.clear_shaders", ...), taken once by the host.
    std::string TakeCommand();
    /// Rows of the current page that show a setting (tests: what is listed).
    std::vector<std::string> ListedIds() const;
    /// The choices a drop-down list of `desc` offers now: value, label, whether it can be chosen and why not.
    struct Option {
        std::string value;
        std::string label;
        bool ok = true;
        std::string why;
    };
    std::vector<Option> Options(const Settings::Desc& desc) const;

private:
    struct Row {
        enum class Kind : u8 { Header, Setting, WindowSize, Info, ResetPage, Command };
        Kind kind = Kind::Setting;
        const Settings::Desc* desc = nullptr;
        std::string_view en, ru; // Header/Info/Command label
        std::string tag;         // FocusedId of non-setting rows
    };
    enum class Modal : u8 { None, ConfirmDisplay, ConfirmReset, List };
    struct Capture {
        Input::Action action;
        size_t slot;
    };
    struct Hit {
        enum class Kind : u8 { Page, Row, Close, ModalButton, ListItem, ListOutside, ScrollThumb, ScrollTrack };
        enum class Part : u8 { Body, Prev, Next, Value, Track, Slot0, Slot1, Button };
        Kind kind;
        Rect rect;
        int index = 0;
        Part part = Part::Body;
    };

    std::string_view T(std::string_view en, std::string_view ru) const {
        return env_.ru ? ru : en;
    }
    void RebuildRows();
    void AddCategory(Settings::Category category);
    void AddHeader(std::string_view en, std::string_view ru);
    void AddInfo(std::string_view en, std::string_view ru, std::string tag);
    bool Shown(const Settings::Desc& desc) const;
    bool Focusable(size_t row) const;
    void MoveFocus(int delta);
    void Adjust(int dir);           // Left/Right on the focused row
    void Activate();                // Accept on the focused row
    void ResetFocused();
    void SetValue(const Settings::Desc& desc, const Settings::Value& value);
    void SetWindowSize(u32 w, u32 h);
    void StepWindowSize(int dir);
    std::vector<std::pair<u32, u32>> WindowSizes() const;
    std::vector<std::string> TextOptions(const Settings::Desc& desc) const;
    std::vector<Option> RowOptions(size_t row) const;
    int CurrentOption(size_t row, const std::vector<Option>& options) const;
    void OpenList();
    void ChooseListItem(int index);
    void BeginDisplayChange();      // remember the display settings before a change
    void EndDisplayChange(bool changed);
    void RevertDisplay();
    void ModalAccept(int button);   // 0 = keep / yes, 1 = revert / no
    void ResetPageSettings();
    /// Why a row does not matter now ("" : it does): another setting off, windowed-only, no upscaler...
    std::string Inactive(size_t row) const;
    /// The current value of a choice row cannot run here (a technology this system lacks, a value this build does
    /// not offer): why; "" when it can.
    std::string ChosenWhy(const Settings::Desc& desc) const;
    std::string ValueText(const Settings::Desc& desc, const Settings::Value& value) const;
    std::string BindingText(const Input::InputCode& input) const;
    std::string HelpText(size_t row, Color& color) const;
    void ScrollToFocus();

    Settings::Service& service_;
    MenuEnv env_;
    bool open_ = false;
    bool close_request_ = false;
    Page page_ = Page::Graphics;
    std::vector<Row> rows_;
    size_t focus_ = 0;
    int slot_ = 0;
    float scroll_ = 0;           // the list's offset (layout units)
    bool scroll_to_focus_ = true; // the next Build brings the focused row into view
    float view_h_ = 400;         // the list's visible height (layout units, from the last Build)
    float content_h_ = 0;        // the list's full height
    float scale_ = 1;            // pixels per layout unit (last Build)
    int hover_row_ = -1;
    int hover_page_ = -1;
    int hover_item_ = -1;        // a drop-down list's item or a dialog button under the pointer
    int hover_close_ = -1;       // 0: the header's close button, 1: the footer's
    int dragging_row_ = -1;      // a slider being dragged with the mouse
    bool dragging_thumb_ = false;
    float drag_grab_ = 0;        // pointer y - thumb top when the thumb was taken
    float mouse_x_ = -1, mouse_y_ = -1;
    std::optional<Capture> capture_;
    Modal modal_ = Modal::None;
    int modal_button_ = 0;
    u64 modal_deadline_ns_ = 0;
    size_t list_row_ = 0;        // the row whose drop-down list is open
    int list_item_ = 0;          // its highlighted item
    int list_first_ = 0;         // its first visible item
    std::vector<std::pair<const Settings::Desc*, Settings::Value>> display_before_;
    std::vector<std::pair<const Settings::Desc*, Settings::Value>> pending_display_;
    std::string status_;
    bool status_warn_ = false;
    std::vector<Hit> hits_;
    Rect thumb_{}, track_{};     // the scroll bar of the last Build (pixels)
    std::string command_;      ///< a confirmed command for the host
    std::string armed_;        ///< a command row activated once: the next activation within kConfirmCommandNs runs it
    u64 armed_ns_ = 0;
};

} // namespace SecondSon::Ui
