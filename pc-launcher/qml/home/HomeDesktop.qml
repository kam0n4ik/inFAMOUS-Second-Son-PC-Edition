import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import Launcher

// Concept C, Modern Desktop: the art as a banner card, Play inside it, the facts and quick actions as cards below.
Item {
    id: home
    signal navigate(string page)

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.px(16)

        // ---- the banner
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumHeight: Theme.px(260)
            Rectangle {
                id: bannerMask
                anchors.fill: parent
                radius: Theme.radiusL
                visible: false
                layer.enabled: true
            }
            Item {
                id: banner
                anchors.fill: parent
                visible: false
                layer.enabled: true
                Rectangle { anchors.fill: parent; color: Theme.bg2 }
                Image {
                    anchors.fill: parent
                    source: App.background
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    sourceSize.width: 1920
                }
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Qt.rgba(0, 0, 0, 0.82) }
                        GradientStop { position: 0.55; color: Qt.rgba(0, 0, 0, 0.25) }
                        GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.0) }
                    }
                }
            }
            MultiEffect {
                anchors.fill: parent
                source: banner
                maskEnabled: true
                maskSource: bannerMask
            }
            Rectangle { anchors.fill: parent; radius: Theme.radiusL; color: "transparent"; border.width: 1; border.color: Theme.stroke }
            ColumnLayout {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: Theme.px(32)
                width: Math.min(parent.width - Theme.px(64), Theme.px(620))
                spacing: Theme.px(8)
                Text {
                    text: App.gameName
                    color: Theme.textDim
                    font.family: Theme.body
                    font.pixelSize: Theme.px(14)
                }
                Text {
                    Layout.fillWidth: true
                    text: App.brand
                    color: Theme.text
                    font.family: Theme.display
                    font.pixelSize: Math.min(Theme.px(68), home.width / 11)
                    font.weight: Font.Black
                    font.letterSpacing: 2
                    wrapMode: Text.Wrap
                    lineHeight: 0.9
                }
                Text {
                    text: qsTr("PC Edition · version %1 · %2").arg(App.gameVersion).arg(App.titleId)
                    color: Theme.accent2
                    font.family: Theme.body
                    font.pixelSize: Theme.px(13)
                    font.weight: Font.DemiBold
                }
                Item { Layout.preferredHeight: Theme.px(8) }
                PlayButton { heightPx: Theme.px(56) }
                GameStatus { Layout.fillWidth: true; compact: true }
            }
        }

        // ---- the facts and the quick actions
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.px(16)
            StatTile {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                icon: "clock"
                caption: qsTr("Last launch")
                value: App.lastLaunchText.split(" · ")[0]
                sub: App.lastLaunchText.indexOf(" · ") > 0 ? App.lastLaunchText.substring(App.lastLaunchText.indexOf(" · ") + 3) : ""
                tone: Theme.accent
            }
            StatTile {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                icon: "flask"
                caption: qsTr("Launch profile")
                value: GameSettings.launchProfile === "experimental" ? qsTr("Experimental") : qsTr("Standard")
                sub: qsTr("Change in Settings → Experimental")
                tone: Theme.accent2
                clickable: true
                onActivated: home.navigate("settings")
            }
            StatTile {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                icon: "database"
                caption: qsTr("Shader cache")
                value: Diag.cacheKnown ? Diag.cacheSize : "…"
                sub: Diag.cacheKnown ? qsTr("%n generation(s)", "", Diag.cacheGenerations) : qsTr("counting…")
                tone: Theme.info
                clickable: true
                onActivated: home.navigate("diagnostics")
            }
            GlassPanel {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.fillHeight: true
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.px(14)
                    spacing: Theme.px(6)
                    Text { text: qsTr("Quick actions"); color: Theme.textDim; font.family: Theme.display; font.pixelSize: Theme.px(13); font.capitalization: Font.AllUppercase; font.letterSpacing: 1.2 }
                    LButton { Layout.fillWidth: true; compact: true; kind: "ghost"; iconName: "saves"; text: qsTr("Saves"); onClicked: home.navigate("saves") }
                    LButton { Layout.fillWidth: true; compact: true; kind: "ghost"; iconName: "file-text"; text: qsTr("Game log"); onClicked: Diag.revealLog() }
                    LButton { Layout.fillWidth: true; compact: true; kind: "ghost"; iconName: "folder"; text: qsTr("Game folder"); onClicked: Diag.revealGameFolder() }
                }
            }
        }
    }
}
