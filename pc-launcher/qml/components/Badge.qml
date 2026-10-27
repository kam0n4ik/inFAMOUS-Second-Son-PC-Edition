import QtQuick
import Launcher

// A small label: "Не работает в First Light", "Не проверено", "Изменено", "Эксперимент".
Rectangle {
    id: badge
    property string text: ""
    property color tone: Theme.textDim
    property string icon: ""
    implicitWidth: row.implicitWidth + Theme.px(14)
    implicitHeight: Theme.px(20)
    radius: height / 2
    color: Qt.rgba(tone.r, tone.g, tone.b, 0.14)
    border.color: Qt.rgba(tone.r, tone.g, tone.b, 0.35)
    border.width: 1
    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.px(4)
        AppIcon { visible: badge.icon.length > 0; name: badge.icon; size: Theme.px(12); color: badge.tone; anchors.verticalCenter: parent.verticalCenter }
        Text {
            text: badge.text
            color: badge.tone
            font.family: Theme.body
            font.pixelSize: Theme.px(11)
            font.weight: Font.DemiBold
            anchors.verticalCenter: parent.verticalCenter
        }
    }
}
