import QtQuick
import QtQuick.Layouts
import Launcher

// Concept A, Cinematic (the default): the art fills the window; the title bar floats over it; the pages open as a
// glass sheet; a compact navigation bar at the bottom centre.
Item {
    id: shell
    property string page: "home"
    signal navigate(string page)

    // pages other than home: the art recedes
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(Theme.bg.r, Theme.bg.g, Theme.bg.b, 0.55)
        opacity: shell.page === "home" ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: Theme.slow; easing.type: Theme.easing } }
    }

    PageStack {
        id: pages
        anchors.fill: parent
        page: shell.page
        home: Component { HomeCinematic { onNavigate: (p) => shell.navigate(p) } }
        // every page but home sits between the title bar and the navigation bar
        topInset: titleBar.height + Theme.px(8)
        bottomInset: navBar.height + Theme.px(40)
        leftInset: Theme.px(40)
        rightInset: Theme.px(40)
    }

    TitleBar {
        id: titleBar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        fill: shell.page === "home" ? "transparent" : Qt.rgba(0, 0, 0, 0.2)
    }

    GlassPanel {
        id: navBar
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.px(22)
        width: navRow.implicitWidth + Theme.px(12)
        height: Theme.px(50)
        radius: height / 2
        tintAmount: 0.7
        Row {
            id: navRow
            anchors.centerIn: parent
            spacing: Theme.px(4)
            NavButton { text: qsTr("Home"); iconName: "home"; selected: shell.page === "home"; onClicked: shell.navigate("home") }
            NavButton { text: qsTr("Settings"); iconName: "settings"; selected: shell.page === "settings"; onClicked: shell.navigate("settings") }
            NavButton { text: qsTr("Saves"); iconName: "saves"; selected: shell.page === "saves"; onClicked: shell.navigate("saves") }
            NavButton { text: qsTr("Diagnostics"); iconName: "diagnostics"; selected: shell.page === "diagnostics"; onClicked: shell.navigate("diagnostics") }
        }
    }
}
