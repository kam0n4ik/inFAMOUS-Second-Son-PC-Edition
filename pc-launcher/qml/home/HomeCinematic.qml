import QtQuick
import QtQuick.Layouts
import Launcher

// Concept A, Cinematic: the game's art in full, the title large at the lower left, Play under it, the facts in chips.
Item {
    id: home
    signal navigate(string page)

    // the art darkened where the text is
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Qt.rgba(0, 0, 0, 0.86) }
            GradientStop { position: 0.42; color: Qt.rgba(0, 0, 0, 0.5) }
            GradientStop { position: 0.75; color: Qt.rgba(0, 0, 0, 0.0) }
        }
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: parent.height * 0.55
        gradient: Gradient {
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 1.0; color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.92) }
        }
    }

    ColumnLayout {
        id: hero
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.px(64)
        anchors.bottomMargin: Theme.px(108)
        width: Math.min(parent.width - Theme.px(128), Theme.px(760))
        spacing: Theme.px(10)

        RowLayout {
            spacing: Theme.px(10)
            Rectangle { Layout.preferredWidth: Theme.px(36); Layout.preferredHeight: 2; color: Theme.accent }
            Text {
                text: "inFAMOUS"
                color: Theme.textDim
                font.family: Theme.display
                font.pixelSize: Theme.px(16)
                font.weight: Font.DemiBold
                font.letterSpacing: 4
            }
        }
        Text {
            Layout.fillWidth: true
            text: App.brand
            color: Theme.text
            font.family: Theme.display
            font.pixelSize: Math.min(Theme.fontHero * 1.15, home.width / 9)
            font.weight: Font.Black
            font.letterSpacing: 3
            lineHeight: 0.9
            wrapMode: Text.Wrap
            style: Text.Normal
        }
        RowLayout {
            spacing: Theme.px(12)
            Text {
                text: "PC EDITION"
                color: Theme.accent2
                font.family: Theme.display
                font.pixelSize: Theme.px(22)
                font.weight: Font.Bold
                font.letterSpacing: 5
            }
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: Theme.px(18); color: Theme.strokeStrong }
            Text {
                text: qsTr("Version %1 · %2").arg(App.gameVersion).arg(App.titleId)
                color: Theme.textDim
                font.family: Theme.body
                font.pixelSize: Theme.px(13)
            }
        }
        Item { Layout.preferredHeight: Theme.px(14) }
        RowLayout {
            spacing: Theme.px(14)
            PlayButton { heightPx: Theme.px(66) }
            LButton {
                Layout.preferredHeight: Theme.px(66)
                Layout.preferredWidth: Theme.px(66)
                iconName: "settings"
                onClicked: home.navigate("settings")
                Accessible.name: qsTr("Settings")
            }
        }
        GameStatus { Layout.fillWidth: true; Layout.topMargin: Theme.px(4) }
        Flow {
            Layout.fillWidth: true
            Layout.topMargin: Theme.px(10)
            spacing: Theme.px(10)
            InfoChip { icon: "clock"; label: qsTr("Last launch"); value: App.lastLaunchText }
            InfoChip {
                icon: "flask"
                label: qsTr("Profile")
                value: GameSettings.launchProfile === "experimental" ? qsTr("Experimental") : qsTr("Standard")
                tone: GameSettings.launchProfile === "experimental" ? Theme.warn : Theme.accent2
            }
            InfoChip {
                icon: "database"
                label: qsTr("Shader cache")
                value: Diag.cacheKnown ? Diag.cacheSize : qsTr("counting…")
            }
        }
    }
}
