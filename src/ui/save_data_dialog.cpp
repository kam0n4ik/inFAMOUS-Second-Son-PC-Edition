// SecondSonPC — the save data dialog over the game (see save_data_dialog.h).
#include "ui/save_data_dialog.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include <fmt/format.h>

namespace SecondSon::Ui {

namespace {

using SaveData::Dialog::Choice;
using SaveData::Dialog::Mode;
using SaveData::Dialog::SysMsg;
using SaveData::Dialog::Type;

constexpr Color kPanel{22, 24, 30, 244};
constexpr Color kBorder{255, 255, 255, 40};
constexpr Color kText{255, 255, 255, 255};
constexpr Color kDim{170, 176, 190, 255};
constexpr Color kRow{255, 255, 255, 12};
constexpr Color kRowHover{255, 255, 255, 26};
constexpr Color kAccent{88, 166, 255, 255};
constexpr Color kAccentFill{88, 166, 255, 56};
constexpr Color kError{255, 110, 100, 255};

std::string SizeText(u64 bytes, bool ru) {
    const double kib = static_cast<double>(bytes) / 1024.0;
    std::string s = kib >= 1024.0 * 1024.0 ? fmt::format("{:.2f} {}", kib / 1024.0 / 1024.0, ru ? "ГБ" : "GB")
                    : kib >= 1024.0        ? fmt::format("{:.1f} {}", kib / 1024.0, ru ? "МБ" : "MB")
                                           : fmt::format("{:.0f} {}", std::max(kib, 1.0), ru ? "КБ" : "KB");
    if (ru) {
        std::replace(s.begin(), s.end(), '.', ',');
    }
    return s;
}

std::string DateText(s64 mtime) {
    if (mtime <= 0) {
        return {};
    }
    const std::time_t t = static_cast<std::time_t>(mtime);
    std::tm tm{};
    if (localtime_s(&tm, &t) != 0) {
        return {};
    }
    return fmt::format("{:02}.{:02}.{:04} {:02}:{:02}", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour,
                       tm.tm_min);
}

} // namespace

void SaveDataDialogView::Open(Request request, std::vector<std::shared_ptr<const Image>> icons) {
    request_ = std::move(request);
    icons_ = std::move(icons);
    icons_.resize(request_.items.size());
    choices_ = SaveData::Dialog::Choices(request_);
    focus_ = std::min(request_.focus, request_.items.empty() ? size_t{0} : request_.items.size() - 1);
    // a question starts on its first button (Yes), as the console's
    choice_ = 0;
    scroll_ = 0;
    scroll_to_focus_ = true;
    hover_ = -1;
    pressed_ = false;
    hits_.clear();
    answer_.reset();
    open_ = true;
}

void SaveDataDialogView::Close() {
    open_ = false;
    hits_.clear();
    answer_.reset();
    icons_.clear();
}

std::optional<SaveDataDialogView::Answer> SaveDataDialogView::TakeAnswer() {
    auto a = answer_;
    answer_.reset();
    return a;
}

float SaveDataDialogView::Scale() const {
    return std::max(env_.height / 1080.0f, 0.5f);
}

std::string SaveDataDialogView::Title() const {
    switch (request_.type) {
    case Type::Save: return std::string(T("Save", "Сохранение"));
    case Type::Load: return std::string(T("Load", "Загрузка"));
    case Type::Delete: return std::string(T("Delete", "Удаление"));
    }
    return {};
}

std::string SaveDataDialogView::MessageText() const {
    const Type type = request_.type;
    const auto by_type = [&](std::string_view save, std::string_view load, std::string_view del) {
        return std::string(type == Type::Save ? save : type == Type::Load ? load : del);
    };
    const std::string space = fmt::format(
        "{}", SizeText(request_.sys_value * 32768, env_.ru)); // (blocks of 32 KiB)
    switch (request_.mode) {
    case Mode::UserMsg:
        return request_.text;
    case Mode::SystemMsg:
        switch (request_.sys) {
        case SysMsg::NoData: return std::string(T("There is no saved data.", "Нет сохранённых данных."));
        case SysMsg::Confirm:
            return env_.ru ? by_type("Сохранить?", "Загрузить эти сохранённые данные?", "Удалить эти сохранённые данные?")
                           : by_type("Do you want to save?", "Do you want to load this saved data?",
                                     "Do you want to delete this saved data?");
        case SysMsg::Overwrite:
            return std::string(T("Do you want to overwrite this saved data?", "Перезаписать эти сохранённые данные?"));
        case SysMsg::NoSpace:
        case SysMsg::NoSpaceContinuable:
            return env_.ru ? fmt::format("Недостаточно места для сохранения. Нужно освободить {}.", space)
                           : fmt::format("There is not enough space to save the data. {} of free space is required.",
                                         space);
        case SysMsg::Progress:
            return env_.ru ? by_type("Сохранение…", "Загрузка…", "Удаление…")
                           : by_type("Saving…", "Loading…", "Deleting…");
        case SysMsg::FileCorrupted: return std::string(T("The saved data is corrupted.", "Сохранённые данные повреждены."));
        case SysMsg::Finished:
            return env_.ru ? by_type("Сохранение завершено.", "Загрузка завершена.", "Удаление завершено.")
                           : by_type("Saving complete.", "Loading complete.", "Deletion complete.");
        case SysMsg::CorruptedAndDeleted:
            return std::string(T("The saved data is corrupted and will be deleted.",
                                 "Сохранённые данные повреждены и будут удалены."));
        case SysMsg::CorruptedAndCreated:
            return std::string(T("The saved data is corrupted. It will be deleted and new saved data will be created.",
                                 "Сохранённые данные повреждены. Они будут удалены, и будут созданы новые."));
        case SysMsg::CorruptedAndRestore:
            return std::string(T("The saved data is corrupted. The backup will be restored.",
                                 "Сохранённые данные повреждены. Будет восстановлена резервная копия."));
        case SysMsg::TotalSizeExceeded:
            return std::string(T("No more saved data can be created.", "Больше нельзя создать сохранённые данные."));
        }
        return fmt::format("{} ({})", T("System message", "Системное сообщение"), static_cast<u32>(request_.sys));
    case Mode::ErrorCode:
        if (request_.error_code == 0x809F0008) {
            return std::string(T("There is no saved data.", "Нет сохранённых данных."));
        }
        if (request_.error_code == 0x809F000F) {
            return std::string(T("The saved data is corrupted.", "Сохранённые данные повреждены."));
        }
        return fmt::format("{} ({:08X})", T("An error has occurred.", "Произошла ошибка."), request_.error_code);
    case Mode::ProgressBar:
        return !request_.text.empty() ? request_.text
                                      : (env_.ru ? by_type("Сохранение…", "Загрузка…", "Удаление…")
                                                 : by_type("Saving…", "Loading…", "Deleting…"));
    default:
        return {};
    }
}

std::string SaveDataDialogView::ChoiceLabel(Choice c) const {
    switch (c) {
    case Choice::Ok: return "OK";
    case Choice::Yes: return std::string(T("Yes", "Да"));
    case Choice::No: return std::string(T("No", "Нет"));
    case Choice::Cancel: return std::string(T("Cancel", "Отмена"));
    }
    return {};
}

std::string SaveDataDialogView::HintText() const {
    const bool back = SaveData::Dialog::CanGoBack(request_);
    std::string accept, cancel;
    switch (env_.prompts) {
    case Input::PromptStyle::PlayStation:
        accept = env_.confirm_is_circle ? "○" : "✕";
        cancel = env_.confirm_is_circle ? "✕" : "○";
        break;
    case Input::PromptStyle::Xbox:
        accept = env_.confirm_is_circle ? "B" : "A";
        cancel = env_.confirm_is_circle ? "A" : "B";
        break;
    default:
        accept = "Enter";
        cancel = "Esc";
        break;
    }
    const bool can_accept = IsList() ? !request_.items.empty() : !choices_.empty();
    std::string s;
    if (can_accept) {
        s = fmt::format("{} — {}", accept, IsList() ? T("choose", "выбрать") : T("confirm", "подтвердить"));
    }
    if (back) {
        s += fmt::format("{}{} — {}", s.empty() ? "" : "     ", cancel, T("back", "назад"));
    }
    return s;
}

void SaveDataDialogView::DrawItem(DrawList& out, Rect r, size_t index, bool focused, bool hovered) const {
    const float s = Scale();
    const auto& item = request_.items[index];
    out.FillRect(r, focused ? kAccentFill : hovered ? kRowHover : kRow, 8 * s);
    if (focused) {
        out.StrokeRect(r, kAccent, 2 * s, 8 * s);
    }
    const float pad = 10 * s;
    const float icon_h = r.h - 2 * pad;
    const Rect icon{r.x + pad, r.y + pad, icon_h * 16.0f / 9.0f, icon_h};
    if (index < icons_.size() && icons_[index] != nullptr) {
        out.DrawImage(icon, icons_[index]);
    } else {
        out.FillRect(icon, {60, 62, 70, 255}, 4 * s);
        if (item.is_new) {
            const float cx = icon.x + icon.w / 2, cy = icon.y + icon.h / 2, a = icon.h * 0.22f;
            out.Line(cx - a, cy, cx + a, cy, kText, 3 * s);
            out.Line(cx, cy - a, cx, cy + a, kText, 3 * s);
        }
    }
    const float x = icon.Right() + 16 * s, w = r.Right() - x - pad;
    float y = r.y + pad;
    std::string title = item.title;
    if (item.is_new && title.empty()) {
        title = T("New saved data", "Новые сохранённые данные");
    }
    out.Text({x, y, w, 28 * s}, title, 21 * s, kText, Align::Left, FontWeight::Semibold);
    y += 30 * s;
    if (item.is_new) {
        return;
    }
    const std::string date_size = fmt::format("{}     {}", DateText(item.mtime), SizeText(item.size_bytes, env_.ru));
    const bool subtitle_first = request_.item_style == 1;
    const bool show_subtitle = request_.item_style != 2 && !item.subtitle.empty();
    if (item.corrupted) {
        out.Text({x, y, w, 22 * s}, T("Corrupted", "Повреждено"), 16 * s, kError);
        y += 22 * s;
    }
    if (subtitle_first && show_subtitle) {
        out.Text({x, y, w, 22 * s}, item.subtitle, 16 * s, kDim);
        y += 22 * s;
    }
    out.Text({x, y, w, 22 * s}, date_size, 16 * s, kDim);
    y += 22 * s;
    if (!subtitle_first && show_subtitle && y + 22 * s <= r.Bottom()) {
        out.Text({x, y, w, 22 * s}, item.subtitle, 16 * s, kDim);
    }
}

void SaveDataDialogView::Build(TextMeasure& measure, DrawList& out) {
    (void)measure;
    hits_.clear();
    const float s = Scale();
    const float W = env_.width, H = env_.height;
    out.Reset(W, H);
    out.SetDim(120);
    if (IsList()) {
        const float pw = std::min(1100 * s, W - 40 * s), ph = std::min(780 * s, H - 40 * s);
        const Rect panel{std::floor((W - pw) / 2), std::floor((H - ph) / 2), pw, ph};
        out.SetSurface(panel);
        out.FillRect(panel, kPanel, 14 * s);
        out.StrokeRect(panel, kBorder, 1 * s, 14 * s);
        const float x = panel.x + 28 * s, iw = panel.w - 56 * s;
        out.Text({x, panel.y + 18 * s, iw, 40 * s}, Title(), 30 * s, kText, Align::Left, FontWeight::Semibold);
        const size_t saves = static_cast<size_t>(
            std::count_if(request_.items.begin(), request_.items.end(), [](const auto& i) { return !i.is_new; }));
        out.Text({x, panel.y + 60 * s, iw, 24 * s},
                 fmt::format("{}: {}", T("Saved data", "Сохранённые данные"), saves), 16 * s, kDim);
        const Rect list{x, panel.y + 96 * s, iw, panel.Bottom() - 64 * s - (panel.y + 96 * s)};
        row_h_ = 112 * s;
        view_h_ = list.h;
        const float content = static_cast<float>(request_.items.size()) * row_h_;
        if (scroll_to_focus_ && !request_.items.empty()) {
            const float top = static_cast<float>(focus_) * row_h_;
            if (top < scroll_) {
                scroll_ = top;
            } else if (top + row_h_ > scroll_ + view_h_) {
                scroll_ = top + row_h_ - view_h_;
            }
            scroll_to_focus_ = false;
        }
        scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - view_h_));
        if (request_.items.empty()) {
            out.Text({list.x, list.y + list.h / 2 - 20 * s, list.w, 40 * s},
                     T("There is no saved data.", "Нет сохранённых данных."), 22 * s, kDim, Align::Center);
        }
        out.PushClip(list);
        for (size_t i = 0; i < request_.items.size(); ++i) {
            const Rect r{list.x, list.y + static_cast<float>(i) * row_h_ - scroll_, list.w - 14 * s, row_h_ - 8 * s};
            if (r.Bottom() < list.y || r.y > list.Bottom()) {
                continue;
            }
            const size_t hit = hits_.size();
            hits_.push_back({Hit::Kind::Item, r, i});
            DrawItem(out, r, i, i == focus_, hover_ == static_cast<int>(hit));
        }
        out.PopClip();
        if (content > view_h_) {
            const Rect track{list.Right() - 6 * s, list.y, 6 * s, list.h};
            out.FillRect(track, {255, 255, 255, 20}, 3 * s);
            const float th = std::max(40 * s, list.h * view_h_ / content);
            const float ty = list.y + (list.h - th) * (scroll_ / (content - view_h_));
            out.FillRect({track.x, ty, track.w, th}, {255, 255, 255, 90}, 3 * s);
        }
        const float fy = panel.Bottom() - 50 * s;
        if (SaveData::Dialog::CanGoBack(request_)) {
            const Rect back{panel.Right() - 28 * s - 150 * s, fy, 150 * s, 36 * s};
            const size_t hit = hits_.size();
            hits_.push_back({Hit::Kind::Back, back, 0});
            out.FillRect(back, hover_ == static_cast<int>(hit) ? kRowHover : kRow, 6 * s);
            out.Text(back, T("Back", "Назад"), 17 * s, kText, Align::Center);
        }
        out.Text({x, fy, iw - 190 * s, 36 * s}, HintText(), 15 * s, kDim);
        return;
    }

    // a message
    const bool has_item = !request_.items.empty();
    const bool bar = request_.mode == Mode::ProgressBar;
    const float pw = std::min(720 * s, W - 40 * s);
    const float ph = std::min((has_item ? 420 : 300) * s + (bar ? 30 * s : 0), H - 40 * s);
    const Rect panel{std::floor((W - pw) / 2), std::floor((H - ph) / 2), pw, ph};
    out.SetSurface(panel);
    out.FillRect(panel, kPanel, 14 * s);
    out.StrokeRect(panel, kBorder, 1 * s, 14 * s);
    const float x = panel.x + 28 * s, iw = panel.w - 56 * s;
    out.Text({x, panel.y + 18 * s, iw, 36 * s}, Title(), 24 * s, kText, Align::Left, FontWeight::Semibold);
    float y = panel.y + 66 * s;
    if (has_item) {
        DrawItem(out, {x, y, iw, 104 * s}, 0, false, false);
        y += 120 * s;
    }
    const float buttons_y = panel.Bottom() - 76 * s;
    const bool error = (request_.mode == Mode::UserMsg && request_.error_text) || request_.mode == Mode::ErrorCode;
    const float text_bottom = bar ? buttons_y - 30 * s : buttons_y - 10 * s;
    out.Text({x, y, iw, text_bottom - y}, MessageText(), 20 * s, error ? kError : kText, Align::Center,
             FontWeight::Regular, true);
    if (bar) {
        const Rect track{x, text_bottom - 6 * s, iw, 10 * s};
        out.FillRect(track, {255, 255, 255, 36}, 5 * s);
        out.FillRect({track.x, track.y, track.w * std::min(request_.progress, 100u) / 100.0f, track.h}, kAccent, 5 * s);
    }
    if (!choices_.empty()) {
        const float bw = 160 * s, bh = 44 * s, gap = 16 * s;
        const float total = static_cast<float>(choices_.size()) * bw + static_cast<float>(choices_.size() - 1) * gap;
        float bx = panel.x + (panel.w - total) / 2;
        for (size_t i = 0; i < choices_.size(); ++i) {
            const Rect b{bx, buttons_y, bw, bh};
            const size_t hit = hits_.size();
            hits_.push_back({Hit::Kind::Choice, b, i});
            const bool focused = i == choice_;
            out.FillRect(b, focused ? kAccentFill : hover_ == static_cast<int>(hit) ? kRowHover : kRow, 8 * s);
            if (focused) {
                out.StrokeRect(b, kAccent, 2 * s, 8 * s);
            }
            out.Text(b, ChoiceLabel(choices_[i]), 18 * s, kText, Align::Center, FontWeight::Semibold);
            bx += bw + gap;
        }
    }
    if (const std::string hint = HintText(); !hint.empty()) {
        out.Text({x, panel.Bottom() - 18 * s, iw, 16 * s}, hint, 13 * s, kDim, Align::Center);
    }
}

