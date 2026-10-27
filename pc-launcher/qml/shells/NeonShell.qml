import QtQuick
import QtQuick.Layouts
import Launcher

// Concept B, Neon Control: a vertical navigation rail with a neon edge on the left, the title bar across the rest,
// the pages beside the rail.
Item {
    id: shell
    property string page: "home"
    signal navigate(string page)

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.82)
        opacity: shell.page === "home" ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: Theme.slow; easing.type: Theme.easing } }
    }

    PageStack {
        id: pages
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        page: shell.page
        home: Component { HomeNeon { onNavigate: (p) => shell.navigate(p) } }
        topInset: titleBar.height + Theme.px(6)
        bottomInset: Theme.px(26)
        leftInset: Theme.px(28)
        rightInset: Theme.px(28)
    }

    TitleBar {
        id: titleBar
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        showBrand: shell.page !== "home"
    }

    Rectangle {
        id: rail
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Theme.px(92)
        color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.94)
        Rectangle {
            anchors.right: parent.right
            width: Theme.px(2)
            height: parent.height
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.accent }
                GradientStop { position: 1; color: Theme.accent2 }
            }
            opacity: 0.8
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.topMargin: Theme.px(14)
            anchors.bottomMargin: Theme.px(14)
            spacing: Theme.px(6)
            Image {
                Layout.alignment: Qt.AlignHCenter
                source: "qrc:/brand/launcher.svg"
                sourceSize: Qt.size(Theme.px(40), Theme.px(40))
                Layout.preferredWidth: Theme.px(40)
                Layout.preferredHeight: Theme.px(40)
            }
            Item { Layout.preferredHeight: Theme.px(18) }
            NavButton { Layout.alignment: Qt.AlignHCenter; kind: "rail"; text: qsTr("Home"); iconName: "home"; selected: shell.page === "home"; onClicked: shell.navigate("home") }
            NavButton { Layout.alignment: Qt.AlignHCenter; kind: "rail"; text: qsTr("Settings"); iconName: "settings"; selected: shell.page === "settings"; onClicked: shell.navigate("settings") }
            NavButton { Layout.alignment: Qt.AlignHCenter; kind: "rail"; text: qsTr("Saves"); iconName: "saves"; selected: shell.page === "saves"; onClicked: shell.navigate("saves") }
            NavButton { Layout.alignment: Qt.AlignHCenter; kind: "rail"; text: qsTr("Diagnostics"); iconName: "diagnostics"; selected: shell.page === "diagnostics"; onClicked: shell.navigate("diagnostics") }
            Item { Layout.fillHeight: true }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "v" + App.launcherVersion
                color: Theme.textFaint
                font.family: Theme.body
                font.pixelSize: Theme.px(11)
            }
        }
    }
}
