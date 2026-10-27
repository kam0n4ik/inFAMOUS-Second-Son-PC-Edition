import QtQuick
import QtQuick.Controls.Basic as T
import Launcher

T.Switch {
    id: control
    hoverEnabled: true
    implicitWidth: Theme.px(46)
    implicitHeight: Theme.px(26)
    opacity: enabled ? 1 : 0.4
    indicator: Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: control.checked ? Theme.accent : Qt.rgba(1, 1, 1, control.hovered ? 0.16 : 0.11)
        border.width: control.checked ? 0 : 1
        border.color: Theme.stroke
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Rectangle {
            width: parent.height - Theme.px(6)
            height: width
            radius: width / 2
            y: Theme.px(3)
            x: control.checked ? parent.width - width - Theme.px(3) : Theme.px(3)
            color: "#ffffff"
            Behavior on x { NumberAnimation { duration: Theme.normal; easing.type: Theme.easing } }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent2
            visible: control.visualFocus
        }
    }
    contentItem: Item {}
}
