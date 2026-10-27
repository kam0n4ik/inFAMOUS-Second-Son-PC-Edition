import QtQuick
import Launcher

// An icon, a caption and a value: "Последний запуск …", "Кэш шейдеров — 1,8 ГБ".
Rectangle {
    id: chip
    property string icon: "info"
    property string label: ""
    property string value: ""
    property color tone: Theme.accent2
    property bool glass: true
    implicitWidth: row.implicitWidth + Theme.px(24)
    implicitHeight: Theme.px(40)
    radius: Theme.radiusM
    color: glass ? Qt.rgba(0, 0, 0, 0.32) : Qt.rgba(1, 1, 1, 0.04)
    border.width: 1
    border.color: Theme.stroke
    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.px(9)
        AppIcon { name: chip.icon; size: Theme.px(16); color: chip.tone; anchors.verticalCenter: parent.verticalCenter }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: chip.label
            color: Theme.textDim
            font.family: Theme.body
            font.pixelSize: Theme.px(12)
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: chip.value
            color: Theme.text
            font.family: Theme.body
            font.pixelSize: Theme.px(13)
            font.weight: Font.DemiBold
        }
    }
}
