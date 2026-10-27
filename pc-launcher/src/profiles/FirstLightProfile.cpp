// inFAMOUS: First Light — PC Edition (CUSA00897, checked in sce_sys\param.sfo). Sources: core\src\app\product.h
// (engine_hooks = false), core\src\launcher\launch.cpp kStandard / kExperimental for SSPC_PRODUCT_FIRST_LIGHT
// (= FirstLight's play.cmd), FirstLightPC.toml, docs\COMPATIBILITY_MATRIX.md and docs\TODO.md of First Light.
#include "profiles/GameProfile.h"

namespace Launcher {

GameProfile MakeFirstLightProfile() {
    GameProfile p;
    p.id = QStringLiteral("first_light");
    p.productId = QStringLiteral("FirstLightPC");
    p.gameName = QStringLiteral("inFAMOUS: First Light");
    p.shortName = QStringLiteral("First Light");
    p.brand = QStringLiteral("FIRST LIGHT");
    p.windowTitle = QStringLiteral("inFAMOUS: First Light — PC Edition");
    p.titleIds = {QStringLiteral("CUSA00897")};
    p.exeName = QStringLiteral("FirstLightPC.exe");
    p.configFileName = QStringLiteral("FirstLightPC.toml");
    p.exeSearchDirs = {QStringLiteral("."), QStringLiteral("build/bin")};
    p.logFileName = QStringLiteral("FirstLightPC.log");
    p.themeId = QStringLiteral("first_light");
    p.artCandidates = {QStringLiteral("sce_sys/pic0.png"), QStringLiteral("sce_sys/pic1.png")};
    p.launchArgs = {QStringLiteral("--no-launcher")};
    // Second Son's preset without what acts on Second Son's engine (NATIVE_GBUFFER, CITY_PERFORMANCE's engine parts,
    // early submission) and without R11_EARLY_SUBMIT
    p.standardEnv = {
        {"SSPC_CP_MODE", "native"},          {"SSPC_CITY_PERFORMANCE", "1"},   {"SSPC_CITY_ENGINE_OPT", "0"},
        {"SSPC_CITY_ENGINE_GBUFFER_RUNS", "0"}, {"SSPC_P54_EARLY_SUBMIT", "0"}, {"SSPC_REA_MERGE_RUNS", "1"},
        {"SSPC_CP_ROUND_NODRAIN", "1"},      {"SSPC_P411_ASYNC_ACQUIRE", "1"}, {"SSPC_ASYNC_PIPELINES", "1"},
        {"SSPC_P53_FRONTEND_THREAD", "1"},   {"SSPC_CP_ASYNC_WRITES", "1"},    {"SSPC_CP_ASYNC_FLIP", "1"},
        {"SSPC_VK_EMIT_THREAD", "1"},        {"SSPC_VK_EMIT_BATCH", "1"},      {"SSPC_CP_RING", "128"},
        {"SSPC_R9_PCACHE", "1"},             {"SSPC_R9_HEAP_WINDOW", "1"},     {"SSPC_R10_AJM", "native"},
        {"SSPC_R10_SYSRT", "native"},
    };
    // the asynchronous compute lane (REA_DEFER_WAIT_B is a Second Son engine hook: not here)
    p.experimentalEnv = {
        {"SSPC_FB_ASYNC_COMPUTE", "1"}, {"SSPC_R10_AQ_MAIN_FREE", "1"}, {"SSPC_R11_AQ_IMAGES", "1"},
        {"SSPC_R11_AQ_ORDER_EPOCH", "1"},
    };
    // render size, HUD, FOV and anti-aliasing have First Light's own sites (render_resolution.cpp); the game clock
    // (0x14bd60) is real time with dt <= 1/30 s, so frame rates above 60 are offered
    p.caps = GameCaps::ResolutionPatch | GameCaps::RenderSwitch | GameCaps::CameraHooks | GameCaps::FreeFrameRate;
    p.hooksReasonRu = QStringLiteral(
        "На First Light эта настройка не работает: хуки эффектов, теней и текстур есть только для движка Second Son "
        "(product.h: all_engine_hooks = false). Адреса First Light ещё ищутся.");
    p.hooksReasonEn = QStringLiteral(
        "This setting does not work on First Light: the effects, shadows and textures hooks exist for Second Son's "
        "engine only (product.h: all_engine_hooks = false). First Light's addresses are still being found.");
    p.renderSwitchReasonRu = QStringLiteral("Нужен переключатель размера рендера движка.");
    p.renderSwitchReasonEn = QStringLiteral("Needs the engine's render size switch.");
    p.frameRateReasonRu = QStringLiteral("Выше 60 к/с на First Light не включается.");
    p.frameRateReasonEn = QStringLiteral("Above 60 FPS is not offered on First Light.");
    p.unverifiedSettings = {QStringLiteral("display.resolution"),         QStringLiteral("display.aspectRatio"),
                            QStringLiteral("display.hudSafeArea"),        QStringLiteral("display.hudArea"),
                            QStringLiteral("graphics.renderScale"),       QStringLiteral("upscaling.upscaler"),
                            QStringLiteral("upscaling.quality"),          QStringLiteral("upscaling.sharpness"),
                            QStringLiteral("upscaling.frameGeneration"),  QStringLiteral("upscaling.frameGenMultiplier"),
                            QStringLiteral("graphics.antiAliasing"),      QStringLiteral("graphics.fov")};
    p.unverifiedReasonRu = QStringLiteral("Сделано в коде для First Light, в игре ещё не проверено.");
    p.unverifiedReasonEn = QStringLiteral("Done in code for First Light, not yet checked in play.");
    return p;
}

} // namespace Launcher
