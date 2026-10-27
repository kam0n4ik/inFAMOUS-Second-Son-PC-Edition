import QtQuick
import QtQuick.Layouts
import Launcher

// The game's saves and their backups (SaveManager): only this game's Title ID, nothing deleted, a restore always
// preceded by a new backup of the current state.
Item {
    id: page
    property string pendingRestore: ""
    property string pendingRestoreName: ""
    Component.onCompleted: Saves.refresh()

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.px(14)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.px(10)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.px(2)
                Text { text: qsTr("Saves"); color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.px(30); font.weight: Font.Bold }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("%1 · Title ID %2 · %3").arg(App.gameName).arg(App.titleId).arg(Saves.savesRoot)
                    color: Theme.textFaint
                    font.family: Theme.body
                    font.pixelSize: Theme.px(12)
                    elide: Text.ElideMiddle
                }
            }
            LButton { compact: true; kind: "ghost"; iconName: "refresh"; text: qsTr("Refresh"); enabled: !Saves.busy; onClicked: Saves.refresh() }
            LButton { compact: true; kind: "secondary"; iconName: "folder"; text: qsTr("Saves folder"); onClicked: Saves.reveal(Saves.savesRoot) }
            LButton { compact: true; kind: "primary"; iconName: "archive"; text: qsTr("Back up now"); enabled: !Saves.busy; onClicked: Saves.backupNow() }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: Saves.locked
            implicitHeight: Theme.px(40)
            radius: Theme.radiusM
            color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.12)
            border.width: 1
            border.color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.4)
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.px(10)
                AppIcon { name: "lock"; color: Theme.warn }
                Text { Layout.fillWidth: true; text: qsTr("The game is running: saves can be viewed and backed up, not restored."); color: Theme.text; font.family: Theme.body; font.pixelSize: Theme.px(13); elide: Text.ElideRight }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: page.width > Theme.px(980) ? 2 : 1
            columnSpacing: Theme.px(14)
            rowSpacing: Theme.px(14)

            // ---- the saves
            GlassPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.px(16)
                    spacing: Theme.px(10)
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: qsTr("Game saves"); color: Theme.accent2; font.family: Theme.display; font.pixelSize: Theme.px(15); font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.5 }
                        Item { Layout.fillWidth: true }
                        Spinner { visible: Saves.busy; size: Theme.px(14) }
                        Text { text: Saves.totalSize; color: Theme.textFaint; font.family: Theme.body; font.pixelSize: Theme.px(12) }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: Saves.saves.length === 0 && !Saves.busy
                        text: qsTr("No saves yet. They appear here after the game saves for the first time.")
                        color: Theme.textDim
                        font.family: Theme.body
                        font.pixelSize: Theme.px(13)
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ListView {
                        id: savesList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: Saves.saves.length > 0
                        clip: true
                        spacing: Theme.px(8)
                        model: Saves.saves
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: Rectangle {
                            id: save
                            required property var modelData
                            width: savesList.width
                            height: Theme.px(92)
                            radius: Theme.radiusM
                            color: Qt.rgba(1, 1, 1, 0.04)
                            border.width: 1
                            border.color: Theme.stroke
                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.px(10)
                                spacing: Theme.px(12)
                                Rectangle {
                                    Layout.preferredWidth: Theme.px(72)
                                    Layout.preferredHeight: Theme.px(72)
                                    radius: Theme.radiusS
                                    color: Theme.bg2
                                    clip: true
                                    Image {
                                        anchors.fill: parent
                                        source: save.modelData.icon
                                        visible: source.toString().length > 0
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        sourceSize: Qt.size(144, 144)
                                    }
                                    AppIcon { anchors.centerIn: parent; visible: save.modelData.icon.length === 0; name: "saves"; size: Theme.px(28); color: Theme.textFaint }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.px(2)
                                    Text {
                                        Layout.fillWidth: true
                                        text: save.modelData.title.length > 0 ? save.modelData.title : save.modelData.dir
                                        color: Theme.text
                                        font.family: Theme.body
                                        font.pixelSize: Theme.px(14)
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: save.modelData.subtitle.length > 0
                                        text: save.modelData.subtitle
                                        color: Theme.textDim
                                        font.family: Theme.body
                                        font.pixelSize: Theme.px(12)
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: App.formatDate(save.modelData.modified) + " · " + save.modelData.size + " · " + save.modelData.dir
                                        color: Theme.textFaint
                                        font.family: Theme.body
                                        font.pixelSize: Theme.px(11)
                                        elide: Text.ElideRight
                                    }
                                }
                                LButton { compact: true; kind: "ghost"; iconName: "folder"; onClicked: Saves.reveal(save.modelData.path) }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("The line under a save's name is the game's own description of it.")
                        visible: Saves.saves.length > 0
                        color: Theme.textFaint
                        font.family: Theme.body
                        font.pixelSize: Theme.px(11)
                        wrapMode: Text.Wrap
                    }
                }
            }

            // ---- the backups
            GlassPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: Theme.px(16)
                    spacing: Theme.px(10)
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: qsTr("Backups"); color: Theme.accent2; font.family: Theme.display; font.pixelSize: Theme.px(15); font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.5 }
                        Item { Layout.fillWidth: true }
                        Text { text: qsTr("%n copies", "", Saves.backups.length); color: Theme.textFaint; font.family: Theme.body; font.pixelSize: Theme.px(12) }
                        LButton { compact: true; kind: "ghost"; iconName: "folder"; onClicked: Saves.reveal(Saves.backupsRoot) }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("A copy is made before every start (as the game's own launcher does) and before every restore. Old copies are never deleted automatically.")
                        color: Theme.textFaint
                        font.family: Theme.body
                        font.pixelSize: Theme.px(11)
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: Saves.backups.length === 0 && !Saves.busy
                        text: qsTr("No backups yet.")
                        color: Theme.textDim
                        font.family: Theme.body
                        font.pixelSize: Theme.px(13)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    ListView {
                        id: backupList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: Saves.backups.length > 0
                        clip: true
                        spacing: Theme.px(6)
                        model: Saves.backups
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: Rectangle {
                            id: backup
                            required property var modelData
                            width: backupList.width
                            height: Theme.px(58)
                            radius: Theme.radiusM
                            color: hoverB.hovered ? Theme.surfaceHover : Qt.rgba(1, 1, 1, 0.03)
                            border.width: 1
                            border.color: Theme.stroke
                            HoverHandler { id: hoverB }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.px(12)
                                anchors.rightMargin: Theme.px(6)
                                spacing: Theme.px(10)
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.px(2)
                                    Text {
                                        Layout.fillWidth: true
                                        text: App.formatDate(backup.modelData.created)
                                        color: Theme.text
                                        font.family: Theme.body
                                        font.pixelSize: Theme.px(13)
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: backup.modelData.hasTitle
                                              ? qsTr("%1 · saves: %2 · %3").arg(backup.modelData.name).arg(backup.modelData.saves).arg(backup.modelData.size)
                                              : qsTr("%1 · no saves of this game").arg(backup.modelData.name)
                                        color: Theme.textFaint
                                        font.family: Theme.body
                                        font.pixelSize: Theme.px(11)
                                        elide: Text.ElideRight
                                    }
                                }
                                LButton {
                                    compact: true
                                    kind: "secondary"
                                    iconName: "undo"
                                    text: qsTr("Restore")
                                    enabled: backup.modelData.hasTitle && !Saves.locked && !Saves.busy
                                    onClicked: {
                                        page.pendingRestore = backup.modelData.path
                                        page.pendingRestoreName = App.formatDate(backup.modelData.created)
                                        confirm.open()
                                    }
                                }
                                LButton { compact: true; kind: "ghost"; iconName: "folder"; onClicked: Saves.reveal(backup.modelData.path) }
                            }
                        }
                    }
                }
            }
        }
    }

    ModalDialog {
        id: confirm
        parent: Theme.overlay
        icon: "undo"
        tone: Theme.warn
        title: qsTr("Restore the saves of %1?").arg(page.pendingRestoreName)
        text: qsTr("The current saves of %1 are copied to a new backup first, then replaced by this copy. Only this game's saves (Title ID %2) change; the copy itself stays.").arg(App.gameName).arg(App.titleId)
        acceptText: qsTr("Restore")
        rejectText: qsTr("Cancel")
        onAccepted: Saves.restore(page.pendingRestore)
    }
}
