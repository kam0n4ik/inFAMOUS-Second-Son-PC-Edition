import QtQuick
import QtQuick.Layouts
import Launcher

// The game's PC settings (GameSettings: the runtime's own keys, written to its user file) and the launcher's own.
Item {
    id: page
    property string section: "display"
    readonly property var sections: GameSettings.sections
    readonly property var current: {
        for (let i = 0; i < sections.length; ++i) if (sections[i].id === section) return sections[i]
        return null
    }

    function sectionIcon(id) {
        for (let i = 0; i < sections.length; ++i) if (sections[i].id === id) return sections[i].icon
        return "settings"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.px(14)

        // ---- header
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.px(12)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.px(2)
                Text {
                    text: qsTr("Settings")
                    color: Theme.text
                    font.family: Theme.display
                    font.pixelSize: Theme.px(30)
                    font.weight: Font.Bold
                }
                Text {
                    Layout.fillWidth: true
                    text: page.section === "launcher"
                          ? qsTr("The launcher's own preferences (they do not change the game)")
                          : qsTr("Written to %1 — the file the game reads; unknown keys and comments are kept").arg(GameSettings.userFile.replace(/\//g, "\\"))
                    color: Theme.textFaint
                    font.family: Theme.body
                    font.pixelSize: Theme.px(12)
                    elide: Text.ElideMiddle
                }
            }
            LButton {
                visible: page.section !== "launcher"
                compact: true
                kind: "ghost"
                iconName: "undo"
                text: qsTr("Defaults")
                enabled: GameSettings.editable
                onClicked: GameSettings.resetSection(page.section)
            }
        }

        // ---- notices: the game running, a damaged file
        Rectangle {
            Layout.fillWidth: true
            visible: !GameSettings.editable
            implicitHeight: lockText.implicitHeight + Theme.px(20)
            radius: Theme.radiusM
            color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.12)
            border.width: 1
            border.color: Qt.rgba(Theme.warn.r, Theme.warn.g, Theme.warn.b, 0.4)
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.px(10)
                spacing: Theme.px(10)
                AppIcon { name: "lock"; color: Theme.warn }
                Text { id: lockText; Layout.fillWidth: true; text: GameSettings.lockReason; color: Theme.text; font.family: Theme.body; font.pixelSize: Theme.px(13); wrapMode: Text.Wrap }
            }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: GameSettings.fileProblem.length > 0
            implicitHeight: problemText.implicitHeight + Theme.px(20)
            radius: Theme.radiusM
            color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.12)
            border.width: 1
            border.color: Qt.rgba(Theme.danger.r, Theme.danger.g, Theme.danger.b, 0.45)
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.px(10)
                spacing: Theme.px(10)
                AppIcon { name: "alert"; color: Theme.danger }
                Text { id: problemText; Layout.fillWidth: true; text: GameSettings.fileProblem; color: Theme.text; font.family: Theme.body; font.pixelSize: Theme.px(13); wrapMode: Text.Wrap }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.px(14)

            // ---- the categories
            GlassPanel {
                Layout.preferredWidth: Theme.px(244)
                Layout.fillHeight: true
                Column {
                    anchors.fill: parent
                    anchors.margins: Theme.px(10)
                    spacing: Theme.px(4)
                    Repeater {
                        model: page.sections
                        delegate: NavButton {
                            required property var modelData
                            width: parent.width
                            kind: "list"
                            text: modelData.title
                            iconName: modelData.icon
                            selected: page.section === modelData.id
                            onClicked: page.section = modelData.id
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.px(12)
                                anchors.verticalCenter: parent.verticalCenter
                                visible: modelData.available < modelData.total
                                text: modelData.available + "/" + modelData.total
                                color: Theme.textFaint
                                font.family: Theme.body
                                font.pixelSize: Theme.px(11)
                            }
                        }
                    }
                    Rectangle { width: parent.width; height: 1; color: Theme.stroke }
                    NavButton {
                        width: parent.width
                        kind: "list"
                        text: qsTr("Launcher")
                        iconName: "palette"
                        selected: page.section === "launcher"
                        onClicked: page.section = "launcher"
                    }
                }
            }

            // ---- the settings of the category
            GlassPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ScrollPage {
                    id: list
                    anchors.fill: parent
                    anchors.margins: Theme.px(10)
                    anchors.bottomMargin: GameSettings.dirty ? saveBar.height + Theme.px(14) : Theme.px(10)
                    spacing: Theme.px(4)
                    visible: page.section !== "launcher"

                    Repeater {
                        model: page.current ? page.current.groups : []
                        delegate: Column {
                            id: group
                            required property var modelData
                            width: list.width - Theme.px(16)
                            spacing: Theme.px(2)
                            Text {
                                text: group.modelData.title
                                color: Theme.accent2
                                font.family: Theme.display
                                font.pixelSize: Theme.px(14)
                                font.weight: Font.Bold
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 1.5
                                topPadding: Theme.px(10)
                                bottomPadding: Theme.px(4)
                                leftPadding: Theme.px(14)
                            }
                            Repeater {
                                model: group.modelData.rows
                                delegate: SettingRow {
                                    required property var modelData
                                    width: group.width
                                    model: modelData
                                }
                            }
                        }
                    }
                    Item { width: 1; height: Theme.px(8) }
                }
                LauncherPrefs {
                    anchors.fill: parent
                    anchors.margins: Theme.px(10)
                    visible: page.section === "launcher"
                }

                // ---- unsaved changes
                Rectangle {
                    id: saveBar
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: Theme.px(10)
                    height: Theme.px(54)
                    radius: Theme.radiusM
                    color: Theme.surfaceSolid
                    border.width: 1
                    border.color: Theme.accent
                    visible: opacity > 0
                    opacity: GameSettings.dirty && page.section !== "launcher" ? 1 : 0
                    Behavior on opacity { NumberAnimation { duration: Theme.normal } }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.px(16)
                        anchors.rightMargin: Theme.px(10)
                        spacing: Theme.px(10)
                        AppIcon { name: "info"; color: Theme.accent }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Unsaved changes: %1. The game reads them at its next start.").arg(GameSettings.pendingCount)
                            color: Theme.text
                            font.family: Theme.body
                            font.pixelSize: Theme.px(13)
                            elide: Text.ElideRight
                        }
                        LButton { compact: true; kind: "ghost"; text: qsTr("Revert"); onClicked: GameSettings.revert() }
                        LButton { compact: true; kind: "primary"; iconName: "check"; text: qsTr("Save"); enabled: GameSettings.editable; onClicked: GameSettings.save() }
                    }
                }
            }
        }
    }
}
