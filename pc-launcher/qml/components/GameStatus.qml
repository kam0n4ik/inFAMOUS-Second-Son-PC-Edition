import QtQuick
import QtQuick.Layouts
import Launcher

// The launch's state under the Play button: progress, "running", or the error with its details and the log.
ColumnLayout {
    id: status
    property bool compact: false
    readonly property string st: Game.state
    spacing: Theme.px(6)
    visible: st !== "idle" || App.status === "incomplete"

    RowLayout {
        spacing: Theme.px(8)
        AppIcon {
            name: status.st === "error" ? "error" : status.st === "running" ? "check" : App.status === "incomplete" && status.st === "idle" ? "alert" : "info"
            size: Theme.px(16)
            color: status.st === "error" ? Theme.danger : status.st === "running" ? Theme.ok : App.status === "incomplete" && status.st === "idle" ? Theme.warn : Theme.info
        }
        Text {
            Layout.fillWidth: true
            text: status.st === "idle" && App.status === "incomplete"
                  ? qsTr("Some of the game's files are missing: %1").arg(App.missingFiles.join(", "))
                  : Game.message
            color: status.st === "error" ? Theme.danger : Theme.text
            font.family: Theme.body
            font.pixelSize: Theme.px(13)
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }
    }
    Text {
        Layout.fillWidth: true
        visible: Game.details.length > 0 && !status.compact
        text: Game.details
        color: Theme.textDim
        font.family: Theme.mono
        font.pixelSize: Theme.px(11)
        wrapMode: Text.WrapAnywhere
        maximumLineCount: 4
        elide: Text.ElideRight
    }
    RowLayout {
        visible: status.st === "error"
        spacing: Theme.px(8)
        LButton { compact: true; kind: "secondary"; iconName: "file-text"; text: qsTr("Open the log"); onClicked: App.openGameLog() }
        LButton { compact: true; kind: "ghost"; text: qsTr("Dismiss"); onClicked: Game.acknowledge() }
    }
}
