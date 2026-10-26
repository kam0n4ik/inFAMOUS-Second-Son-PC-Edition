// SecondSonPC — self-tests of the PC settings (see settings_checks.h).
#include "settings/settings_checks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <tuple>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <fmt/format.h>

#include "app/config.h"
#include "engine/render/display_plan.h"
#include "input/binding_rules.h"
#include "input/device_tracker.h"
#include "input/gamepad_tuning.h"
#include "input/keyboard_mouse.h"
#include "settings/experiments.h"
#include "settings/registry.h"
#include "settings/settings_service.h"
#include "settings/user_store.h"
#include "ui/prompt_icons.h"
#include "ui/prompt_textures.h"
#include "ui/settings_menu.h"

namespace SecondSon::Settings {

namespace {

namespace fs = std::filesystem;
using Input::InputCode;

App::Config TestConfig() {
    App::Config c;
    c.language_locale = "ru-RU";
    c.language_id = 8;
    return c;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteText(const fs::path& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

InputCode Key(SDL_Scancode sc) {
    return {InputCode::Kind::Key, static_cast<u16>(sc)};
}

SDL_Event KeyEvent(SDL_Scancode sc, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    return e;
}

void Registry(const CheckFn& check) {
    const auto g = "Settings.registry";
    const auto all = All();
    std::set<std::string_view> ids;
    std::set<std::pair<std::string_view, std::string_view>> keys;
    bool unique_ids = true, unique_keys = true, accessors = true, labels = true, reasons = true;
    for (const auto& d : all) {
        unique_ids = unique_ids && ids.insert(d.id).second;
        if (!d.table.empty()) {
            unique_keys = unique_keys && keys.insert({d.table, d.key}).second;
            accessors = accessors && d.get != nullptr && d.set != nullptr;
        }
        labels = labels && !d.en.empty() && !d.ru.empty();
        if (d.apply == Apply::Unavailable) {
            reasons = reasons && !d.why_en.empty() && !d.why_ru.empty() && d.table.empty();
        }
    }
    check(g, "ids unique", unique_ids, fmt::format("{} settings", all.size()));
    check(g, "toml keys unique", unique_keys, fmt::format("{} stored", keys.size()));
    check(g, "stored settings have accessors", accessors, "");
    check(g, "labels in both languages", labels, "");
    check(g, "unavailable settings say why and store nothing", reasons, "");
    check(g, "every keyboard action has a setting",
          std::ranges::count_if(all, [](const Desc& d) { return d.type == Type::Binding; }) ==
              static_cast<long long>(Input::kActionCount),
          fmt::format("{} actions", Input::kActionCount));

    // the defaults are valid values, and get(set(get)) is the identity
    auto cfg = TestConfig();
    std::string bad;
    for (const auto& d : all) {
        if (d.get == nullptr) {
            continue;
        }
        std::string error;
        const Value v = d.get(cfg, d);
        if (!Validate(d, v, error)) {
            bad += fmt::format("{}: {} ({}); ", d.id, Describe(v), error);
        }
        auto copy = cfg;
        d.set(copy, d, v);
        if (!Equal(d.get(copy, d), v)) {
            bad += fmt::format("{}: round trip; ", d.id);
        }
    }
    check(g, "defaults valid, accessors round-trip", bad.empty(), bad);

    const Desc* vol = Find("audio.volume");
    std::string e;
    check(g, "range: volume 101 refused", vol != nullptr && !Validate(*vol, Value{s64{101}}, e), e);
    const Desc* sens = Find("input.mouseSensitivity");
    check(g, "range: NaN sensitivity refused", sens != nullptr && !Validate(*sens, Value{std::nan("")}, e), e);
    check(g, "range: sensitivity 0.5 accepted", sens != nullptr && Validate(*sens, Value{0.5}, e), e);
    const Desc* pm = Find("display.presentMode");
    check(g, "choice: unknown value refused", pm != nullptr && !Validate(*pm, Value{std::string("vsync")}, e), e);
    // R11 display: the stored values of the older files stay valid, the new list values and a file's own numbers too
    {
        const auto valid = [&e](const Desc* d, std::string_view v) { return d != nullptr && Validate(*d, Value{std::string(v)}, e); };
        const Desc* fl = Find("display.frameRateLimit");
        const Desc* res = Find("display.resolution");
        check(g, "vertical sync: fifo / immediate / mailbox (the files' values) valid",
              valid(pm, "fifo") && valid(pm, "immediate") && valid(pm, "mailbox") && pm->choices.size() == 3, "");
        check(g, "frame rate limit: 30 40 50 60 90 120 144 165 240 console unlimited, and a file's 75 / 1000; 19, 1001, "
                 "\"60fps\" refused",
              valid(fl, "30") && valid(fl, "40") && valid(fl, "50") && valid(fl, "60") && valid(fl, "90") &&
                  valid(fl, "120") && valid(fl, "144") && valid(fl, "165") && valid(fl, "240") && valid(fl, "console") &&
                  valid(fl, "unlimited") && valid(fl, "75") && valid(fl, "1000") && !valid(fl, "19") &&
                  !valid(fl, "1001") && !valid(fl, "60fps") && !valid(fl, ""),
              e);
        check(g, "resolution: a display's own size (1366x768) accepted, 100x100 and \"wide\" refused",
              valid(res, "1366x768") && valid(res, "auto") && !valid(res, "100x100") && !valid(res, "wide"), e);
    }
    const Desc* cross = Find("keyboard.cross");
    check(g, "binding: F10 (hotkey) refused",
          cross != nullptr && !Validate(*cross, Value{Bindings{Key(SDL_SCANCODE_F10)}}, e), e);
    check(g, "binding: empty list accepted (unbound)", cross != nullptr && Validate(*cross, Value{Bindings{}}, e), e);

    // experiments: a changed one reaches the environment at start, the others keep the launcher's value
    {
        const auto keep = [](const char* name) {
            const char* v = std::getenv(name);
            return std::string(v != nullptr ? v : "");
        };
        const std::string spread = keep("SSPC_R10_SPREAD"), ring = keep("SSPC_CP_RING");
        const std::string draws = keep("SSPC_R10_NATIVE_DRAWS"), sources = keep("SSPC_R10_NATIVE_DRAWS_SOURCES");
        _putenv_s("SSPC_CP_RING", "128");
        auto x = TestConfig();
        ReadExperiments(x);
        const Desc* sp = Find(ExperimentId("SSPC_R10_SPREAD"));
        const Desc* nd = Find(ExperimentId("SSPC_R10_NATIVE_DRAWS"));
        const Desc* rg = Find(ExperimentId("SSPC_CP_RING"));
        bool ok = sp != nullptr && nd != nullptr && rg != nullptr && sp->category == Category::Experiments &&
                  rg->get(x, *rg) == Value{std::string("128")};
        if (ok) {
            sp->set(x, *sp, Value{true});
            nd->set(x, *nd, Value{true});
            Overrides o;
            o[std::string(sp->id)] = true;
            o[std::string(nd->id)] = true;
            const auto set = ExportExperiments(x, o);
            ok = set.size() == 3 && keep("SSPC_R10_SPREAD") == "1" && keep("SSPC_R10_NATIVE_DRAWS") == "native" &&
                 keep("SSPC_R10_NATIVE_DRAWS_SOURCES") == "shadow" && keep("SSPC_CP_RING") == "128";
        }
        _putenv_s("SSPC_R10_SPREAD", spread.c_str());
        _putenv_s("SSPC_CP_RING", ring.c_str());
        _putenv_s("SSPC_R10_NATIVE_DRAWS", draws.c_str());
        _putenv_s("SSPC_R10_NATIVE_DRAWS_SOURCES", sources.c_str());
        check(g, "experiments: the user's choice reaches the environment, the launcher's stays", ok, "");
    }
}

void UserFile(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.file";
    Overrides o;
    o["display.fullscreen"] = true;
    o["audio.volume"] = s64{35};
    o["input.mouseSensitivity"] = 1.0;
    o["gamepad.cameraSensitivity"] = 1.25;
    o["display.presentMode"] = std::string("fifo");
    o["display.frameRateLimit"] = std::string("75"); // (R11: a number outside the list, as a hand-edited file has it)
    o["display.resolution"] = std::string("1366x768");
    o["display.monitor"] = std::string("Монитор \"DELL\" \\ 27");
    o["keyboard.cross"] = Bindings{Key(SDL_SCANCODE_SPACE), {InputCode::Kind::MouseButton, SDL_BUTTON_X1}};
    o["keyboard.dpadUp"] = Bindings{{InputCode::Kind::MouseWheel, 0}};
    o["keyboard.shake"] = Bindings{};
    const std::string text = SerializeUserFile(o);
    Overrides back;
    std::vector<std::string> warnings;
    std::string perr;
    const bool parsed = ParseUserText(text, back, warnings, perr);
    check(g, "save -> load round trip (every type, Cyrillic, quotes, mouse X1, wheel, unbound)",
          parsed && back == o && warnings.empty(), parsed ? fmt::format("{} keys, {} warnings", back.size(), warnings.size()) : perr);
    check(g, "a float stays a float (1.0, not 1)", text.find("mouseSensitivity = 1.0") != std::string::npos, "");

    // lenient reading: bad lines skipped, the rest used
    const std::string mixed = "[Display]\nfullscreen = true\nunknownKey = 3\n[Audio]\nvolume = 400\n"
                              "[Input]\ninvertY = \"yes\"\n[Keyboard]\ncross = \"F10\"\ncircle = \"NoSuchKey\"\n"
                              "square = \"Q\"\n[Nonsense]\nx = 1\n";
    back.clear();
    warnings.clear();
    const bool ok = ParseUserText(mixed, back, warnings, perr);
    check(g, "lenient: 2 good keys kept, 6 bad ones skipped with warnings",
          ok && back.size() == 2 && back.contains("display.fullscreen") && back.contains("keyboard.square") &&
              warnings.size() == 6,
          fmt::format("kept {}, warnings {}", back.size(), warnings.size()));
    check(g, "BOM accepted", ParseUserText("\xEF\xBB\xBF[Audio]\nvolume = 10\n", back, warnings, perr) &&
                                 back.size() == 1, perr);
    check(g, "broken TOML detected", !ParseUserText("[Display\nfullscreen = ", back, warnings, perr), perr);

    // keys this version does not take (a newer version's, a hand-edited bad value) are written back by every save
    {
        const std::string newer = "[Display]\nfullscreen = true\nhdrPeakNits = 1000\n[Audio]\nvolume = 400\n"
                                  "[FutureTable]\nmode = \"x\"\nlist = [1, 2]\n";
        Overrides ov;
        KeptKeys kept;
        warnings.clear();
        const bool p = ParseUserText(newer, ov, warnings, perr, &kept);
        const std::string again = SerializeUserFile(ov, kept);
        Overrides ov2;
        KeptKeys kept2;
        std::vector<std::string> w2;
        const bool p2 = ParseUserText(again, ov2, w2, perr, &kept2);
        const auto by_key = [](const KeptKey& a, const KeptKey& b) {
            return std::tie(a.table, a.key) < std::tie(b.table, b.key);
        };
        std::ranges::sort(kept, by_key);
        std::ranges::sort(kept2, by_key);
        check(g, "unknown keys and bad values kept: written back unchanged (nothing of the file is lost)",
              p && p2 && kept.size() == 4 && ov2 == ov && kept2 == kept &&
                  again.find("hdrPeakNits = 1000") != std::string::npos &&
                  again.find("[FutureTable]") != std::string::npos && again.find("volume = 400") != std::string::npos,
              fmt::format("kept {}, again {}", kept.size(), kept2.size()));
        ov["audio.volume"] = s64{40};
        const std::string replaced = SerializeUserFile(ov, kept);
        check(g, "a kept key's setting changed in the menu: the new value replaces the old line",
              replaced.find("volume = 40\n") != std::string::npos &&
                  replaced.find("volume = 400") == std::string::npos,
              "");
    }

    // files
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto file = dir / "settings.toml";
    fs::remove(file, ec);
    Overrides loaded;
    check(g, "missing file: SecondSonPC.toml alone",
          LoadUserFile(file, loaded).status == LoadReport::Status::Missing && loaded.empty(), "");
    std::string werr;
    const bool w1 = WriteFileAtomic(file, "[Audio]\nvolume = 10\n", werr);
    const bool w2 = WriteFileAtomic(file, text, werr);
    auto bak = file;
    bak += ".bak";
    auto tmp = file;
    tmp += ".tmp";
    check(g, "atomic write: new content, previous kept as .bak, no .tmp left",
          w1 && w2 && ReadFile(file) == text && ReadFile(bak) == "[Audio]\nvolume = 10\n" && !fs::exists(tmp, ec),
          werr);
    const auto report = LoadUserFile(file, loaded);
    check(g, "load from disk = what was saved", report.status == LoadReport::Status::Loaded && loaded == o,
          fmt::format("{} keys", loaded.size()));
    WriteText(file, "[Display]\nfullscreen = tru");
    const auto corrupt = LoadUserFile(file, loaded);
    check(g, "corrupt file: moved aside, nothing used, the original name free for the next save",
          corrupt.status == LoadReport::Status::Corrupt && loaded.empty() && fs::exists(corrupt.moved_to, ec) &&
              !fs::exists(file, ec),
          corrupt.moved_to.string());

    // the background writer coalesces a burst (a dragged slider) and writes the newest
    {
        AsyncWriter writer(dir / "burst.toml");
        for (int i = 0; i <= 50; ++i) {
            writer.Save(fmt::format("[Audio]\nvolume = {}\n", i));
        }
        const bool flushed = writer.Flush(5000);
        check(g, "async writer: 51 saves -> the last one on disk, writes coalesced",
              flushed && ReadFile(dir / "burst.toml") == "[Audio]\nvolume = 50\n" && writer.Writes() < 51,
              fmt::format("{} writes", writer.Writes()));
    }
}

void ServiceChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.service";
    const auto defaults = TestConfig();
    Service s;
    const auto file = dir / "service.toml";
    std::error_code ec;
    fs::remove(file, ec);
    s.Init(defaults, {}, defaults, file, Capability::Window | Capability::BridgePresenter | Capability::LiveVolume |
                                              Capability::RealController);
    std::vector<std::string> applied;
    s.AddApplier([&applied](const App::Config&, std::span<const Desc* const> changed) {
        for (const auto* d : changed) {
            applied.emplace_back(d->id);
        }
    });
    const Desc& vol = *Find("audio.volume");
    const Desc& inv = *Find("input.invertY");
    const Desc& pm = *Find("display.presentMode");
    const Desc& rs = *Find("graphics.textures");
    const Desc& sens = *Find("input.mouseSensitivity");

    check(g, "live change applied and recorded",
          s.Set(vol, Value{s64{40}}) == SetResult::Ok && std::get<s64>(s.Get(vol)) == 40 && s.IsOverridden(vol) &&
              applied == std::vector<std::string>{"audio.volume"},
          "");
    check(g, "same value: unchanged, no applier call", s.Set(vol, Value{s64{40}}) == SetResult::Unchanged &&
                                                           applied.size() == 1, "");
    check(g, "out of range refused, value kept",
          s.Set(vol, Value{s64{140}}) == SetResult::Invalid && std::get<s64>(s.Get(vol)) == 40, "");
    check(g, "unavailable setting refused (never a dead switch)",
          s.Set(rs, Value{true}) == SetResult::Unavailable && !s.Available(rs).available, "");
    check(g, "back to the default: override dropped", s.Set(vol, Value{s64{100}}) == SetResult::Ok &&
                                                          !s.IsOverridden(vol), "");
    applied.clear();
    check(g, "restart setting: stored, marked, no live applier",
          s.Set(pm, Value{std::string("fifo")}) == SetResult::Ok && s.NeedsRestart(pm) && applied.empty(), "");
    s.Set(pm, Value{std::string("mailbox")});
    check(g, "restart setting back to the started value: not marked", !s.NeedsRestart(pm), "");
    // R11: the vertical sync is Live where the presenter switches its swapchain (the native backend), Restart else
    {
        const u32 caps = s.Capabilities();
        const bool restart_here = s.EffectiveApply(pm) == Apply::Restart;
        s.SetCapabilities(caps | Capability::PresentModeSwitch);
        applied.clear();
        const bool live = s.EffectiveApply(pm) == Apply::Live && s.Set(pm, Value{std::string("immediate")}) == SetResult::Ok &&
                          applied == std::vector<std::string>{"display.presentMode"} && !s.NeedsRestart(pm) &&
                          s.Current().present_mode == "Immediate";
        s.Set(pm, Value{std::string("mailbox")});
        s.SetCapabilities(caps);
        applied.clear();
        check(g, "vertical sync: live with the native presenter (applier called, no restart mark), restart with the "
                 "bridge's",
              restart_here && live, "");
    }

    s.Set(inv, Value{true});
    s.Set(sens, Value{2.5});
    s.ResetCategory(Category::Controls);
    check(g, "reset a category: its settings back to the defaults",
          !std::get<bool>(s.Get(inv)) && std::get<double>(s.Get(sens)) == 1.0 && !s.IsOverridden(inv) &&
              !s.IsOverridden(sens),
          "");

    // all-or-nothing
    const Desc& w = *Find("display.width");
    const Desc& h = *Find("display.height");
    const std::pair<const Desc*, Value> bad_pair[] = {{&w, Value{s64{1600}}}, {&h, Value{s64{99999}}}};
    check(g, "several settings: one invalid, none changed",
          s.SetMany(bad_pair) == SetResult::Invalid && std::get<s64>(s.Get(w)) == 1280, "");

    // capability missing (--gpu-backend sspc: no bridge presenter)
    s.SetCapabilities(Capability::Window | Capability::LiveVolume | Capability::RealController);
    const Desc& fps = *Find("display.showFps");
    const auto a = s.Available(fps);
    check(g, "a presenter without the FPS counter: the setting unavailable, with the reason",
          !a.available && !a.why_ru.empty() && s.Set(fps, Value{false}) == SetResult::Unavailable,
          std::string(a.why_en));
    s.SetCapabilities(Capability::Window | Capability::BridgePresenter | Capability::LiveVolume |
                      Capability::RealController);

    // persistence: the file holds exactly the overrides
    s.Set(vol, Value{s64{55}});
    s.Set(*Find("keyboard.cross"), Value{Bindings{Key(SDL_SCANCODE_J)}});
    const bool flushed = s.Flush(5000);
    Overrides on_disk;
    const auto rep = LoadUserFile(file, on_disk);
    check(g, "saved off-thread: the file = the overrides",
          flushed && rep.status == LoadReport::Status::Loaded && on_disk == s.CurrentOverrides(),
          fmt::format("{} keys, error \"{}\"", on_disk.size(), s.LastSaveError()));
    // restart: a new process with these overrides starts with them
    auto restarted = defaults;
    ApplyOverrides(restarted, on_disk);
    check(g, "restored at the next start", restarted.volume == 55 &&
                                               restarted.input.bindings[0] == Bindings{Key(SDL_SCANCODE_J)}, "");
}

void ConfigChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.config";
    std::error_code ec;
    const auto root = dir / "root";
    fs::create_directories(root / "game", ec);
    WriteText(root / "game" / "eboot.bin", "x");
    const auto toml = [&](std::string_view extra) {
        WriteText(root / "SecondSonPC.toml",
                  fmt::format("[Game]\npath = '{}'\n[Language]\nsystem = \"en-US\"\n{}", (root / "game").string(),
                              extra));
        std::vector<std::string> errors;
        auto c = App::LoadConfig(root, errors);
        return std::make_pair(std::move(c), errors);
    };
    auto [c1, e1] = toml("[Gamepad]\nvibration = 40\ninvertCameraY = true\n[Interface]\nlanguage = \"en\"\nscale = 125\n"
                         "[Graphics]\nfsr = true\n[Keyboard]\ntouchTwoFingers = [\"C\", \"Wheel Down\"]\n");
    check(g, "SecondSonPC.toml: new sections parsed through the registry",
          c1 && c1->pc.vibration == 40 && c1->pc.invert_camera_y && c1->pc.ui_language == "en" &&
              c1->pc.ui_scale == 125 && c1->pc.fsr &&
              c1->input.bindings[static_cast<size_t>(Input::Action::TouchTwoFingers)].size() == 2,
          e1.empty() ? "" : e1.front());
    auto [c2, e2] = toml("[Gamepad]\nvibration = 400\n");
    check(g, "SecondSonPC.toml: a bad new value is an error (strict as before)", !c2 && !e2.empty(),
          e2.empty() ? "" : e2.front());
    auto [c3, e3] = toml("[Gamepad]\nvibrations = 40\n");
    check(g, "SecondSonPC.toml: a misspelt new key is an error", !c3 && !e3.empty(), e3.empty() ? "" : e3.front());
    auto [c4, e4] = toml("");
    check(g, "SecondSonPC.toml without the new sections: defaults", c4 && c4->pc.vibration == 100 && !c4->pc.fsr,
          e4.empty() ? "" : e4.front());
    // R11 display: what the files had before stays as it was (the first start after the update behaves the same)
    check(g, "SecondSonPC.toml without [Display] keys: vertical sync Mailbox, 60 FPS limit, windowed (as before)",
          c4 && c4->present_mode == "Mailbox" && c4->pc.frame_rate_limit == "60" && !c4->fullscreen &&
              !c4->exclusive_fullscreen,
          "");
    auto [c5, e5] = toml("[Display]\npresentMode = \"immediate\"\nframeRateLimit = \"75\"\nresolution = \"1366x768\"\n"
                         "fullscreen = true\nfullscreenMode = \"exclusive\"\n");
    check(g, "SecondSonPC.toml: immediate, a 75 FPS limit, 1366x768, exclusive full screen kept",
          c5 && c5->present_mode == "Immediate" && c5->pc.frame_rate_limit == "75" && c5->pc.resolution == "1366x768" &&
              c5->fullscreen && c5->exclusive_fullscreen,
          e5.empty() ? "" : e5.front());
    auto [c6, e6] = toml("[Display]\nframeRateLimit = \"7\"\n");
    check(g, "SecondSonPC.toml: a frame rate limit of 7 is an error", !c6 && !e6.empty(), e6.empty() ? "" : e6.front());
}

void BindingChecks(const CheckFn& check) {
    const auto g = "Input.bindings";
    auto s = Input::DefaultSettings();
    check(g, "default layout: no conflicts (Esc = Options + cursor is allowed)", Input::FindConflicts(s).empty(), "");
    Input::AssignBinding(s, Input::Action::Cross, 0, Key(SDL_SCANCODE_E));
    const auto conflicts = Input::FindConflicts(s);
    check(g, "E on Cross: conflict with Circle found",
          conflicts.size() == 1 && conflicts[0].first == Input::Action::Cross &&
              conflicts[0].second == Input::Action::Circle,
          fmt::format("{} conflicts", conflicts.size()));
    check(g, "F11 refused", !Input::AssignBinding(s, Input::Action::Square, 0, Key(SDL_SCANCODE_F11)), "");
    auto t = Input::DefaultSettings();
    Input::AssignBinding(t, Input::Action::R3, 1, Key(SDL_SCANCODE_X));     // replaces Z
    Input::AssignBinding(t, Input::Action::R3, 0, Key(SDL_SCANCODE_X));     // the same input moves, not doubled
    const auto& r3 = t.bindings[static_cast<size_t>(Input::Action::R3)];
    check(g, "assign: an input listed once per action", r3.size() == 1 && r3[0] == Key(SDL_SCANCODE_X),
          fmt::format("{} inputs", r3.size()));
    Input::ClearBinding(t, Input::Action::R3, 0);
    check(g, "clear a slot", t.bindings[static_cast<size_t>(Input::Action::R3)].empty(), "");
    InputCode code;
    check(g, "names: Wheel Up, Mouse X2 parse and print back",
          Input::ParseInput("wheel up", code) && Input::InputName(code) == "Wheel Up" &&
              Input::ParseInput("Mouse X2", code) && Input::InputName(code) == "Mouse X2",
          "");
    check(g, "touchTwoFingers default: C", Input::DefaultSettings().bindings[static_cast<size_t>(
                                               Input::Action::TouchTwoFingers)] == Bindings{Key(SDL_SCANCODE_C)},
          "");
}

void KeyboardMouseChecks(const CheckFn& check) {
    const auto g = "Input.keyboard_mouse";
    Input::KeyboardMouse km;
    auto settings = Input::DefaultSettings();
    settings.bindings[static_cast<size_t>(Input::Action::DpadUp)].push_back({InputCode::Kind::MouseWheel, 0});
    km.Configure(settings);
    km.AttachWindow(nullptr, true); // logical capture, no system cursor
    u64 t = 1'000'000;
    const auto read = [&km, &t] {
        t += 16'667;
        return km.Read(t);
    };
    read();
    km.OnEvent(KeyEvent(SDL_SCANCODE_C, true));
    auto s = read();
    check(g, "C: two fingers on the touch pad and its click",
          (s.buttons & Input::Button::TouchPad) != 0 && s.touch_active && s.touch[0].down && s.touch[1].down &&
              s.touch[0].x == 480 && s.touch[1].x == 1440 && s.touch[0].y == 471 && s.touch[0].id != s.touch[1].id,
          fmt::format("buttons {:#x} ({},{}) ({},{})", s.buttons, s.touch[0].x, s.touch[0].y, s.touch[1].x,
                      s.touch[1].y));
    km.OnEvent(KeyEvent(SDL_SCANCODE_C, false));
    s = read();
    check(g, "C released: fingers lifted", !s.touch_active && (s.buttons & Input::Button::TouchPad) == 0, "");

    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    km.OnEvent(wheel);
    km.OnEvent(wheel); // two notches before one reading: still one press
    s = read();
    const bool pressed = (s.buttons & Input::Button::Up) != 0;
    s = read();
    check(g, "wheel up bound to D-pad up: pressed for one reading", pressed && (s.buttons & Input::Button::Up) == 0, "");

    km.OnEvent(KeyEvent(SDL_SCANCODE_W, true));
    s = read();
    const bool walking = s.ly == 0;
    km.SetMenuOpen(true);
    km.OnEvent(KeyEvent(SDL_SCANCODE_W, false)); // the release happened while the menu had the keyboard
    km.SetMenuOpen(false);
    s = read();
    check(g, "menu opened while W held: nothing stays pressed afterwards", walking && s.ly == 128 && s.lx == 128,
          fmt::format("ly {}", s.ly));

    Input::KeyboardMouse focus;
    focus.Configure(Input::DefaultSettings());
    focus.AttachWindow(nullptr, false);
    focus.OnEvent(KeyEvent(SDL_SCANCODE_G, true));
    focus.OnEvent(KeyEvent(SDL_SCANCODE_G, false));
    u64 ft = 2'000'000;
    const bool motion_on = focus.Read(ft).info.motion_mode;
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focus.OnEvent(lost);
    ft += 20'000;
    check(g, "focus lost: motion mode (G) ends", motion_on && !focus.Read(ft).info.motion_mode, "");
}

void GamepadChecks(const CheckFn& check) {
    const auto g = "Input.gamepad";
    const Input::GamepadTuning neutral;
    bool identity = true;
    for (int x = 0; x < 256; x += 3) {
        for (int y = 0; y < 256; y += 5) {
            u8 tx = static_cast<u8>(x), ty = static_cast<u8>(y);
            Input::TuneRightStick(tx, ty, neutral);
            identity = identity && tx == x && ty == y;
        }
    }
    check(g, "neutral tuning: every stick value unchanged", identity, "");
    Input::GamepadTuning t;
    t.camera_sensitivity = 2.0;
    u8 x = 160, y = 128;
    Input::TuneRightStick(x, y, t);
    check(g, "sensitivity 2: deflection doubled, the resting centre stays 128", x == 192 && y == 128,
          fmt::format("{} {}", x, y));
    x = 255;
    y = 192;
    Input::TuneRightStick(x, y, t);
    check(g, "sensitivity 2 past the edge: clamped, direction kept (y/x ratio)", x == 255 && y == 192,
          fmt::format("{} {}", x, y));
    x = 64;
    y = 128;
    t.camera_sensitivity = 0.5;
    Input::TuneRightStick(x, y, t);
    check(g, "sensitivity 0.5: deflection halved", x == 96 && y == 128, fmt::format("{} {}", x, y));
    Input::GamepadTuning inv;
    inv.invert_camera_y = true;
    x = 40;
    y = 10;
    Input::TuneRightStick(x, y, inv);
    check(g, "invert Y: mirrored, X unchanged", x == 40 && y == 245, fmt::format("{} {}", x, y));
    x = 128;
    y = 128;
    inv.invert_camera_x = true;
    Input::TuneRightStick(x, y, inv);
    check(g, "inverted resting stick stays at the centre (128, 128)", x == 128 && y == 128, fmt::format("{} {}", x, y));
    check(g, "vibration 50 %: half, 0 %: off, 100 %: unchanged",
          Input::ScaleMotor(255, 50) == 127 && Input::ScaleMotor(200, 0) == 0 && Input::ScaleMotor(77, 100) == 77,
          "");
}

void DeviceChecks(const CheckFn& check) {
    const auto g = "Input.device";
    Input::DeviceTracker d;
    constexpr u64 ms = 1'000'000;
    d.PadButton(Input::Device::PlayStation, 1000 * ms);
    const bool to_pad = d.Active() == Input::Device::PlayStation;
    d.MouseMotion(3, 2, 1100 * ms);
    const bool nudge_kept = d.Active() == Input::Device::PlayStation;
    d.MouseMotion(60, 10, 1200 * ms);
    const bool hold_kept = d.Active() == Input::Device::PlayStation; // within 1.5 s of the switch
    d.MouseMotion(60, 10, 2700 * ms);
    const bool moved = d.Active() == Input::Device::KeyboardMouse;
    d.PadAxis(Input::Device::Xbox, 0.2, 2800 * ms);
    const bool jitter_kept = d.Active() == Input::Device::KeyboardMouse;
    d.PadButton(Input::Device::Xbox, 2850 * ms);
    const bool button_now = d.Active() == Input::Device::Xbox;
    d.KeyOrMouseButton(2900 * ms);
    check(g, "switching: buttons at once, nudges/jitter never, mouse movement after the hold",
          to_pad && nudge_kept && hold_kept && moved && jitter_kept && button_now &&
              d.Active() == Input::Device::KeyboardMouse,
          fmt::format("{} switches", d.Switches()));
    Input::DeviceTracker c;
    c.PadConnected(5000 * ms);
    c.PadAxis(Input::Device::Xbox, 0.9, 5100 * ms); // a new controller's first reports
    c.PadButton(Input::Device::Xbox, 5200 * ms);
    const bool quiet = c.Active() == Input::Device::KeyboardMouse;
    c.PadButton(Input::Device::Xbox, 6100 * ms);
    check(g, "a controller just connected switches nothing for 1 s, then its use does",
          quiet && c.Active() == Input::Device::Xbox, "");
    check(g, "hints: auto follows the device, a fixed style wins",
          Input::ResolvePrompts("auto", Input::Device::PlayStation) == Input::PromptStyle::PlayStation &&
              Input::ResolvePrompts("auto", Input::Device::OtherGamepad) == Input::PromptStyle::Xbox &&
              Input::ResolvePrompts("keyboard", Input::Device::PlayStation) == Input::PromptStyle::Keyboard,
          "");
}

/// The game's button icons (ui/prompt_icons.h): the atlas codec on any bytes; with the game's language table at hand
/// (SSPC_GAME_DIR, else E:\shdps4\CUSA00004) the real atlas: found, decoded and encoded back to the same bytes, the
/// three styles built, PNGs of them in `dir` (prompt_*.png) to look at.
void DisplayPlanChecks(const CheckFn& check) {
    namespace R = Engine::Render;
    const auto g = "Display.plan";
    const auto plan = [](u32 w, u32 h, R::AspectMode a = R::AspectMode::Auto, R::Size fixed = {},
                         u32 scale = R::kRenderScaleAuto) { return R::PlanDisplay({w, h}, a, fixed, scale); };
    const auto is = [](R::Size s, u32 w, u32 h) { return s.width == w && s.height == h; };
    const R::DisplayPlan p720 = plan(1280, 720), p1080 = plan(1920, 1080);
    check(g, "16:9 up to 1080p: the PS4's sizes, no extra memory",
          p720.Native() && p1080.Native() && p720.extra_direct_mib == 0 && !p720.boxed && plan(1366, 768).Native(),
          R::Describe(p720));
    const R::DisplayPlan uw = plan(2560, 1080);
    check(g, "2560x1080: display = screen = scene 2560x1080 (Hor+), not boxed",
          is(uw.display, 2560, 1080) && is(uw.screen, 2560, 1080) && is(uw.scene, 2560, 1080) && !uw.boxed,
          R::Describe(uw));
    const R::DisplayPlan uw2 = plan(3440, 1440);
    check(g, "3440x1440: screen 1080 tall in its shape, auto scene at most 1080 lines",
          is(uw2.display, 3440, 1440) && is(uw2.screen, 2580, 1080) && uw2.scene.height == 1080 &&
              uw2.extra_direct_mib > 0,
          R::Describe(uw2));
    const R::DisplayPlan uhd = plan(3840, 2160), uhd100 = plan(3840, 2160, R::AspectMode::Auto, {}, 100);
    check(g, "3840x2160: screen 1920x1080, auto scene 1920x1080; 100 % capped to the verified pixels",
          is(uhd.display, 3840, 2160) && is(uhd.screen, 1920, 1080) && is(uhd.scene, 1920, 1080) &&
              uhd100.scene_capped && u64{uhd100.scene.width} * uhd100.scene.height <= R::kMaxScenePixels &&
              uhd.extra_direct_mib >= 1024,
          R::Describe(uhd100));
    const R::DisplayPlan sw = plan(5120, 1440);
    check(g, "5120x1440 (32:9): screen 3840x1080", is(sw.display, 5120, 1440) && is(sw.screen, 3840, 1080),
          R::Describe(sw));
    const R::DisplayPlan p43 = plan(800, 600), p1610 = plan(1280, 800), portrait = plan(600, 800);
    check(g, "small windows draw 1080 lines: 4:3 1440x1080, 16:10 1728x1080, portrait 16:9 with bars",
          is(p43.display, 1440, 1080) && is(p43.screen, 1440, 1080) && p43.display_raised && !p43.boxed &&
              is(p1610.display, 1728, 1080) && is(portrait.display, 1920, 1080) && portrait.boxed,
          R::Describe(portrait));
    const R::DisplayPlan fixed = plan(1920, 1080, R::AspectMode::Auto, {2560, 1080});
    const R::DisplayPlan forced = plan(2560, 1080, R::AspectMode::R16x9);
    check(g, "a fixed resolution / aspect ratio is boxed into the output",
          is(fixed.display, 2560, 1080) && fixed.boxed && is(forced.display, 1920, 1080) && forced.boxed &&
              plan(7680, 1080).aspect <= 32.0 / 9.0 + 1e-3,
          R::Describe(fixed));
    R::Size parsed;
    R::AspectMode aspect = R::AspectMode::Auto;
    check(g, "parsing: WxH, aspect names, render scale",
          R::ParseSize("3440x1440", parsed) && is(parsed, 3440, 1440) && !R::ParseSize("3440", parsed) &&
              !R::ParseSize("100x100", parsed) && R::ParseAspectMode("21:9", aspect) &&
              aspect == R::AspectMode::R21x9 && !R::ParseAspectMode("5:4", aspect) &&
              R::ParseRenderScale("75") == 75 && R::ParseRenderScale("auto") == R::kRenderScaleAuto &&
              R::ParseRenderScale("40") == R::kRenderScaleAuto,
          "");
}

void PromptIconChecks(const CheckFn& check, const fs::path& dir) {
    namespace P = Ui::PromptIcons;
    const auto g = "Ui.prompts";
    std::vector<u8> tiled(P::kAtlasBytes), rgba(P::kAtlasBytes), back(P::kAtlasBytes);
    u32 seed = 12345;
    for (auto& b : tiled) {
        seed = seed * 1664525u + 1013904223u;
        b = static_cast<u8>(seed >> 24);
    }
    P::Detile(tiled.data(), rgba.data());
    P::Tile(rgba.data(), back.data());
    check(g, "atlas codec: tile(detile(x)) == x (random 1 MiB)", back == tiled, "");
    // element (x, y) of the image: tile (x/8, (511-y)/8), Morton order inside
    const u32 x = 13, y = 500, sy = 511 - y;
    const u32 i = (x & 1) | ((sy & 1) << 1) | ((x & 2) << 1) | ((sy & 2) << 2) | ((x & 4) << 2) | ((sy & 4) << 3);
    const u64 t = ((u64{sy / 8} * 64 + x / 8) * 64 + i) * 4;
    check(g, "atlas codec: 8x8 Morton micro tiles, rows bottom-up",
          std::memcmp(rgba.data() + (u64{y} * 512 + x) * 4, tiled.data() + t, 4) == 0, "");

    char* env = nullptr;
    size_t env_len = 0;
    const bool have_env = _dupenv_s(&env, &env_len, "SSPC_GAME_DIR") == 0 && env != nullptr;
    const fs::path game = have_env ? fs::path(env) : fs::path(L"E:\\shdps4\\CUSA00004");
    std::free(env);
    const fs::path table = game / "art" / "cache" / "lang_russian_text.xpps";
    std::error_code ec;
    if (!fs::exists(table, ec)) {
        check(g, "game atlas: skipped (no game files)", true, table.string());
        return;
    }
    const std::string file = ReadFile(table);
    std::string why;
    const auto info = P::Locate(file, &why);
    check(g, "game atlas: located in lang_russian_text.xpps by its id, descriptor and hash", info.has_value(),
          info ? fmt::format("data @{:#x}, glyphs @{:#x}, {} cells", info->data_offset, info->glyph_offset,
                             info->cells.size())
               : why);
    if (!info) {
        return;
    }
    const u8* orig = reinterpret_cast<const u8*>(file.data()) + info->data_offset;
    P::Detile(orig, rgba.data());
    P::Tile(rgba.data(), back.data());
    check(g, "game atlas: decode -> encode gives the same bytes", std::memcmp(back.data(), orig, P::kAtlasBytes) == 0,
          "");
    const auto cell = [&](u16 code) {
        for (const auto& c : info->cells) {
            if (c.code == code) {
                return c;
            }
        }
        return P::Cell{};
    };
    const P::Cell cross = cell(0xF083), l1 = cell(0xF084);
    check(g, "game atlas: cross 50x50, L1 58x50 (the glyph table)",
          cross.w == 50 && cross.h == 50 && l1.w == 58 && l1.h == 50, "");
    // the cross's circle: opaque at its rim, transparent at its corner (white shapes in alpha)
    const auto alpha = [&](const P::Cell& c, u32 dx, u32 dy) { return rgba[((u64{c.y} + dy) * 512 + c.x + dx) * 4 + 3]; };
    check(g, "game atlas: decoded icons are white shapes in alpha", alpha(cross, 25, 8) > 200 && alpha(cross, 1, 1) == 0,
          fmt::format("{} {}", alpha(cross, 25, 8), alpha(cross, 1, 1)));
    P::SavePng((dir / "prompt_playstation.png").wstring(), rgba.data(), 512, 512);

    const auto layout = Input::DefaultSettings();
    std::string error;
    u32 drawn = 0;
    const auto ps = P::BuildAtlas(orig, info->cells, Input::PromptStyle::PlayStation, layout, &drawn, &error);
    check(g, "PlayStation: the game's atlas byte for byte", ps.size() == P::kAtlasBytes &&
                                                              std::memcmp(ps.data(), orig, P::kAtlasBytes) == 0, "");
    const auto kb = P::BuildAtlas(orig, info->cells, Input::PromptStyle::Keyboard, layout, &drawn, &error);
    check(g, "keyboard: 19 cells redrawn (U+F080..F08D, F090..F094)", kb.size() == P::kAtlasBytes && drawn == 19,
          fmt::format("{} {}", drawn, error));
    if (kb.size() == P::kAtlasBytes) {
        P::Detile(kb.data(), back.data());
        P::SavePng((dir / "prompt_keyboard.png").wstring(), back.data(), 512, 512);
        // outside the redrawn cells nothing changed (the logo, the second set, the cursors)
        const P::Cell logo = cell(0xF095);
        bool same = logo.w != 0;
        for (u32 yy = 0; yy < logo.h && same; ++yy) {
            same = std::memcmp(back.data() + ((u64{logo.y} + yy) * 512 + logo.x) * 4,
                               rgba.data() + ((u64{logo.y} + yy) * 512 + logo.x) * 4, u64{logo.w} * 4) == 0;
        }
        check(g, "keyboard: other glyphs untouched (U+F095 logo)", same, "");
        // the space bar's cell is not the DualShock cross any more, and stays a white shape
        const auto a2 = [&](const P::Cell& c, u32 dx, u32 dy) { return back[((u64{c.y} + dy) * 512 + c.x + dx) * 4 + 3]; };
        check(g, "keyboard: cross -> a wide key (Space), opaque at its edge", a2(cross, 5, 25) > 200 &&
                                                                                  alpha(cross, 5, 25) < 50,
              fmt::format("{} {}", a2(cross, 5, 25), alpha(cross, 5, 25)));
    }
    // a binding changed (cross on F instead of Space): the cell follows
    auto moved = layout;
    moved.bindings[static_cast<size_t>(Input::Action::Cross)] = {Key(SDL_SCANCODE_F)};
    const auto a1 = P::Draw(cross, Input::PromptStyle::Keyboard, layout);
    const auto a3 = P::Draw(cross, Input::PromptStyle::Keyboard, moved);
    check(g, "keyboard: rebinding cross Space -> F changes its icon", a1.drawn && a3.drawn && a1.alpha != a3.alpha, "");
    moved.bindings[static_cast<size_t>(Input::Action::Cross)].clear();
    check(g, "keyboard: an unbound button keeps the DualShock icon",
          !P::Draw(cross, Input::PromptStyle::Keyboard, moved).drawn, "");
    const auto xb = P::BuildAtlas(orig, info->cells, Input::PromptStyle::Xbox, layout, &drawn, &error);
    check(g, "Xbox: 14 cells redrawn (face, shoulders, triggers, sticks, Menu/View; the d-pad stays)",
          xb.size() == P::kAtlasBytes && drawn == 14, fmt::format("{} {}", drawn, error));
    if (xb.size() == P::kAtlasBytes) {
        P::Detile(xb.data(), back.data());
        P::SavePng((dir / "prompt_xbox.png").wstring(), back.data(), 512, 512);
    }
}

/// The game's button pictures (ui/prompt_textures.h): each found at its first place in the game's files, its bytes the
/// game's, the BC3 codec stable on it, the keyboard picture drawn (different bytes) - before/after PNGs in `dir`.
void PromptPictureChecks(const CheckFn& check, const fs::path& dir) {
    namespace T = Ui::PromptTextures;
    const auto g = "Ui.prompt pictures";
    char* env = nullptr;
    size_t env_len = 0;
    const bool have_env = _dupenv_s(&env, &env_len, "SSPC_GAME_DIR") == 0 && env != nullptr;
    const fs::path cache = (have_env ? fs::path(env) : fs::path(L"E:\\shdps4\\CUSA00004")) / "art" / "cache";
    std::free(env);
    const Input::Settings layout = Input::DefaultSettings();
    struct One {
        const char* pack;
        T::PictureKind kind;
        u32 w, h;
        const char* name;
    };
    const One all[] = {{"a8.hires.xpps", T::PictureKind::kSideways256, 256, 256, "sideways"},
                       {"a8.hires.xpps", T::PictureKind::kRattle1024, 1024, 256, "rattle"},
                       {"core_common.sprig.xpps", T::PictureKind::kTouchPrint256, 256, 256, "touchprint"},
                       {"core_common.sprig.xpps", T::PictureKind::kDpad256, 256, 256, "dpad"},
                       {"core_common.sprig.xpps", T::PictureKind::kDpadKarma256, 256, 256, "dpad_karma"},
                       {"core_common.sprig.xpps", T::PictureKind::kDpad64, 64, 64, "dpad64"},
                       {"core_common.sprig.xpps", T::PictureKind::kDiscTriangle128, 128, 128, "disc_triangle"},
                       {"core_common.sprig.xpps", T::PictureKind::kDiscSquare128, 128, 128, "disc_square"},
                       {"core_common.sprig.xpps", T::PictureKind::kDpadRed64, 64, 64, "dpad_red64"}};
    for (const One& o : all) {
        const T::Location* at = nullptr;
        for (const auto* l : T::InPack(o.pack)) {
            at = l->kind == o.kind ? l : at;
        }
        std::vector<u8> orig(T::Bytes(o.kind));
        std::ifstream in(cache / o.pack, std::ios::binary);
        const bool read = at != nullptr && in.seekg(static_cast<std::streamoff>(at->offset)) &&
                          in.read(reinterpret_cast<char*>(orig.data()), static_cast<std::streamsize>(orig.size()));
        if (!read) {
            check(g, fmt::format("{}: in the game's files (skipped: no game files)", o.name), true, "");
            continue;
        }
        check(g, fmt::format("{}: the game's bytes at its place in {}", o.name, o.pack), T::IsOriginal(o.kind, orig.data()),
              "");
        std::vector<u8> rgba(size_t{o.w} * o.h * 4), again(orig.size());
        T::DecodeBc3(orig.data(), o.w, o.h, rgba.data());
        T::EncodeBc3(rgba.data(), o.w, o.h, again.data());
        std::vector<u8> rgba2(rgba.size());
        T::DecodeBc3(again.data(), o.w, o.h, rgba2.data());
        // alpha everywhere; colour where it is seen (a transparent texel's colour is free - the encoder takes the
        // block's colours from its visible texels)
        int worst_a = 0, worst_c = 0;
        for (size_t i = 0; i < rgba.size(); i += 4) {
            worst_a = std::max(worst_a, std::abs(int(rgba[i + 3]) - int(rgba2[i + 3])));
            if (rgba[i + 3] >= 64) {
                for (int c = 0; c < 3; ++c) {
                    worst_c = std::max(worst_c, std::abs(int(rgba[i + c]) - int(rgba2[i + c])));
                }
            }
        }
        check(g, fmt::format("{}: BC3 decode -> encode -> decode: alpha within 24, seen colour within 48", o.name),
              worst_a <= 24 && worst_c <= 48, fmt::format("alpha {} colour {}", worst_a, worst_c));
        const auto kb = T::Build(o.kind, orig.data(), Input::PromptStyle::Keyboard, layout);
        check(g, fmt::format("{}: keyboard picture drawn", o.name), kb.size() == orig.size() && kb != orig, "");
        check(g, fmt::format("{}: PlayStation = the game's own", o.name),
              T::Build(o.kind, orig.data(), Input::PromptStyle::PlayStation, layout) == orig, "");
        const auto save = [&](const std::vector<u8>& tiled, const char* suffix) {
            std::vector<u8> px(size_t{o.w} * o.h * 4), top(px.size());
            T::DecodeBc3(tiled.data(), o.w, o.h, px.data());
            for (u32 y = 0; y < o.h; ++y) { // (stored bottom-up)
                std::memcpy(top.data() + size_t{y} * o.w * 4, px.data() + size_t{o.h - 1 - y} * o.w * 4, size_t{o.w} * 4);
            }
            Ui::PromptIcons::SavePng((dir / fmt::format("picture_{}_{}.png", o.name, suffix)).wstring(), top.data(), o.w,
                                     o.h);
        };
        save(orig, "ps");
        save(kb, "keyboard");
    }
}

void MenuChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Ui.menu";
    const auto defaults = TestConfig();
    Service s;
    constexpr u32 kCaps = Capability::Window | Capability::LiveVolume | Capability::RealController |
                          Capability::RenderScale | Capability::FrameLimiter | Capability::FpsCounter |
                          Capability::FsrFrameGen; // the native backend on a GPU without DLSS
    s.Init(defaults, {}, defaults, {}, kCaps);
    (void)dir;
    Ui::SettingsMenu menu(s);
    Ui::MenuEnv env;
    env.window_sizes = {{1280, 720}, {1600, 900}, {1920, 1080}};
    env.now_ns = 1'000'000'000;
    menu.SetEnv(env);
    menu.Open();
    Ui::EstimateMeasure measure;
    Ui::DrawList list;
    const auto contains = [](const std::vector<std::string>& v, std::string_view id) {
        return std::ranges::find(v, id) != v.end();
    };

