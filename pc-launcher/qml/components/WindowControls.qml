import QtQuick
import QtQuick.Window
import Launcher

// Minimize, maximize / restore, close (the window has no system frame). Closing the launcher never ends the game.
Row {
    id: controls
    property Window window: Window.window
    readonly property bool maximized: window && window.visibility === Window.Maximized
    spacing: 0

    component Ctl: Rectangle {
        id: ctl
        property string icon
        property bool danger: false
        signal clicked()
        width: Theme.px(46)
        height: Theme.px(34)
        color: area.containsMouse ? (danger ? "#d9342b" : Qt.rgba(1, 1, 1, 0.1)) : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        AppIcon { anchors.centerIn: parent; name: ctl.icon; size: Theme.px(16); color: Theme.text }
        MouseArea { id: area; anchors.fill: parent; hoverEnabled: true; onClicked: ctl.clicked() }
        Accessible.role: Accessible.Button
        Accessible.name: ctl.icon
    }

    Ctl { icon: "minimize"; onClicked: controls.window.showMinimized() }
    Ctl {
        icon: controls.maximized ? "restore" : "maximize"
        onClicked: controls.maximized ? controls.window.showNormal() : controls.window.showMaximized()
    }
    Ctl { icon: "close"; danger: true; onClicked: controls.window.close() }
}
