pragma Singleton
import QtQuick

// The launcher's look for the game it serves (docs/UI_DESIGN.md). One set of components, three palettes:
// second_son (red / orange / graphite / white), first_light (neon pink / purple / graphite / white), and a neutral
// one while no game is found. Main.qml sets themeId, reduced, the window size (for the scale) and the backdrop.
QtObject {
    id: theme

    property string themeId: ""
    property bool reduced: false
    property real windowWidth: 1280
    property real windowHeight: 760
    property Item backdrop: null   // the blurred art the glass panels show (ArtBackground)
    property int layoutTick: 0     // bumped when glass panels must re-sample their part of the backdrop
    property Item overlay: null    // the layer the dialogs open in (the whole window's content)

    readonly property bool secondSon: themeId === "second_son"
    readonly property bool firstLight: themeId === "first_light"

    // a mild interface scale: the 1280 x 760 design grows on large windows (4K at 100 %), never below 0.85
    readonly property real k: Math.max(0.85, Math.min(1.75, Math.min(windowWidth / 1280, windowHeight / 760)))
    function px(v) { return Math.round(v * k) }

    // ---- palette
    readonly property color bg: firstLight ? "#0b0a12" : secondSon ? "#0d0c0e" : "#0e0f12"
    readonly property color bg2: firstLight ? "#151225" : secondSon ? "#18161a" : "#171a20"
    readonly property color surface: firstLight ? Qt.rgba(0.09, 0.07, 0.15, 0.72) : secondSon ? Qt.rgba(0.09, 0.085, 0.09, 0.72) : Qt.rgba(0.09, 0.1, 0.12, 0.74)
    readonly property color surfaceSolid: firstLight ? "#171427" : secondSon ? "#1a181b" : "#1a1d23"
    readonly property color surfaceHover: firstLight ? Qt.rgba(1, 0.85, 1, 0.07) : Qt.rgba(1, 1, 1, 0.06)
    readonly property color stroke: firstLight ? Qt.rgba(0.85, 0.7, 1, 0.12) : Qt.rgba(1, 1, 1, 0.09)
    readonly property color strokeStrong: firstLight ? Qt.rgba(0.95, 0.75, 1, 0.22) : Qt.rgba(1, 1, 1, 0.18)
    readonly property color text: firstLight ? "#f5f1ff" : "#f5f2ef"
    readonly property color textDim: firstLight ? "#b4abd0" : secondSon ? "#b3aca6" : "#aab0bb"
    readonly property color textFaint: firstLight ? "#7a7196" : secondSon ? "#7b7470" : "#737a86"
    readonly property color accent: firstLight ? "#ff3fa4" : secondSon ? "#ea3b2a" : "#7aa2ff"
    readonly property color accent2: firstLight ? "#8b5cff" : secondSon ? "#ff8a2b" : "#9fb8ff"
    readonly property color glow: firstLight ? "#ff4fd8" : secondSon ? "#ff6a1f" : "#7aa2ff"
    readonly property color textOnAccent: "#ffffff"
    readonly property color ok: "#58d08f"
    readonly property color warn: "#f3b544"
    readonly property color danger: "#ff5b52"
    readonly property color info: firstLight ? "#c9a8ff" : "#9fb8ff"

    // ---- type
    readonly property string display: "Roboto Condensed"
    readonly property string body: "Segoe UI"
    readonly property string mono: "Cascadia Mono, Consolas"
    readonly property int fontSmall: px(12)
    readonly property int fontBody: px(14)
    readonly property int fontLarge: px(16)
    readonly property int fontTitle: px(22)
    readonly property int fontHero: px(84)

    // ---- shape and motion (150..350 ms, eased)
    readonly property int radiusS: px(6)
    readonly property int radiusM: px(10)
    readonly property int radiusL: px(16)
    readonly property int fast: reduced ? 0 : 150
    readonly property int normal: reduced ? 0 : 230
    readonly property int slow: reduced ? 0 : 340
    readonly property int easing: Easing.OutCubic
}