    // the window: about 900 x 700 at 1080p, inside every window, scaled with it, the same size on every page
    std::string layout;
    float w1080 = 0, w2160 = 0;
    bool fits = true;
    for (const auto& [w, h] : {std::pair{1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}, {3840.0f, 2160.0f},
                               {800.0f, 600.0f}, {2560.0f, 1080.0f}, {1024.0f, 768.0f}}) {
        auto e = env;
        e.width = w;
        e.height = h;
        menu.SetEnv(e);
        menu.Build(measure, list);
        const auto r = list.Surface();
        fits = fits && r.x >= 0 && r.y >= 0 && r.Right() <= w && r.Bottom() <= h && !list.Commands().empty();
        layout += fmt::format("{}x{}: {:.0f}x{:.0f}; ", w, h, r.w, r.h);
        w1080 = h == 1080.0f && w == 1920.0f ? r.w : w1080;
        w2160 = h == 2160.0f ? r.w : w2160;
    }
    check(g, "window fits and scales (720p..4K, 800x600, 21:9, 4:3)", fits && w2160 > w1080 * 1.8f, layout);
    {
        auto e = env;
        e.width = 1920;
        e.height = 1080;
        menu.SetEnv(e);
        bool same = true;
        Ui::Rect first{};
        for (size_t p = 0; p < Ui::kPageCount; ++p) {
            menu.SelectPage(static_cast<Ui::Page>(p));
            menu.Build(measure, list);
            const auto r = list.Surface();
            if (p == 0) {
                first = r;
            }
            same = same && r.x == first.x && r.y == first.y && r.w == first.w && r.h == first.h;
        }
        check(g, "1080p: 900 x 700 (+ its shadow), the same on every page",
              same && std::abs(first.w - 920.0f) < 1.0f && std::abs(first.h - 720.0f) < 1.0f,
              fmt::format("{:.0f}x{:.0f}", first.w, first.h));
        auto hi = e;
        hi.dpi_scale = 1.5f;
        menu.SetEnv(hi);
        menu.Build(measure, list);
        check(g, "display scale 150 %: the window grows (still inside)", list.Surface().w > first.w * 1.1f &&
                                                                          list.Surface().Right() <= 1920.0f,
              fmt::format("{:.0f}", list.Surface().w));
    }
    menu.SetEnv(env);
    menu.SelectPage(Ui::Page::Graphics);

