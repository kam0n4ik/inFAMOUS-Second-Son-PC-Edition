import QtQuick
import QtQuick.Controls.Basic as T
import Launcher

// A navigation item: "pill" (horizontal, the Cinematic bar), "rail" (icon over label, Neon), "list" (Desktop sidebar).
T.AbstractButton {
    id: nav
    property string iconName: ""
    property bool selected: false
    property string kind: "pill"
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    implicitHeight: kind === "rail" ? Theme.px(64) : kind === "list" ? Theme.px(40) : Theme.px(38)
    implicitWidth: kind === "rail" ? Theme.px(84) : kind === "list" ? Theme.px(200) : row.implicitWidth + Theme.px(30)
    Accessible.name: text

    background: Rectangle {
        radius: nav.kind === "pill" ? height / 2 : Theme.radiusM
        color: nav.selected ? (nav.kind === "rail" ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.16) : Qt.rgba(1, 1, 1, 0.12))
             : nav.hovered ? Theme.surfaceHover : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Rectangle {
            visible: nav.kind !== "pill" && nav.selected
            width: Theme.px(3)
            height: parent.height * 0.5
            radius: 2
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            color: Theme.accent
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.accent2
            visible: nav.visualFocus
        }
    }
    contentItem: Item {
        Row {
            id: row
            visible: nav.kind !== "rail"
            anchors.verticalCenter: parent.verticalCenter
            anchors.horizontalCenter: nav.kind === "pill" ? parent.horizontalCenter : undefined
            x: nav.kind === "list" ? Theme.px(14) : 0
            spacing: Theme.px(10)
            AppIcon { name: nav.iconName; size: Theme.px(17); color: nav.selected ? Theme.text : Theme.textDim; anchors.verticalCenter: parent.verticalCenter }
            Text {
                text: nav.text
                color: nav.selected ? Theme.text : Theme.textDim
                font.family: Theme.body
                font.pixelSize: Theme.px(13)
                font.weight: nav.selected ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Column {
            visible: nav.kind === "rail"
            anchors.centerIn: parent
            spacing: Theme.px(5)
            AppIcon { name: nav.iconName; size: Theme.px(20); color: nav.selected ? Theme.accent : Theme.textDim; anchors.horizontalCenter: parent.horizontalCenter }
            Text {
                text: nav.text
                color: nav.selected ? Theme.text : Theme.textDim
                font.family: Theme.body
                font.pixelSize: Theme.px(11)
                anchors.horizontalCenter: parent.horizontalCenter
                width: Math.min(implicitWidth, nav.width - 6)
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
