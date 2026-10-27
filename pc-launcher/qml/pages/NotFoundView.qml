import QtQuick
import QtQuick.Layouts
import Launcher

// No supported game in the launcher's folder (or the two above it): what to do, and everything that was checked.
Item {
    id: view
    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.px(80), Theme.px(820))
        spacing: Theme.px(16)
        Image {
            Layout.alignment: Qt.AlignHCenter
            source: "qrc:/brand/launcher.svg"
            sourceSize: Qt.size(Theme.px(84), Theme.px(84))
            Layout.preferredWidth: Theme.px(84)
            Layout.preferredHeight: Theme.px(84)
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("No supported game found")
            color: Theme.text
            font.family: Theme.display
            font.pixelSize: Theme.px(32)
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Could not find the files of a supported game. Put the launcher in the Second Son PC Edition or First Light PC Edition folder.")
            color: Theme.textDim
            font.family: Theme.body
            font.pixelSize: Theme.px(15)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
        }
        GlassPanel {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(details.implicitHeight + Theme.px(32), Theme.px(300))
            tint: Theme.surfaceSolid
            Flickable {
                anchors.fill: parent
                anchors.margins: Theme.px(16)
                clip: true
                contentHeight: details.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                Column {
                    id: details
                    width: parent.width
                    spacing: Theme.px(4)
                    Text {
                        width: parent.width
                        text: qsTr("Searched from: %1").arg(App.searchedFolder)
                        color: Theme.text
                        font.family: Theme.body
                        font.pixelSize: Theme.px(13)
                        font.weight: Font.DemiBold
                        wrapMode: Text.WrapAnywhere
                    }
                    Repeater {
                        model: App.detectionEvidence
                        delegate: Text {
                            required property var modelData
                            width: details.width
                            text: (modelData.positive ? "+ " : "− ") + modelData.text
                            color: modelData.positive ? Theme.textDim : Theme.textFaint
                            font.family: Theme.body
                            font.pixelSize: Theme.px(12)
                            wrapMode: Text.WrapAnywhere
                        }
                    }
                    Text {
                        visible: App.detectionEvidence.length === 0
                        width: details.width
                        text: qsTr("Neither SecondSonPC.toml / SecondSonPC.exe nor FirstLightPC.toml / FirstLightPC.exe, nor a game's sce_sys/param.sfo, is in this folder or the two above it.")
                        color: Theme.textFaint
                        font.family: Theme.body
                        font.pixelSize: Theme.px(12)
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: Theme.px(10)
            LButton { kind: "primary"; iconName: "refresh"; text: qsTr("Search again"); onClicked: App.redetect() }
            LButton { iconName: "copy"; text: qsTr("Copy the details"); onClicked: { Diag.copyText(App.detectionReport); App.message(qsTr("Copied."), "ok") } }
        }
    }
}