    check(g, "opens on Graphics, the upscaler focused",
          menu.CurrentPage() == Ui::Page::Graphics && menu.FocusedId() == "upscaling.upscaler", menu.FocusedId());
    const auto graphics = menu.ListedIds();
    check(g, "Graphics: not implemented settings and the bridge's presenter rows are not listed",
          contains(graphics, "graphics.renderScale") && !contains(graphics, "graphics.textures") &&
              !contains(graphics, "graphics.fsr") && !contains(graphics, "upscaling.output") &&
              !contains(graphics, "upscaling.frameGenMultiplier") && contains(graphics, "graphics.smokeQuality") &&
              contains(graphics, "info.render") && contains(graphics, "graphics.motionBlur") &&
              contains(graphics, "graphics.motionBlurStrength") && contains(graphics, "graphics.reflectionQuality") &&
              contains(graphics, "graphics.rainShadows") && !contains(graphics, "graphics.filmGrain") &&
              !contains(graphics, "graphics.depthOfField"),
          fmt::format("{} rows", graphics.size()));

    // only what this system runs: DLSS is offered greyed with the reason, DLSS frame generation not at all
    const Desc& up = *Find("upscaling.upscaler");
    const auto ups = menu.Options(up);
    const bool dlss_greyed = std::ranges::any_of(ups, [](const auto& o) { return o.value == "dlss" && !o.ok && !o.why.empty(); });
    menu.Focus("upscaling.upscaler");
    menu.OnNav(Ui::Nav::Right); // off -> (dlss skipped) -> fsr
    check(g, "upscaler: DLSS greyed with why on a GPU without it, Right skips to FSR",
          dlss_greyed && s.Current().pc.upscaler == "fsr", s.Current().pc.upscaler);
    const auto fgs = menu.Options(*Find("upscaling.frameGeneration"));
    check(g, "frame generation: DLSS FG (not in this build) not offered",
          std::ranges::none_of(fgs, [](const auto& o) { return o.value == "dlss"; }) && fgs.size() == 2, "");

