import QtQuick
import Launcher

// Short notices (App.message): saved, copied, failed. Each fades out after a few seconds.
Item {
    id: host
    function show(text, kind) { toasts.append({ text: text, kind: kind || "info" }) }
    ListModel { id: toasts }
    Connections {
        target: App
        function onMessage(text, kind) { host.show(text, kind) }
    }
    Column {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.px(22)
        spacing: Theme.px(8)
        Repeater {
            model: toasts
            delegate: Rectangle {
                id: toast
                required property int index
                required property string text
                required property string kind
                width: Math.min(Theme.px(460), host.width - Theme.px(44))
                height: label.implicitHeight + Theme.px(24)
                radius: Theme.radiusM
                color: Theme.surfaceSolid
                border.width: 1
                border.color: kind === "error" ? Theme.danger : kind === "ok" ? Qt.rgba(Theme.ok.r, Theme.ok.g, Theme.ok.b, 0.6) : Theme.strokeStrong
                opacity: 0
                Component.onCompleted: opacity = 1
                Behavior on opacity { NumberAnimation { duration: Theme.normal } }
                AppIcon {
                    id: icon
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.px(12)
                    anchors.verticalCenter: parent.verticalCenter
                    name: toast.kind === "error" ? "error" : toast.kind === "ok" ? "check" : "info"
                    color: toast.kind === "error" ? Theme.danger : toast.kind === "ok" ? Theme.ok : Theme.info
                }
                Text {
                    id: label
                    anchors.left: icon.right
                    anchors.right: parent.right
                    anchors.margins: Theme.px(12)
                    anchors.verticalCenter: parent.verticalCenter
                    text: toast.text
                    color: Theme.text
                    font.family: Theme.body
                    font.pixelSize: Theme.px(13)
                    wrapMode: Text.WrapAnywhere
                }
                Timer {
                    interval: toast.kind === "error" ? 9000 : 4500
                    running: true
                    onTriggered: toasts.remove(toast.index)
                }
                MouseArea { anchors.fill: parent; onClicked: toasts.remove(toast.index) }
            }
        }
    }
}
