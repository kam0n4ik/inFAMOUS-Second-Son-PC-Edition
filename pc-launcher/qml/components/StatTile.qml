import QtQuick
import QtQuick.Layouts
import Launcher

// A tile with a caption, a large value and a line under it (the Neon and Desktop homes).
GlassPanel {
    id: tile
    property string icon: "info"
    property string caption: ""
    property string value: ""
    property string sub: ""
    property color tone: Theme.accent
    property bool clickable: false
    signal activated()
    radius: Theme.radiusL
    implicitHeight: col.implicitHeight + Theme.px(32)
    implicitWidth: Theme.px(220)

    Rectangle {
        anchors.fill: parent
        radius: tile.radius
        color: tileArea.containsMouse && tile.clickable ? Theme.surfaceHover : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }
    ColumnLayout {
        id: col
        anchors.fill: parent
        anchors.margins: Theme.px(16)
        spacing: Theme.px(6)
        RowLayout {
            spacing: Theme.px(8)
            Rectangle {
                width: Theme.px(28); height: width; radius: Theme.radiusS
                color: Qt.rgba(tile.tone.r, tile.tone.g, tile.tone.b, 0.16)
                AppIcon { anchors.centerIn: parent; name: tile.icon; size: Theme.px(16); color: tile.tone }
            }
            Text {
                Layout.fillWidth: true
                text: tile.caption
                color: Theme.textDim
                font.family: Theme.display
                font.pixelSize: Theme.px(13)
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 1.2
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: Theme.px(10)
                elide: Text.ElideRight
            }
        }
        Text {
            Layout.fillWidth: true
            text: tile.value
            color: Theme.text
            font.family: Theme.display
            font.pixelSize: Theme.px(24)
            font.weight: Font.Bold
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: Theme.px(15)
            elide: Text.ElideRight
        }
        Text {
            Layout.fillWidth: true
            visible: tile.sub.length > 0
            text: tile.sub
            color: Theme.textFaint
            font.family: Theme.body
            font.pixelSize: Theme.px(12)
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }
    MouseArea {
        id: tileArea
        anchors.fill: parent
        hoverEnabled: tile.clickable
        enabled: tile.clickable
        cursorShape: tile.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: tile.activated()
    }
}