    // a drop-down list: opened with Enter, moved, chosen; Esc closes it without a change
    menu.Focus("upscaling.quality");
    const auto q0 = s.Current().pc.upscale_quality;
    menu.OnNav(Ui::Nav::Accept);
    const bool opened = menu.ListOpen();
    menu.OnNav(Ui::Nav::Down);
    menu.OnNav(Ui::Nav::Back);
    const bool unchanged = !menu.ListOpen() && s.Current().pc.upscale_quality == q0;
    menu.OnNav(Ui::Nav::Accept);
    menu.OnNav(Ui::Nav::Down);
    menu.OnNav(Ui::Nav::Accept);
    check(g, "drop-down list: Esc closes it unchanged, Enter chooses (quality -> balanced)",
          opened && unchanged && !menu.ListOpen() && s.Current().pc.upscale_quality == "balanced",
          s.Current().pc.upscale_quality);
    menu.Build(measure, list); // (a list closed by Close: nothing chosen)
    menu.OnNav(Ui::Nav::Accept);
    menu.OnNav(Ui::Nav::Down);
    menu.Close();
    menu.Open();
    check(g, "closed with a list open: nothing chosen, the page kept for the session",
          s.Current().pc.upscale_quality == "balanced" && !menu.ListOpen() && menu.CurrentPage() == Ui::Page::Graphics &&
              menu.FocusedId() == "upscaling.quality",
          menu.FocusedId());