void SaveDataDialogView::MoveFocus(int delta) {
    if (request_.items.empty()) {
        return;
    }
    const int n = static_cast<int>(request_.items.size());
    focus_ = static_cast<size_t>(std::clamp(static_cast<int>(focus_) + delta, 0, n - 1));
    scroll_to_focus_ = true;
}

void SaveDataDialogView::Choose() {
    if (IsList()) {
        if (!request_.items.empty()) {
            answer_ = Answer{.kind = Answer::Kind::Item, .item = focus_};
        }
    } else if (choice_ < choices_.size()) {
        answer_ = Answer{.kind = Answer::Kind::Choice, .choice = choices_[choice_]};
    }
}

void SaveDataDialogView::OnNav(Nav nav) {
    if (!open_) {
        return;
    }
    switch (nav) {
    case Nav::Up:
        if (IsList()) {
            MoveFocus(-1);
        }
        break;
    case Nav::Down:
        if (IsList()) {
            MoveFocus(1);
        }
        break;
    case Nav::PageUp:
        if (IsList()) {
            MoveFocus(-std::max(1, static_cast<int>(view_h_ / std::max(row_h_, 1.0f))));
        }
        break;
    case Nav::PageDown:
        if (IsList()) {
            MoveFocus(std::max(1, static_cast<int>(view_h_ / std::max(row_h_, 1.0f))));
        }
        break;
    case Nav::Left:
        if (!IsList() && choice_ > 0) {
            --choice_;
        }
        break;
    case Nav::Right:
        if (!IsList() && choice_ + 1 < choices_.size()) {
            ++choice_;
        }
        break;
    case Nav::Accept:
        Choose();
        break;
    case Nav::Back:
        if (SaveData::Dialog::CanGoBack(request_)) {
            answer_ = Answer{.kind = Answer::Kind::Back};
        }
        break;
    default:
        break;
    }
}

