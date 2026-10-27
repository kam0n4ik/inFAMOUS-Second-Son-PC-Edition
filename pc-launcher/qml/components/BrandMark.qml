import QtQuick
import Launcher

// The wordmark: "inFAMOUS" (type, not the publisher's logo image), the game's name, "PC Edition" and the version.
Row {
    id: mark
    property bool showVersion: true
    property int size: Theme.px(15)
    spacing: Theme.px(10)

    Image {
        source: "qrc:/brand/launcher.svg"
        sourceSize: Qt.size(mark.size * 1.6, mark.size * 1.6)
        width: mark.size * 1.6
        height: width
        anchors.verticalCenter: parent.verticalCenter
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: "in<b>FAMOUS</b>"
        textFormat: Text.StyledText
        color: Theme.text
        font.family: Theme.display
        font.pixelSize: mark.size * 1.2
        font.letterSpacing: 1.5
    }
    Rectangle { width: 1; height: mark.size * 1.2; color: Theme.strokeStrong; anchors.verticalCenter: parent.verticalCenter; visible: App.hasGame }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        visible: App.hasGame
        text: App.shortName
        color: Theme.text
        font.family: Theme.display
        font.pixelSize: mark.size
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 2
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        text: "PC Edition"
        color: Theme.accent2
        font.family: Theme.display
        font.pixelSize: mark.size * 0.85
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 2
    }
    Text {
        anchors.verticalCenter: parent.verticalCenter
        visible: mark.showVersion
        text: "v" + App.launcherVersion
        color: Theme.textFaint
        font.family: Theme.body
        font.pixelSize: mark.size * 0.75
    }
}