    menu.OnNav(Ui::Nav::NextTab);
    menu.OnNav(Ui::Nav::NextTab);
    check(g, "pages: Graphics -> Display -> Controls", menu.CurrentPage() == Ui::Page::Controls, "");
    menu.Focus("input.invertY");
    menu.OnNav(Ui::Nav::Accept);
    const bool toggled = std::get<bool>(s.Get(*Find("input.invertY")));
    menu.OnNav(Ui::Nav::Reset);
    check(g, "toggle with Enter, back to the default with Del",
          toggled && !std::get<bool>(s.Get(*Find("input.invertY"))), "");
    menu.Focus("input.mouseSensitivity");
    menu.OnNav(Ui::Nav::Right);
    menu.OnNav(Ui::Nav::Right);
    const double sens = std::get<double>(s.Get(*Find("input.mouseSensitivity")));
    check(g, "slider: two steps right = +0.10", std::abs(sens - 1.10) < 1e-9, fmt::format("{}", sens));

    menu.SelectPage(Ui::Page::Keys);
    menu.Focus("keyboard.cross");
    menu.OnNav(Ui::Nav::Accept);
    const bool capturing = menu.Capturing();
    menu.OnCaptureInput(Key(SDL_SCANCODE_E));
    const auto cross = std::get<Bindings>(s.Get(*Find("keyboard.cross")));
    check(g, "rebind: Enter, then E -> Cross = E, the conflict with Circle reported",
          capturing && !menu.Capturing() && cross == Bindings{Key(SDL_SCANCODE_E)} && !menu.Status().empty(),
          menu.Status());
    menu.OnNav(Ui::Nav::Accept);
    menu.OnCaptureInput(Key(SDL_SCANCODE_F10));
    check(g, "rebind to F10 refused (a SecondSonPC hotkey)",
          std::get<Bindings>(s.Get(*Find("keyboard.cross"))) == Bindings{Key(SDL_SCANCODE_E)}, menu.Status());
    menu.OnNav(Ui::Nav::Accept);
    menu.OnNav(Ui::Nav::Back);
    check(g, "Esc during capture cancels it", !menu.Capturing(), "");
    menu.OnNav(Ui::Nav::Reset);
    check(g, "Del: the action's default keys", std::get<Bindings>(s.Get(*Find("keyboard.cross"))) ==
                                                  Bindings{Key(SDL_SCANCODE_SPACE)}, "");

