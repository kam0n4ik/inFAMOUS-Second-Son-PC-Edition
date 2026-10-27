import QtQuick
import QtQuick.Layouts
import Launcher

// More than one game (or contradicting signs) in the launcher's folder: the evidence of each, and the choice.
Item {
    id: view
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.px(80), Theme.px(940))
        spacing: Theme.px(16)
        Text {
            Layout.fillWidth: true
            text: qsTr("Which game is this?")
            color: Theme.text
            font.family: Theme.display
            font.pixelSize: Theme.px(32)
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("The folder %1 has signs of more than one supported game, or signs that contradict each other. Choose the game to use; nothing is changed in the folder.").arg(App.searchedFolder)
            color: Theme.textDim
            font.family: Theme.body
            font.pixelSize: Theme.px(14)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.px(14)
            Repeater {
                model: App.detectionCandidates
                delegate: GlassPanel {
                    id: card
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    Layout.preferredHeight: Theme.px(320)
                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.px(18)
                        spacing: Theme.px(8)
                        Text { Layout.fillWidth: true; text: card.modelData.name; color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.px(22); font.weight: Font.Bold; wrapMode: Text.Wrap }
                        Text { Layout.fillWidth: true; text: card.modelData.root; color: Theme.textFaint; font.family: Theme.body; font.pixelSize: Theme.px(12); wrapMode: Text.WrapAnywhere }
                        Flickable {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            contentHeight: ev.implicitHeight
                            Column {
                                id: ev
                                width: parent.width
                                spacing: Theme.px(3)
                                Repeater {
                                    model: card.modelData.evidence
                                    delegate: Text {
                                        required property string modelData
                                        width: ev.width
                                        text: modelData
                                        color: modelData.startsWith("+") ? Theme.textDim : Theme.warn
                                        font.family: Theme.body
                                        font.pixelSize: Theme.px(12)
                                        wrapMode: Text.WrapAnywhere
                                    }
                                }
                            }
                        }
                        LButton {
                            Layout.fillWidth: true
                            kind: "primary"
                            text: qsTr("Use %1").arg(card.modelData.name)
                            onClicked: App.chooseGame(card.modelData.id, card.modelData.root)
                        }
                    }
                }
            }
        }
    }
}
