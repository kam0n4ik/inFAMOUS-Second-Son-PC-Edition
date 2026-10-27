import QtQuick
import QtQuick.Controls.Basic as T
import QtQuick.Effects
import Launcher

// "Играть": starts the game through ProcessManager and shows its state. "The game is running" only once the game's
// own window exists (Game.state "running"), not when the process was merely created.
T.AbstractButton {
    id: play
    property int heightPx: Theme.px(64)
    property bool glow: !Theme.reduced
    readonly property string st: Game.state
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    enabled: App.hasGame && !Game.busy
    implicitHeight: heightPx
    implicitWidth: Math.max(Theme.px(250), label.implicitWidth + Theme.px(110))
    text: st === "running" ? qsTr("Game is running")
        : st === "starting" ? qsTr("Starting…")
        : st === "preparing" ? qsTr("Preparing…")
        : qsTr("Play")
    onClicked: App.play()
    Accessible.name: text

    background: Item {
        RectangularShadow {
            anchors.fill: face
            visible: play.glow
            radius: face.radius
            blur: Theme.px(28)
            spread: 0
            color: Qt.rgba(Theme.glow.r, Theme.glow.g, Theme.glow.b, play.enabled ? (play.hovered ? 0.75 : 0.5) : 0.15)
            Behavior on color { ColorAnimation { duration: Theme.normal } }
        }
        Rectangle {
            id: face
            anchors.fill: parent
            radius: Theme.radiusM
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: play.enabled ? Theme.accent : Qt.rgba(1, 1, 1, 0.12) }
                GradientStop { position: 1; color: play.enabled ? Theme.accent2 : Qt.rgba(1, 1, 1, 0.08) }
            }
            border.width: play.visualFocus ? 2 : 0
            border.color: "#ffffff"
            scale: play.down ? 0.985 : 1
            Behavior on scale { NumberAnimation { duration: Theme.fast } }
            Rectangle {
                anchors.fill: parent
                radius: parent.radius
                color: "#ffffff"
                opacity: play.hovered && play.enabled ? 0.08 : 0
                Behavior on opacity { NumberAnimation { duration: Theme.fast } }
            }
        }
    }
    contentItem: Item {
        Row {
            anchors.centerIn: parent
            spacing: Theme.px(14)
            Item {
                width: Theme.px(26)
                height: width
                anchors.verticalCenter: parent.verticalCenter
                AppIcon {
                    anchors.centerIn: parent
                    visible: play.st === "idle" || play.st === "error"
                    name: "play"
                    size: Theme.px(24)
                    color: Theme.textOnAccent
                }
                Spinner {
                    anchors.centerIn: parent
                    visible: play.st === "preparing" || play.st === "starting"
                    size: Theme.px(22)
                    color: Theme.text
                }
                AppIcon {
                    anchors.centerIn: parent
                    visible: play.st === "running"
                    name: "check"
                    size: Theme.px(24)
                    color: Theme.ok
                }
            }
            Text {
                id: label
                anchors.verticalCenter: parent.verticalCenter
                text: play.text
                color: play.enabled ? Theme.textOnAccent : Theme.textDim
                font.family: Theme.display
                font.pixelSize: Math.round(play.heightPx * 0.4)
                font.weight: Font.Bold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 2.5
            }
        }
    }
}