    // display changes are confirmed or reverted
    menu.SelectPage(Ui::Page::Display);
    {
        // R11: the page - one "Window mode" row for fullscreen + fullscreenMode, then size, monitor, the frames
        const auto ids = menu.ListedIds();
        const auto at = [&ids](std::string_view id) { return std::ranges::find(ids, id) - ids.begin(); };
        check(g, "Display: window mode, window size, monitor, vertical sync, FPS limit, resolution (no separate full "
                 "screen rows)",
              contains(ids, "window.mode") && !contains(ids, "display.fullscreen") &&
                  !contains(ids, "display.fullscreenMode") && at("window.mode") < at("window.size") &&
                  at("window.size") < at("display.monitor") && at("display.monitor") < at("display.presentMode") &&
                  at("display.presentMode") < at("display.frameRateLimit") &&
                  at("display.frameRateLimit") < at("display.resolution") && contains(ids, "display.hudArea"),
              fmt::format("{} rows", ids.size()));
        const auto modes = menu.WindowModeOptions();
        check(g, "window mode: windowed and borderless offered, exclusive not (no Vulkan exclusive mode)",
              modes.size() == 2 && modes[0].value == "window" && modes[1].value == "borderless", "");
    }
    menu.Focus("window.mode");
    menu.OnNav(Ui::Nav::Right); // windowed -> borderless
    const bool asked = menu.ConfirmPending() && std::get<bool>(s.Get(*Find("display.fullscreen")));
    menu.Tick(env.now_ns + Ui::SettingsMenu::kConfirmNs + 1);
    check(g, "display change: confirmation asked, reverted after 15 s without an answer",
          asked && !menu.ConfirmPending() && !std::get<bool>(s.Get(*Find("display.fullscreen"))), menu.Status());
    menu.OnNav(Ui::Nav::Right);
    menu.OnNav(Ui::Nav::Accept); // "Keep" is focused first
    check(g, "display change: kept when confirmed",
          !menu.ConfirmPending() && std::get<bool>(s.Get(*Find("display.fullscreen"))) &&
              std::get<std::string>(s.Get(*Find("display.fullscreenMode"))) == "borderless",
          menu.Status());
    menu.Focus("window.size");
    menu.OnNav(Ui::Nav::Right);
    const auto cfg = s.Current();
    menu.OnNav(Ui::Nav::Accept);
    check(g, "window size: next preset sets width and height together",
          cfg.window_width == 1600 && cfg.window_height == 900, fmt::format("{}x{}", cfg.window_width, cfg.window_height));
    menu.Focus("window.mode");
    menu.OnNav(Ui::Nav::Left); // borderless -> windowed
    menu.Close(); // closed while asking: the safe side
    menu.Open();
    check(g, "closed during the confirmation: the display change is reverted",
          std::get<bool>(s.Get(*Find("display.fullscreen"))) && !menu.ConfirmPending(), "");
    // the monitor: a list of the system's displays (with their size and refresh rate), confirmed as the others
    {
        auto e = env;
        e.displays = {"DELL U2414H", "DELL U2414H (2)"};
        e.display_labels = {"DELL U2414H — 1920 × 1080, 60 Гц (основной)", "DELL U2414H (2) — 2560 × 1440, 144 Гц"};
        e.primary_label = e.display_labels[0];
        menu.SetEnv(e);
        const auto mons = menu.Options(*Find("display.monitor"));
        menu.Focus("display.monitor");
        menu.OnNav(Ui::Nav::Right);
        const bool moved = menu.ConfirmPending() && s.Current().pc.monitor == "DELL U2414H";
        menu.OnNav(Ui::Nav::Back); // Esc in the dialog: revert
        check(g, "monitor: the displays listed with size and refresh rate (two of one model told apart), a change "
                 "confirmed or reverted",
              mons.size() == 3 && mons[2].label.find("144") != std::string::npos && moved &&
                  s.Current().pc.monitor.empty() && !menu.ConfirmPending(),
              mons.size() == 3 ? mons[0].label : "");
        menu.SetEnv(env);
    }
    // the frames: vertical sync and the frame rate limit (no confirmation: nothing of the window changes)
    {
        auto e = env;
        e.present_known = true;
        e.present_immediate = false; // a surface without IMMEDIATE
        e.present_mailbox = true;
        e.present_supported = "FIFO, MAILBOX";
        e.present_now = "MAILBOX";
        menu.SetEnv(e);
        const auto vs = menu.Options(*Find("display.presentMode"));
        const bool fallback_said = vs.size() == 3 && vs[1].value == "immediate" &&
                                   vs[1].label.find("Mailbox") != std::string::npos && vs[1].ok;
        menu.Focus("display.presentMode");
        menu.OnNav(Ui::Nav::Left); // (wraps) -> the previous choice
        const bool changed = !menu.ConfirmPending() && s.Current().present_mode != "Mailbox";
        menu.OnNav(Ui::Nav::Reset);
        check(g, "vertical sync: on / off / off without tearing; Off says what this surface does instead; no "
                 "confirmation; Del back to the default",
              fallback_said && changed && s.Current().present_mode == "Mailbox", s.Current().present_mode);
        menu.SetEnv(env);
        const Desc& fl = *Find("display.frameRateLimit");
        const auto lims = menu.Options(fl);
        s.Set(fl, Value{std::string("75")});
        const auto lims75 = menu.Options(fl);
        menu.Focus("display.frameRateLimit");
        menu.OnNav(Ui::Nav::Right); // from the file's 75 to the list's first
        const bool stepped = s.Current().pc.frame_rate_limit != "75";
        s.Reset(fl);
        check(g, "frame rate limit: 40 and 50 offered, a file's 75 listed while chosen and left with the arrows",
              std::ranges::any_of(lims, [](const auto& o) { return o.value == "40"; }) &&
                  std::ranges::any_of(lims, [](const auto& o) { return o.value == "50"; }) &&
                  std::ranges::none_of(lims, [](const auto& o) { return o.value == "75"; }) &&
                  std::ranges::any_of(lims75, [](const auto& o) { return o.value == "75"; }) && stepped &&
                  s.Current().pc.frame_rate_limit == "60",
              s.Current().pc.frame_rate_limit);
    }
    // a file's exclusive full screen: shown as chosen, with why it is not offered; the arrows leave it
    {
        const Desc& fm = *Find("display.fullscreenMode");
        const Desc& fs = *Find("display.fullscreen");
        s.Set(fm, Value{std::string("exclusive")});
        s.Set(fs, Value{true});
        const auto modes = menu.WindowModeOptions();
        const bool listed = modes.size() == 3 && modes[2].value == "exclusive" && !modes[2].ok && !modes[2].why.empty();
        menu.Focus("window.mode");
        menu.OnNav(Ui::Nav::Left); // exclusive -> borderless
        menu.OnNav(Ui::Nav::Accept); // keep
        check(g, "exclusive from the file: listed greyed with why; Left goes to borderless",
              listed && s.Current().fullscreen && !s.Current().exclusive_fullscreen && !menu.ConfirmPending(), "");
    }

