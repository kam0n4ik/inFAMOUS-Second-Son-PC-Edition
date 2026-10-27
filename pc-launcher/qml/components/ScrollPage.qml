import QtQuick
import QtQuick.Controls.Basic as T
import Launcher

// A vertically scrolling area with the launcher's thin scroll bar.
Flickable {
    id: flick
    default property alias content: column.data
    property alias spacing: column.spacing
    property int padding: Theme.px(4)
    clip: true
    contentWidth: width
    contentHeight: column.implicitHeight + padding * 2
    boundsBehavior: Flickable.StopAtBounds
    flickableDirection: Flickable.VerticalFlick
    pressDelay: 0
    T.ScrollBar.vertical: T.ScrollBar {
        id: bar
        policy: flick.contentHeight > flick.height ? T.ScrollBar.AsNeeded : T.ScrollBar.AlwaysOff
        contentItem: Rectangle {
            implicitWidth: Theme.px(5)
            radius: width / 2
            color: Qt.rgba(1, 1, 1, bar.pressed ? 0.4 : bar.hovered ? 0.3 : 0.18)
        }
    }
    Column {
        id: column
        x: flick.padding
        y: flick.padding
        width: flick.width - flick.padding * 2 - Theme.px(8)
        spacing: Theme.px(12)
    }
}