void SaveDataDialogView::OnMouseMove(float x, float y) {
    mouse_x_ = x;
    mouse_y_ = y;
    hover_ = -1;
    for (size_t i = 0; i < hits_.size(); ++i) {
        if (hits_[i].rect.Contains(x, y)) {
            hover_ = static_cast<int>(i);
            break;
        }
    }
    // the pointer over a list item or a button focuses it (as the keyboard would)
    if (hover_ >= 0) {
        const Hit& h = hits_[static_cast<size_t>(hover_)];
        if (h.kind == Hit::Kind::Item) {
            focus_ = h.index;
        } else if (h.kind == Hit::Kind::Choice) {
            choice_ = h.index;
        }
    }
}

void SaveDataDialogView::OnMouseButton(float x, float y, bool down) {
    OnMouseMove(x, y);
    if (down) {
        pressed_ = hover_ >= 0;
        return;
    }
    if (!pressed_ || hover_ < 0) {
        pressed_ = false;
        return;
    }
    pressed_ = false;
    const Hit h = hits_[static_cast<size_t>(hover_)];
    switch (h.kind) {
    case Hit::Kind::Item:
        focus_ = h.index;
        Choose();
        break;
    case Hit::Kind::Choice:
        choice_ = h.index;
        Choose();
        break;
    case Hit::Kind::Back:
        OnNav(Nav::Back);
        break;
    }
}

void SaveDataDialogView::OnWheel(float notches) {
    if (!IsList()) {
        return;
    }
    scroll_ -= notches * row_h_;
    scroll_to_focus_ = false;
    hover_ = -1; // (the rows moved under the pointer: found again at the next move)
}

} // namespace SecondSon::Ui
