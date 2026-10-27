import QtQuick
import QtQuick.Shapes
import QtQuick.Layouts
import QtQuick.Effects
import Launcher

// Concept B, Neon Control: a slanted control panel over the left of the art, a neon edge, the facts as tiles.
Item {
    id: home
    signal navigate(string page)
    readonly property real cut: Math.min(width * 0.56, Theme.px(720))

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: home.cut + Theme.px(120); y2: 0
                GradientStop { position: 0; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.97) }
                GradientStop { position: 0.8; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.9) }
                GradientStop { position: 1; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.75) }
            }
            startX: 0; startY: 0
            PathLine { x: home.cut + Theme.px(120); y: 0 }
            PathLine { x: home.cut; y: home.height }
            PathLine { x: 0; y: home.height }
            PathLine { x: 0; y: 0 }
        }
        ShapePath {
            strokeWidth: Theme.px(2)
            strokeColor: Theme.accent
            fillColor: "transparent"
            startX: home.cut + Theme.px(120); startY: 0
            PathLine { x: home.cut; y: home.height }
        }
    }
    // the neon edge's glow (a static blur of the line: rendered once per size)
    Item {
        id: edgeGlowSrc
        anchors.fill: parent
        visible: false
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeWidth: Theme.px(6)
                strokeColor: Theme.glow
                fillColor: "transparent"
                startX: home.cut + Theme.px(120); startY: 0
                PathLine { x: home.cut; y: home.height }
            }
        }
    }
    MultiEffect {
        anchors.fill: parent
        source: edgeGlowSrc
        visible: !Theme.reduced
        blurEnabled: true
        blur: 1
        blurMax: 40
        opacity: 0.85
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.px(44)
        anchors.topMargin: Theme.px(70)
        anchors.bottomMargin: Theme.px(36)
        width: home.cut - Theme.px(80)
        spacing: Theme.px(10)

        RowLayout {
            spacing: Theme.px(8)
            Rectangle {
                Layout.preferredHeight: Theme.px(24)
                Layout.preferredWidth: tag.implicitWidth + Theme.px(16)
                radius: Theme.px(4)
                color: "transparent"
                border.width: 1
                border.color: Theme.accent
                Text { id: tag; anchors.centerIn: parent; text: App.titleId; color: Theme.accent; font.family: Theme.display; font.pixelSize: Theme.px(13); font.letterSpacing: 2; font.weight: Font.Bold }
            }
            Text { text: qsTr("Version %1").arg(App.gameVersion); color: Theme.textDim; font.family: Theme.body; font.pixelSize: Theme.px(12) }
        }
        Text {
            Layout.fillWidth: true
            text: App.brand
            color: Theme.text
            font.family: Theme.display
            font.pixelSize: Math.min(Theme.px(92), home.cut / 6.2)
            font.weight: Font.Black
            font.letterSpacing: 2
            lineHeight: 0.88
            wrapMode: Text.Wrap
            layer.enabled: !Theme.reduced
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Theme.glow
                shadowBlur: 0.9
                shadowOpacity: 0.55
                shadowHorizontalOffset: 0
                shadowVerticalOffset: 0
            }
        }
        Text {
            text: "inFAMOUS · PC EDITION"
            color: Theme.accent2
            font.family: Theme.display
            font.pixelSize: Theme.px(18)
            font.weight: Font.Bold
            font.letterSpacing: 4
        }
        Item { Layout.fillHeight: true; Layout.minimumHeight: Theme.px(10) }
        PlayButton { Layout.fillWidth: true; heightPx: Theme.px(62) }
        GameStatus { Layout.fillWidth: true }
        Item { Layout.preferredHeight: Theme.px(8) }
        GridLayout {
            Layout.fillWidth: true
            columns: home.cut > Theme.px(600) ? 3 : 2
            columnSpacing: Theme.px(10)
            rowSpacing: Theme.px(10)
            StatTile {
                Layout.fillWidth: true
                icon: "clock"
                caption: qsTr("Last launch")
                value: App.lastLaunchText.split(" · ")[0]
                sub: App.lastLaunchText.indexOf(" · ") > 0 ? App.lastLaunchText.substring(App.lastLaunchText.indexOf(" · ") + 3) : ""
                tone: Theme.accent
            }
            StatTile {
                Layout.fillWidth: true
                icon: "flask"
                caption: qsTr("Profile")
                value: GameSettings.launchProfile === "experimental" ? qsTr("Experimental") : qsTr("Standard")
                sub: qsTr("as play.cmd")
                tone: GameSettings.launchProfile === "experimental" ? Theme.warn : Theme.accent2
                clickable: true
                onActivated: home.navigate("settings")
            }
            StatTile {
                Layout.fillWidth: true
                icon: "database"
                caption: qsTr("Shader cache")
                value: Diag.cacheKnown ? Diag.cacheSize : "…"
                sub: Diag.cacheKnown ? qsTr("%n generation(s)", "", Diag.cacheGenerations) : qsTr("counting…")
                tone: Theme.info
                clickable: true
                onActivated: home.navigate("diagnostics")
            }
        }
    }
}
