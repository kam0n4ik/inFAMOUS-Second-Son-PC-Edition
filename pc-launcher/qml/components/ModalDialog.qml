import QtQuick
import QtQuick.Layouts
import Launcher

// A modal dialog inside the window (an item, not a separate window): dimmed backdrop, a glass card, a title, text,
// optional content and up to two buttons.
Item {
    id: dialog
    property string title: ""
    property string text: ""
    property string icon: "info"
    property color tone: Theme.accent2
    property string acceptText: qsTr("OK")
    property string rejectText: ""
    property string acceptKind: "primary"
    property int cardWidth: Theme.px(560)
    property bool opened: false
    default property alias content: extra.data
    signal accepted()
    signal rejected()

    function open() { opened = true; forceActiveFocus() }
    function close() { opened = false }

    anchors.fill: parent
    visible: opacity > 0
    opacity: opened ? 1 : 0
    z: 100
    Behavior on opacity { NumberAnimation { duration: Theme.normal; easing.type: Theme.easing } }
    Keys.onEscapePressed: { dialog.close(); dialog.rejected() }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.6)
        MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: {} }
    }
    GlassPanel {
        id: card
        width: Math.min(dialog.cardWidth, dialog.width - Theme.px(48))
        height: Math.min(col.implicitHeight + Theme.px(48), dialog.height - Theme.px(48))
        anchors.centerIn: parent
        tintAmount: 0.82
        scale: dialog.opened ? 1 : 0.96
        Behavior on scale { NumberAnimation { duration: Theme.normal; easing.type: Theme.easing } }
        ColumnLayout {
            id: col
            anchors.fill: parent
            anchors.margins: Theme.px(24)
            spacing: Theme.px(14)
            RowLayout {
                spacing: Theme.px(12)
                Rectangle {
                    width: Theme.px(36); height: width; radius: Theme.radiusM
                    color: Qt.rgba(dialog.tone.r, dialog.tone.g, dialog.tone.b, 0.16)
                    AppIcon { anchors.centerIn: parent; name: dialog.icon; size: Theme.px(20); color: dialog.tone }
                }
                Text {
                    Layout.fillWidth: true
                    text: dialog.title
                    color: Theme.text
                    font.family: Theme.display
                    font.pixelSize: Theme.px(22)
                    font.weight: Font.Bold
                    wrapMode: Text.Wrap
                }
            }
            Text {
                Layout.fillWidth: true
                visible: dialog.text.length > 0
                text: dialog.text
                color: Theme.textDim
                font.family: Theme.body
                font.pixelSize: Theme.px(14)
                wrapMode: Text.Wrap
                lineHeight: 1.2
            }
            Item {
                id: extra
                Layout.fillWidth: true
                Layout.fillHeight: children.length > 0
                Layout.preferredHeight: childrenRect.height
                visible: children.length > 0
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: Theme.px(10)
                LButton {
                    visible: dialog.rejectText.length > 0
                    kind: "ghost"
                    text: dialog.rejectText
                    onClicked: { dialog.close(); dialog.rejected() }
                }
                LButton {
                    kind: dialog.acceptKind
                    text: dialog.acceptText
                    onClicked: { dialog.close(); dialog.accepted() }
                }
            }
        }
    }
}
