import QtQuick
import QtQuick.Layouts
import Launcher

// One setting of SettingsManager's sections: label, badges, help, and its control (switch / list / slider).
// Values are read with GameSettings.value(id) and re-read when GameSettings.revision changes.
Item {
    id: row
    property var model: ({})
    readonly property string sid: model.id || ""
    readonly property int rev: GameSettings.revision
    readonly property var value: { rev; return GameSettings.value(sid) }
    readonly property bool available: model.available !== false
    readonly property string inactive: { rev; return GameSettings.inactiveReason(sid) }
    readonly property bool active: inactive.length === 0
    readonly property bool modified: { rev; return GameSettings.isModified(sid) }
    readonly property bool pending: { rev; return GameSettings.isPending(sid) }
    readonly property string issue: { rev; return GameSettings.fileIssue(sid) }
    readonly property bool editable: GameSettings.editable && available

    implicitHeight: layout.implicitHeight + Theme.px(24)

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusM
        color: hover.hovered ? Theme.surfaceHover : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.fast } }
        Rectangle {
            width: Theme.px(3)
            height: parent.height - Theme.px(20)
            anchors.verticalCenter: parent.verticalCenter
            radius: 2
            color: Theme.accent
            visible: row.pending
        }
    }
    HoverHandler { id: hover }

    RowLayout {
        id: layout
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.px(14)
        anchors.rightMargin: Theme.px(14)
        spacing: Theme.px(20)

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.px(4)
            opacity: row.available && row.active ? 1 : 0.62
            Flow {
                Layout.fillWidth: true
                spacing: Theme.px(8)
                Text {
                    text: row.model.label || ""
                    color: Theme.text
                    font.family: Theme.body
                    font.pixelSize: Theme.fontBody
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    width: Math.min(implicitWidth, parent.width)
                }
                Badge { visible: !row.available; text: qsTr("Unavailable"); tone: Theme.textDim; icon: "lock" }
                Badge { visible: row.model.unverified === true; text: qsTr("Not verified"); tone: Theme.warn; icon: "alert" }
                Badge { visible: row.model.experiment === true && (row.model.envName || "").length > 0; text: row.model.envName || ""; tone: Theme.info }
                Badge { visible: row.modified && row.available; text: qsTr("Changed"); tone: Theme.accent2 }
                Badge { visible: row.pending; text: qsTr("Not saved"); tone: Theme.accent }
            }
            Text {
                Layout.fillWidth: true
                visible: (row.model.help || "").length > 0
                text: row.model.help || ""
                color: Theme.textDim
                font.family: Theme.body
                font.pixelSize: Theme.px(12)
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
            Text {
                Layout.fillWidth: true
                visible: !row.available
                text: row.model.reason || ""
                color: Theme.warn
                font.family: Theme.body
                font.pixelSize: Theme.px(12)
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                visible: row.available && !row.active
                text: row.inactive
                color: Theme.textFaint
                font.family: Theme.body
                font.pixelSize: Theme.px(12)
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                visible: row.model.unverified === true
                text: row.model.unverifiedReason || ""
                color: Theme.warn
                font.family: Theme.body
                font.pixelSize: Theme.px(12)
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                visible: (row.model.warn || "").length > 0
                text: row.model.warn || ""
                color: Theme.danger
                font.family: Theme.body
                font.pixelSize: Theme.px(12)
                wrapMode: Text.Wrap
            }
            Text {
                Layout.fillWidth: true
                visible: row.issue.length > 0
                text: qsTr("In the file: %1 (the default is used)").arg(row.issue)
                color: Theme.warn
                font.family: Theme.body
                font.pixelSize: Theme.px(12)
                wrapMode: Text.Wrap
            }
            Text {
                text: row.model.fileKey || ""
                color: Theme.textFaint
                font.family: Theme.mono
                font.pixelSize: Theme.px(11)
            }
        }

        Loader {
            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
            sourceComponent: row.model.type === "switch" ? switchC : row.model.type === "slider" ? sliderC : choiceC
        }
    }

    Component {
        id: switchC
        LSwitch {
            checked: row.value === true
            enabled: row.editable
            onToggled: GameSettings.setValue(row.sid, checked)
        }
    }
    Component {
        id: choiceC
        LChoice {
            choices: row.model.choices || []
            current: row.value === undefined || row.value === null ? "" : String(row.value)
            enabled: row.editable
            implicitWidth: Math.min(Theme.px(320), Math.max(Theme.px(200), row.width * 0.34))
            onPicked: (v) => GameSettings.setValue(row.sid, v)
        }
    }
    Component {
        id: sliderC
        RowLayout {
            spacing: Theme.px(10)
            LSlider {
                from: row.model.min
                to: row.model.max
                stepSize: row.model.step
                snapMode: LSlider.SnapAlways
                value: Number(row.value)
                enabled: row.editable
                implicitWidth: Math.min(Theme.px(240), Math.max(Theme.px(150), row.width * 0.25))
                onMoved: GameSettings.setValue(row.sid, value)
            }
            Text {
                Layout.preferredWidth: Theme.px(76)
                text: Math.round(Number(row.value)) + (row.model.unit ? " " + row.model.unit : "")
                color: Theme.text
                font.family: Theme.body
                font.pixelSize: Theme.px(13)
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignRight
            }
        }
    }
}
