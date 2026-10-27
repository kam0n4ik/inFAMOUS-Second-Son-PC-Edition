import QtQuick
import QtQuick.Window
import Launcher

// The launcher's window: frameless, 1280 x 760 at start, 960 x 600 at least. The game's art and the ambient layer
// under one of three shells (concepts A / B / C, App.concept) sharing the same pages and data.
Window {
    id: win
    width: 1280
    height: 760
    minimumWidth: 960
    minimumHeight: 600
    visible: true
    flags: Qt.Window | Qt.FramelessWindowHint
    color: Theme.bg
    title: App.hasGame ? App.gameName + " — PC Launcher" : "inFAMOUS PC Launcher"

    property string page: App.initialPage
    readonly property bool shot: App.screenshotMode
    readonly property bool animate: active && visibility !== Window.Minimized && visibility !== Window.Hidden
                                    && !Theme.reduced && !Game.gameRunning && App.hasGame && !shot

    Binding { target: Theme; property: "themeId"; value: App.themeId }
    Binding { target: Theme; property: "reduced"; value: App.reducedEffects }
    Binding { target: Theme; property: "windowWidth"; value: content.width }
    Binding { target: Theme; property: "windowHeight"; value: content.height }
    Binding { target: Theme; property: "backdrop"; value: art.blurredItem }
    Binding { target: Theme; property: "overlay"; value: overlay }

    Connections {
        target: App
        function onRequestMinimize() { win.showMinimized() }
        function onRequestRestore() {
            if (win.visibility === Window.Minimized) win.showNormal()
            win.raise()
            win.requestActivate()
        }
    }
    onClosing: (close) => { close.accepted = true } // the game, if running, goes on

    // a resize settles, then the glass panels re-sample their part of the backdrop
    Timer { id: settle; interval: 60; onTriggered: Theme.layoutTick++ }
    onWidthChanged: settle.restart()
    onHeightChanged: settle.restart()

    Item {
        id: content
        width: win.shot && App.contentSize.width > 0 ? App.contentSize.width : win.width
        height: win.shot && App.contentSize.height > 0 ? App.contentSize.height : win.height
        clip: true

        ArtBackground {
            id: art
            anchors.fill: parent
            visible: App.hasGame
            artOpacity: App.concept === "desktop" ? 0.22 : 1
            artScale: App.concept === "cinematic" && win.page === "home" && !Theme.reduced ? 1.035 : 1.0
            alignment: App.concept === "neon" ? Image.AlignRight : Image.AlignHCenter
        }
        Rectangle {
            anchors.fill: parent
            visible: !App.hasGame
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.bg2 }
                GradientStop { position: 1; color: Theme.bg }
            }
        }
        AmbientLayer {
            anchors.fill: parent
            visible: App.hasGame && !Theme.reduced && App.concept !== "desktop" && (win.page === "home" || App.concept === "cinematic")
            running: win.animate
            time: win.shot ? 14.0 : 0
            intensity: win.page === "home" ? 1.0 : 0.45
        }

        Loader {
            id: shell
            anchors.fill: parent
            active: App.hasGame
            sourceComponent: App.concept === "neon" ? neonC : App.concept === "desktop" ? desktopC : cinematicC
            onLoaded: Theme.layoutTick++
        }
        Component { id: cinematicC; CinematicShell { page: win.page; onNavigate: (p) => win.page = p } }
        Component { id: neonC; NeonShell { page: win.page; onNavigate: (p) => win.page = p } }
        Component { id: desktopC; DesktopShell { page: win.page; onNavigate: (p) => win.page = p } }

        // no game, or more than one: the detection's own screens
        Loader {
            anchors.fill: parent
            active: !App.hasGame
            sourceComponent: App.status === "ambiguous" ? ambiguousC : notFoundC
        }
        Component { id: notFoundC; NotFoundView {} }
        Component { id: ambiguousC; AmbiguityView {} }
        TitleBar {
            anchors.left: parent.left
            anchors.right: parent.right
            visible: !App.hasGame
        }

        Item {
            id: overlay
            anchors.fill: parent
            z: 50
        }
        ToastHost {
            anchors.fill: parent
            z: 60
        }
    }

    ResizeFrame { visible: !win.shot && win.visibility === Window.Windowed }

    Shortcut { sequence: "Ctrl+1"; onActivated: win.page = "home" }
    Shortcut { sequence: "Ctrl+2"; onActivated: win.page = "settings" }
    Shortcut { sequence: "Ctrl+3"; onActivated: win.page = "saves" }
    Shortcut { sequence: "Ctrl+4"; onActivated: win.page = "diagnostics" }
    Shortcut { sequence: "Ctrl+S"; enabled: GameSettings.dirty; onActivated: GameSettings.save() }

    // --screenshot: render once the art, the pages and the diagnostics have settled, save, exit
    Timer {
        running: win.shot
        interval: 2600
        onTriggered: App.takeScreenshot(content)
    }
}