    menu.Focus("reset.page");
    menu.OnNav(Ui::Nav::Accept);
    const bool reset_asked = menu.ResetPending();
    menu.OnNav(Ui::Nav::Left);
    menu.OnNav(Ui::Nav::Accept);
    const auto after = s.Current();
    menu.OnNav(Ui::Nav::Accept); // keep the reset display settings
    check(g, "reset the page: asked (Cancel first), then defaults",
          reset_asked && !after.fullscreen && after.window_width == 1280,
          fmt::format("fullscreen {}, {}x{}", after.fullscreen, after.window_width, after.window_height));

    // restart settings: stored now, shown as not in use yet
    menu.SelectPage(Ui::Page::Game);
    menu.Focus("game.enterButton");
    menu.OnNav(Ui::Nav::Right);
    check(g, "a restart setting: stored, marked as waiting for a restart, the window says so",
          s.NeedsRestart(*Find("game.enterButton")) && !menu.Status().empty(), menu.Status());
    menu.OnNav(Ui::Nav::Left);

    menu.SelectPage(Ui::Page::Advanced);
    check(g, "Advanced: the FPS counter listed and available (a presenter that has it)",
          contains(menu.ListedIds(), "display.showFps") && s.Available(*Find("display.showFps")).available, "");
    menu.OnNav(Ui::Nav::Back);
    check(g, "Esc at the top asks the host to close", menu.TakeCloseRequest(), "");

