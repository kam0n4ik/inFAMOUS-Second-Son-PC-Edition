import QtQuick
import QtQuick.Controls.Basic as T
import Launcher

T.Slider {
    id: control
    hoverEnabled: true
    implicitWidth: Theme.px(220)
    implicitHeight: Theme.px(26)
    opacity: enabled ? 1 : 0.4
    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: Theme.px(4)
        radius: 2
        color: Qt.rgba(1, 1, 1, 0.12)
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.accent }
                GradientStop { position: 1; color: Theme.accent2 }
            }
        }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: Theme.px(18)
        height: width
        radius: width / 2
        color: "#ffffff"
        border.width: control.visualFocus ? 3 : 0
        border.color: Theme.accent2
        scale: control.pressed ? 1.12 : control.hovered ? 1.06 : 1
        Behavior on scale { NumberAnimation { duration: Theme.fast } }
    }
}
