import QtQuick
import QtQuick.Controls.Basic as T
import Launcher

// The launcher's button: kind "primary" (accent), "secondary" (glass), "ghost" (text only), "danger".
T.AbstractButton {
    id: control
    property string kind: "secondary"
    property string iconName: ""
    property bool compact: false
    readonly property color base: kind === "primary" ? Theme.accent : kind === "danger" ? Theme.danger : Theme.text
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    implicitHeight: compact ? Theme.px(30) : Theme.px(38)
    implicitWidth: Math.max(implicitHeight, contentRow.implicitWidth + (compact ? Theme.px(20) : Theme.px(30)))
    opacity: enabled ? 1 : 0.45
    Behavior on opacity { NumberAnimation { duration: Theme.fast } }

    background: Rectangle {
        radius: Theme.radiusM
        color: control.kind === "primary" ? (control.down ? Qt.darker(Theme.accent, 1.15) : control.hovered ? Qt.lighter(Theme.accent, 1.12) : Theme.accent)
             : control.kind === "danger" ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, control.hovered ? 0.28 : 0.18)
             : control.kind === "ghost" ? (control.hovered ? Theme.surfaceHover : "transparent")
             : (control.down ? Qt.rgba(1, 1, 1, 0.12) : control.hovered ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(1, 1, 1, 0.05))
        border.width: control.kind === "ghost" || control.kind === "primary" ? 0 : 1
        border.color: control.kind === "danger" ? Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.5) : Theme.stroke
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: 2
            border.color: Theme.accent2
            visible: control.visualFocus
        }
    }
    contentItem: Item {
        implicitWidth: contentRow.implicitWidth
        implicitHeight: contentRow.implicitHeight
        Row {
            id: contentRow
            anchors.centerIn: parent
            spacing: Theme.px(8)
            AppIcon {
                visible: control.iconName.length > 0
                name: control.iconName
                size: control.compact ? Theme.px(15) : Theme.px(17)
                color: control.kind === "primary" ? Theme.textOnAccent : control.kind === "danger" ? Theme.danger : Theme.text
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: control.text.length > 0
                text: control.text
                color: control.kind === "primary" ? Theme.textOnAccent : control.kind === "danger" ? Theme.danger : Theme.text
                font.family: Theme.body
                font.pixelSize: control.compact ? Theme.px(13) : Theme.fontBody
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
}
