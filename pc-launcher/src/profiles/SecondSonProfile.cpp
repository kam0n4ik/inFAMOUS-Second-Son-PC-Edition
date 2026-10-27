// inFAMOUS: Second Son — PC Edition (CUSA00004). Sources of every value: core\src\app\product.h (names, title id),
// core\src\launcher\launch.cpp kStandard / kExperimental (= play.cmd / play_experimental.cmd), SecondSonPC.toml.
#include "profiles/GameProfile.h"

namespace Launcher {

GameProfile MakeSecondSonProfile() {
    GameProfile p;
    p.id = QStringLiteral("second_son");
    p.productId = QStringLiteral("SecondSonPC");
    p.gameName = QStringLiteral("inFAMOUS: Second Son");
    p.shortName = QStringLiteral("Second Son");
    p.brand = QStringLiteral("SECOND SON");
    p.windowTitle = QStringLiteral("inFAMOUS: Second Son — PC Edition");
    p.titleIds = {QStringLiteral("CUSA00004")};
    p.exeName = QStringLiteral("SecondSonPC.exe");
    p.configFileName = QStringLiteral("SecondSonPC.toml");
    p.exeSearchDirs = {QStringLiteral("."), QStringLiteral("build/bin")};
    p.logFileName = QStringLiteral("SecondSonPC.log");
    p.themeId = QStringLiteral("second_son");
    p.artCandidates = {QStringLiteral("sce_sys/pic0.png"), QStringLiteral("sce_sys/pic1.png")};
    p.launchArgs = {QStringLiteral("--no-launcher")};
    p.standardEnv = {
        {"SSPC_CP_MODE", "native"},          {"SSPC_CITY_PERFORMANCE", "1"},    {"SSPC_REA_MERGE_RUNS", "1"},
        {"SSPC_CP_ROUND_NODRAIN", "1"},      {"SSPC_P411_ASYNC_ACQUIRE", "1"},  {"SSPC_ASYNC_PIPELINES", "1"},
        {"SSPC_P53_FRONTEND_THREAD", "1"},   {"SSPC_CP_ASYNC_WRITES", "1"},     {"SSPC_CP_ASYNC_FLIP", "1"},
        {"SSPC_VK_EMIT_THREAD", "1"},        {"SSPC_VK_EMIT_BATCH", "1"},       {"SSPC_CP_RING", "128"},
        {"SSPC_NATIVE_GBUFFER", "native"},   {"SSPC_R9_PCACHE", "1"},           {"SSPC_R9_HEAP_WINDOW", "1"},
        {"SSPC_R10_AJM", "native"},          {"SSPC_R10_SYSRT", "native"},      {"SSPC_R11_EARLY_SUBMIT", "256"},
    };
    p.experimentalEnv = {
        {"SSPC_REA_DEFER_WAIT_B", "1"}, {"SSPC_FB_ASYNC_COMPUTE", "1"},   {"SSPC_R10_AQ_MAIN_FREE", "1"},
        {"SSPC_R11_AQ_IMAGES", "1"},    {"SSPC_R11_AQ_ORDER_EPOCH", "1"},
    };
    p.caps = GameCaps::EngineHooks | GameCaps::RenderSwitch | GameCaps::ResolutionPatch | GameCaps::FreeFrameRate |
             GameCaps::CameraHooks;
    return p;
}

} // namespace Launcher