    // the bridge (--gpu-backend bridge): its presenter's rows listed, the native backend's greyed with the reason
    s.SetCapabilities(Capability::Window | Capability::BridgePresenter | Capability::LiveVolume |
                      Capability::RealController | Capability::FpsCounter);
    menu.SelectPage(Ui::Page::Graphics);
    menu.Focus("upscaling.frameGeneration");
    menu.OnNav(Ui::Nav::Right);
    check(g, "--gpu-backend bridge: FSR 1.0 listed, frame generation unavailable and says why",
          contains(menu.ListedIds(), "graphics.fsr") && s.Current().pc.frame_generation == "off" &&
              !menu.Status().empty(),
          menu.Status());
}

} // namespace

/// R11 postfx: the game's motion blur and its strength as settings (film grain and depth of field not offered).
void PostFxChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.postfx";
    const auto defaults = TestConfig();
    Service s;
    const auto file = dir / "postfx.toml";
    std::error_code ec;
    fs::remove(file, ec);
    s.Init(defaults, {}, defaults, file, Capability::Window | Capability::LiveVolume);
    std::vector<std::string> applied;
    s.AddApplier([&applied](const App::Config&, std::span<const Desc* const> changed) {
        for (const auto* d : changed) {
            applied.emplace_back(d->id);
        }
    });
    const Desc* mb = Find("graphics.motionBlur");
    const Desc* st = Find("graphics.motionBlurStrength");
    const Desc* dof = Find("graphics.depthOfField");
    check(g, "motion blur and its strength exist, live, in [Graphics]; no film grain, depth of field unavailable",
          mb && st && dof && !Find("graphics.filmGrain") && mb->apply == Apply::Live && st->apply == Apply::Live &&
              dof->apply == Apply::Unavailable && mb->table == "Graphics" && st->key == "motionBlurStrength" &&
              s.Available(*mb).available && !s.Available(*dof).available,
          "");
    if (!(mb && st)) {
        return;
    }
    check(g, "defaults: the game's own (motion blur on at 100 %)",
          std::get<bool>(s.Get(*mb)) && std::get<s64>(s.Get(*st)) == 100 && !s.IsOverridden(*mb), "");
    check(g, "motion blur off: applied live, the strength greyed (requires it)",
          s.Set(*mb, Value{false}) == SetResult::Ok && !s.Current().pc.motion_blur && st->requires_on == mb->id &&
              applied == std::vector<std::string>{"graphics.motionBlur"},
          "");
    check(g, "strength: 50 % taken, 5 % and 120 % refused",
          s.Set(*st, Value{s64{50}}) == SetResult::Ok && s.Current().pc.motion_blur_strength == 50 &&
              s.Set(*st, Value{s64{5}}) == SetResult::Invalid && s.Set(*st, Value{s64{120}}) == SetResult::Invalid,
          "");
    const bool flushed = s.Flush(5000);
    Overrides on_disk;
    const auto rep = LoadUserFile(file, on_disk);
    auto restarted = defaults;
    ApplyOverrides(restarted, on_disk);
    check(g, "saved and restored at the next start",
          flushed && rep.status == LoadReport::Status::Loaded && !restarted.pc.motion_blur &&
              restarted.pc.motion_blur_strength == 50,
          fmt::format("{} keys", on_disk.size()));
    s.ResetCategory(Category::Graphics);
    check(g, "reset Graphics: back to the game's own",
          s.Current().pc.motion_blur && s.Current().pc.motion_blur_strength == 100 && !s.IsOverridden(*mb) &&
              !s.IsOverridden(*st),
          "");
}

// R12 refl: [Graphics] reflectionQuality (docs/R12_REFLECTIONS.md)
void ReflectionChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.reflections";
    const auto defaults = TestConfig();
    Service s;
    const auto file = dir / "reflections.toml";
    std::error_code ec;
    fs::remove(file, ec);
    s.Init(defaults, {}, defaults, file, Capability::Window | Capability::LiveVolume);
    std::vector<std::string> applied;
    s.AddApplier([&applied](const App::Config&, std::span<const Desc* const> changed) {
        for (const auto* d : changed) {
            applied.emplace_back(d->id);
        }
    });
    const Desc* rq = Find("graphics.reflectionQuality");
    check(g, "reflection quality exists, a live choice in [Graphics], available on either backend",
          rq && rq->apply == Apply::Live && rq->type == Type::Choice && rq->table == "Graphics" &&
              rq->key == "reflectionQuality" && rq->category == Category::Graphics && s.Available(*rq).available,
          "");
    if (!rq) {
        return;
    }
    check(g, "levels: original and low only (no level that is not measured)",
          rq->choices.size() == 2 && rq->choices[0].value == "original" && rq->choices[1].value == "low", "");
    check(g, "default: the game's own (original)",
          std::get<std::string>(s.Get(*rq)) == "original" && defaults.pc.reflection_quality == "original" &&
              !s.IsOverridden(*rq),
          "");
    check(g, "low: applied live, an unknown value refused",
          s.Set(*rq, Value{std::string("low")}) == SetResult::Ok && s.Current().pc.reflection_quality == "low" &&
              applied == std::vector<std::string>{"graphics.reflectionQuality"} &&
              s.Set(*rq, Value{std::string("ultra")}) == SetResult::Invalid,
          s.Current().pc.reflection_quality);
    const bool flushed = s.Flush(5000);
    Overrides on_disk;
    const auto rep = LoadUserFile(file, on_disk);
    auto restarted = defaults;
    ApplyOverrides(restarted, on_disk);
    check(g, "saved and restored at the next start",
          flushed && rep.status == LoadReport::Status::Loaded && restarted.pc.reflection_quality == "low",
          fmt::format("{} keys", on_disk.size()));
    s.ResetCategory(Category::Graphics);
    check(g, "reset Graphics: back to the game's own",
          s.Current().pc.reflection_quality == "original" && !s.IsOverridden(*rq), "");
}

// R12 rain: [Graphics] rainShadows (docs/R12_RAIN_SHADOWS.md)
void RainShadowChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.rainShadows";
    const auto defaults = TestConfig();
    Service s;
    const auto file = dir / "rain_shadows.toml";
    std::error_code ec;
    fs::remove(file, ec);
    s.Init(defaults, {}, defaults, file, Capability::Window | Capability::LiveVolume);
    std::vector<std::string> applied;
    s.AddApplier([&applied](const App::Config&, std::span<const Desc* const> changed) {
        for (const auto* d : changed) {
            applied.emplace_back(d->id);
        }
    });
    const Desc* rs = Find("graphics.rainShadows");
    check(g, "rain shadows exist, a live choice in [Graphics], available on either backend",
          rs && rs->apply == Apply::Live && rs->type == Type::Choice && rs->table == "Graphics" &&
              rs->key == "rainShadows" && rs->category == Category::Graphics && s.Available(*rs).available,
          "");
    if (!rs) {
        return;
    }
    check(g, "levels: original and off only (no level that is not measured)",
          rs->choices.size() == 2 && rs->choices[0].value == "original" && rs->choices[1].value == "off", "");
    check(g, "default: the game's own (original)",
          std::get<std::string>(s.Get(*rs)) == "original" && defaults.pc.rain_shadows == "original" &&
              !s.IsOverridden(*rs),
          "");
    check(g, "off: applied live, an unknown value refused",
          s.Set(*rs, Value{std::string("off")}) == SetResult::Ok && s.Current().pc.rain_shadows == "off" &&
              applied == std::vector<std::string>{"graphics.rainShadows"} &&
              s.Set(*rs, Value{std::string("low")}) == SetResult::Invalid,
          s.Current().pc.rain_shadows);
    const bool flushed = s.Flush(5000);
    Overrides on_disk;
    const auto rep = LoadUserFile(file, on_disk);
    auto restarted = defaults;
    ApplyOverrides(restarted, on_disk);
    check(g, "saved and restored at the next start",
          flushed && rep.status == LoadReport::Status::Loaded && restarted.pc.rain_shadows == "off",
          fmt::format("{} keys", on_disk.size()));
    s.ResetCategory(Category::Graphics);
    check(g, "reset Graphics: back to the game's own",
          s.Current().pc.rain_shadows == "original" && !s.IsOverridden(*rs), "");
}

void RunChecks(const CheckFn& check, const std::filesystem::path& scratch) {
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    Registry(check);
    UserFile(check, scratch);
    ServiceChecks(check, scratch);
    PostFxChecks(check, scratch);
    ReflectionChecks(check, scratch);
    RainShadowChecks(check, scratch);
    ConfigChecks(check, scratch);
    BindingChecks(check);
    KeyboardMouseChecks(check);
    GamepadChecks(check);
    DeviceChecks(check);
    DisplayPlanChecks(check);
    PromptIconChecks(check, scratch);
    PromptPictureChecks(check, scratch);
    MenuChecks(check, scratch);
}

} // namespace SecondSon::Settings
