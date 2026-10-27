import QtQuick
import QtQuick.Layouts
import Launcher

// Concept C, Modern Desktop: a sidebar with the game's card and the navigation, the title bar over the content, the
// pages as cards; the art only in the home banner and faintly behind everything.
Item {
    id: shell
    property string page: "home"
    signal navigate(string page)

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.86)
    }

    Rectangle {
        id: sidebar
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.px(250)
        color: Qt.rgba(Theme.surfaceSolid.r, Theme.surfaceSolid.g, Theme.surfaceSolid.b, 0.92)
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.stroke }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.px(16)
            spacing: Theme.px(6)
            RowLayout {
                spacing: Theme.px(10)
                Image {
                    source: "qrc:/brand/launcher.svg"
                    sourceSize: Qt.size(Theme.px(22), Theme.px(22))
                    Layout.preferredWidth: Theme.px(22)
                    Layout.preferredHeight: Theme.px(22)
                }
                Text { text: "inFAMOUS PC Launcher"; color: Theme.textDim; font.family: Theme.display; font.pixelSize: Theme.px(14); font.letterSpacing: 1 }
            }
            Item { Layout.preferredHeight: Theme.px(10) }
            // the game's card: its own icon (sce_sys\icon0.png)
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.px(84)
                radius: Theme.radiusL
                color: Qt.rgba(1, 1, 1, 0.04)
                border.width: 1
                border.color: Theme.stroke
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.px(10)
                    spacing: Theme.px(10)
                    Rectangle {
                        Layout.preferredWidth: Theme.px(62)
                        Layout.preferredHeight: Theme.px(62)
                        radius: Theme.radiusM
                        color: Theme.bg2
                        clip: true
                        Image {
                            anchors.fill: parent
                            source: App.gameIcon
                            fillMode: Image.PreserveAspectCrop
                            sourceSize: Qt.size(128, 128)
                            asynchronous: true
                        }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.px(2)
                        Text { Layout.fillWidth: true; text: App.shortName; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.px(18); font.weight: Font.Bold; elide: Text.ElideRight }
                        Text { Layout.fillWidth: true; text: "PC Edition"; color: Theme.accent2; font.family: Theme.body; font.pixelSize: Theme.px(12); font.weight: Font.DemiBold }
                        Text { Layout.fillWidth: true; text: App.titleId + " · " + App.gameVersion; color: Theme.textFaint; font.family: Theme.body; font.pixelSize: Theme.px(11); elide: Text.ElideRight }
                    }
                }
            }
            Item { Layout.preferredHeight: Theme.px(10) }
            NavButton { Layout.fillWidth: true; kind: "list"; text: qsTr("Home"); iconName: "home"; selected: shell.page === "home"; onClicked: shell.navigate("home") }
            NavButton { Layout.fillWidth: true; kind: "list"; text: qsTr("Settings"); iconName: "settings"; selected: shell.page === "settings"; onClicked: shell.navigate("settings") }
            NavButton { Layout.fillWidth: true; kind: "list"; text: qsTr("Saves"); iconName: "saves"; selected: shell.page === "saves"; onClicked: shell.navigate("saves") }
            NavButton { Layout.fillWidth: true; kind: "list"; text: qsTr("Diagnostics"); iconName: "diagnostics"; selected: shell.page === "diagnostics"; onClicked: shell.navigate("diagnostics") }
            Item { Layout.fillHeight: true }
            GameStatus { Layout.fillWidth: true; compact: true; visible: shell.page !== "home" && Game.state !== "idle" }
            Text {
                Layout.fillWidth: true
                text: qsTr("Launcher %1 · %2").arg(App.launcherVersion).arg(App.graphicsApi)
                color: Theme.textFaint
                font.family: Theme.body
                font.pixelSize: Theme.px(11)
                elide: Text.ElideRight
            }
        }
    }

    TitleBar {
        id: titleBar
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: parent.top
        showBrand: false
        Text {
            anchors.left: parent.left
            anchors.leftMargin: Theme.px(28)
            anchors.verticalCenter: parent.verticalCenter
            text: shell.page === "home" ? App.gameName + " — PC Edition"
                : shell.page === "settings" ? qsTr("Settings")
                : shell.page === "saves" ? qsTr("Saves") : qsTr("Diagnostics")
            color: Theme.textDim
            font.family: Theme.body
            font.pixelSize: Theme.px(13)
        }
    }

    PageStack {
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.top: titleBar.bottom
        anchors.bottom: parent.bottom
        anchors.margins: Theme.px(24)
        anchors.topMargin: Theme.px(4)
        page: shell.page
        home: Component { HomeDesktop { onNavigate: (p) => shell.navigate(p) } }
    }
}
