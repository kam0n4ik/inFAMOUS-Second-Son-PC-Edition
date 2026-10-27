import QtQuick
import QtQuick.Controls.Basic as T
import QtQuick.Layouts
import Launcher

// What the launcher knows about the game, the runtime and the computer (DiagnosticsManager), and the tools: logs,
// the report, the folders, the file and configuration checks, clearing the shader cache.
Item {
    id: page
    property string resultTitle: ""
    property string resultText: ""
    Component.onCompleted: if (!Diag.loading) Diag.refresh()

    function showResult(title, text) {
        resultTitle = title
        resultText = text
        result.open()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.px(14)

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.px(10)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.px(2)
                Text { text: qsTr("Diagnostics"); color: Theme.text; font.family: Theme.display; font.pixelSize: Theme.px(30); font.weight: Font.Bold }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Only what the launcher could establish; everything else is shown as unknown.")
                    color: Theme.textFaint
                    font.family: Theme.body
                    font.pixelSize: Theme.px(12)
                    elide: Text.ElideRight
                }
            }
            Spinner { visible: Diag.loading; size: Theme.px(16) }
            LButton { compact: true; kind: "ghost"; iconName: "refresh"; text: qsTr("Refresh"); enabled: !Diag.loading; onClicked: Diag.refresh() }
            LButton {
                compact: true
                kind: "primary"
                iconName: "copy"
                text: qsTr("Copy report")
                onClicked: { Diag.copyReport(); App.message(qsTr("The report is in the clipboard."), "ok") }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.px(14)

            GlassPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ListView {
                    id: list
                    anchors.fill: parent
                    anchors.margins: Theme.px(12)
                    clip: true
                    model: Diag.items
                    boundsBehavior: Flickable.StopAtBounds
                    section.property: "group"
                    section.delegate: Text {
                        required property string section
                        text: section
                        color: Theme.accent2
                        font.family: Theme.display
                        font.pixelSize: Theme.px(14)
                        font.weight: Font.Bold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 1.5
                        topPadding: Theme.px(12)
                        bottomPadding: Theme.px(6)
                        leftPadding: Theme.px(8)
                    }
                    T.ScrollBar.vertical: T.ScrollBar {
                        contentItem: Rectangle { implicitWidth: Theme.px(5); radius: width / 2; color: Qt.rgba(1, 1, 1, 0.2) }
                    }
                    delegate: Rectangle {
                        id: kv
                        required property var modelData
                        width: list.width - Theme.px(10)
                        height: Math.max(Theme.px(34), valueText.implicitHeight + Theme.px(14))
                        radius: Theme.radiusS
                        color: kvHover.hovered ? Theme.surfaceHover : "transparent"
                        HoverHandler { id: kvHover }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.px(8)
                            anchors.rightMargin: Theme.px(8)
                            spacing: Theme.px(12)
                            Rectangle {
                                Layout.preferredWidth: Theme.px(8)
                                Layout.preferredHeight: Theme.px(8)
                                radius: width / 2
                                color: kv.modelData.state === "ok" ? Theme.ok : kv.modelData.state === "warn" ? Theme.warn : kv.modelData.state === "error" ? Theme.danger : Theme.textFaint
                            }
                            Text {
                                Layout.preferredWidth: Math.min(Theme.px(230), list.width * 0.3)
                                text: kv.modelData.label
                                color: Theme.textDim
                                font.family: Theme.body
                                font.pixelSize: Theme.px(13)
                                elide: Text.ElideRight
                            }
                            Text {
                                id: valueText
                                Layout.fillWidth: true
                                text: kv.modelData.value
                                color: kv.modelData.state === "unknown" ? Theme.textFaint : Theme.text
                                font.family: Theme.body
                                font.pixelSize: Theme.px(13)
                                wrapMode: Text.WrapAnywhere
                            }
                            LButton {
                                visible: kv.modelData.path.length > 0 && kvHover.hovered
                                compact: true
                                kind: "ghost"
                                iconName: "folder"
                                onClicked: Saves.reveal(kv.modelData.path)
                            }
                        }
                    }
                }
            }

            GlassPanel {
                Layout.preferredWidth: Theme.px(290)
                Layout.fillHeight: true
                ScrollPage {
                    anchors.fill: parent
                    anchors.margins: Theme.px(14)
                    spacing: Theme.px(8)
                    Text { text: qsTr("Tools"); color: Theme.accent2; font.family: Theme.display; font.pixelSize: Theme.px(14); font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.5 }
                    component Tool: LButton { width: parent ? parent.width - Theme.px(8) : 100; kind: "secondary"; compact: false }
                    Tool { iconName: "file-text"; text: qsTr("Show the game log"); onClicked: { logView.text = Diag.logTail(400); logView.title = Diag.logPath; logView.open() } }
                    Tool { iconName: "folder"; text: qsTr("Open the logs folder"); onClicked: Diag.revealLog() }
                    Tool { iconName: "terminal"; text: qsTr("Launcher log"); onClicked: { logView.text = Diag.launcherLogTail(300); logView.title = Diag.launcherLogPath; logView.open() } }
                    Tool { iconName: "folder"; text: qsTr("Game folder"); onClicked: Diag.revealGameFolder() }
                    Tool { iconName: "settings"; text: qsTr("Settings folder"); onClicked: Diag.revealConfigFolder() }
                    Tool { iconName: "shield"; text: qsTr("Check the game files"); onClicked: page.showResult(qsTr("Game files"), Diag.checkFiles()) }
                    Tool { iconName: "search"; text: qsTr("Check the configuration"); onClicked: page.showResult(qsTr("Configuration"), Diag.checkConfig()) }
                    Item { width: 1; height: Theme.px(6) }
                    Text { text: qsTr("Shader cache"); color: Theme.accent2; font.family: Theme.display; font.pixelSize: Theme.px(14); font.weight: Font.Bold; font.capitalization: Font.AllUppercase; font.letterSpacing: 1.5 }
                    Text {
                        width: parent.width - Theme.px(8)
                        text: Diag.cacheKnown ? qsTr("%1 in %2 generation(s)").arg(Diag.cacheSize).arg(Diag.cacheGenerations) : qsTr("Counting…")
                        color: Theme.text
                        font.family: Theme.body
                        font.pixelSize: Theme.px(13)
                        wrapMode: Text.Wrap
                    }
                    Tool { iconName: "database"; text: qsTr("Open the cache folder"); onClicked: Diag.revealCache() }
                    Tool { kind: "danger"; iconName: "trash"; text: qsTr("Clear the shader cache"); enabled: Diag.cacheKnown && Diag.cacheBytes > 0; onClicked: clearConfirm.open() }
                }
            }
        }
    }

    ModalDialog {
        id: result
        parent: Theme.overlay
        icon: "info"
        title: page.resultTitle
        cardWidth: Theme.px(760)
        acceptText: qsTr("Close")
        Rectangle {
            width: parent.width
            height: Math.min(resultBody.implicitHeight + Theme.px(20), Theme.px(380))
            radius: Theme.radiusS
            color: Qt.rgba(0, 0, 0, 0.35)
            Flickable {
                anchors.fill: parent
                anchors.margins: Theme.px(10)
                clip: true
                contentHeight: resultBody.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                TextEdit {
                    id: resultBody
                    width: parent.width
                    text: page.resultText
                    readOnly: true
                    selectByMouse: true
                    color: Theme.text
                    font.family: Theme.mono
                    font.pixelSize: Theme.px(12)
                    wrapMode: TextEdit.WrapAnywhere
                }
            }
        }
    }

    ModalDialog {
        id: logView
        parent: Theme.overlay
        icon: "file-text"
        cardWidth: Theme.px(980)
        acceptText: qsTr("Close")
        property alias text: logBody.text
        Column {
            width: parent.width
            spacing: Theme.px(8)
            Rectangle {
                width: parent.width
                height: Theme.px(420)
                radius: Theme.radiusS
                color: Qt.rgba(0, 0, 0, 0.45)
                Flickable {
                    id: logFlick
                    anchors.fill: parent
                    anchors.margins: Theme.px(10)
                    clip: true
                    contentHeight: logBody.implicitHeight
                    contentY: Math.max(0, contentHeight - height)
                    boundsBehavior: Flickable.StopAtBounds
                    T.ScrollBar.vertical: T.ScrollBar {}
                    TextEdit {
                        id: logBody
                        width: logFlick.width
                        readOnly: true
                        selectByMouse: true
                        color: Theme.textDim
                        font.family: Theme.mono
                        font.pixelSize: Theme.px(11)
                        wrapMode: TextEdit.WrapAnywhere
                        text: ""
                    }
                }
            }
            Row {
                spacing: Theme.px(8)
                LButton { compact: true; iconName: "copy"; text: qsTr("Copy"); onClicked: { Diag.copyText(logBody.text); App.message(qsTr("Copied."), "ok") } }
                LButton { compact: true; kind: "ghost"; iconName: "folder"; text: qsTr("Open the folder"); onClicked: Diag.revealLog() }
            }
        }
    }

    ModalDialog {
        id: clearConfirm
        parent: Theme.overlay
        icon: "trash"
        tone: Theme.danger
        title: qsTr("Clear the shader cache?")
        text: qsTr("Only the cache's own generation folders in %1 are removed (%2). The next start prepares the shaders again, which takes a few minutes. While the game runs, the cache is cleared at its next start instead.").arg(Diag.cacheDir).arg(Diag.cacheSize)
        acceptText: qsTr("Clear")
        acceptKind: "danger"
        rejectText: qsTr("Cancel")
        onAccepted: App.message(Diag.clearShaderCache(), "info")
    }
}
