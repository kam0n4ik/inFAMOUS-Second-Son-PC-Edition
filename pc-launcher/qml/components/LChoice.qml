import QtQuick
import QtQuick.Controls.Basic as T
import Launcher

// A drop-down list of {value, label, enabled, reason}: a choice the runtime does not support here stays visible,
// greyed, with its reason.
T.ComboBox {
    id: control
    property var choices: []
    property string current: ""
    signal picked(string value)
    model: choices
    textRole: "label"
    valueRole: "value"
    hoverEnabled: true
    implicitWidth: Theme.px(280)
    implicitHeight: Theme.px(36)
    opacity: enabled ? 1 : 0.45
    currentIndex: {
        for (let i = 0; i < choices.length; ++i) if (choices[i].value === current) return i
        return -1
    }
    displayText: currentIndex >= 0 ? choices[currentIndex].label : current
    onActivated: (index) => { if (choices[index].enabled !== false) picked(choices[index].value) }

    background: Rectangle {
        radius: Theme.radiusM
        color: control.pressed || control.popup.visible ? Qt.rgba(1, 1, 1, 0.1) : control.hovered ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(1, 1, 1, 0.05)
        border.width: 1
        border.color: control.visualFocus || control.popup.visible ? Theme.accent : Theme.stroke
        Behavior on color { ColorAnimation { duration: Theme.fast } }
    }
    contentItem: Text {
        leftPadding: Theme.px(12)
        rightPadding: Theme.px(34)
        text: control.displayText
        color: Theme.text
        font.family: Theme.body
        font.pixelSize: Theme.px(13)
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: AppIcon {
        name: "chevron-down"
        size: Theme.px(16)
        color: Theme.textDim
        x: control.width - width - Theme.px(12)
        y: (control.height - height) / 2
        rotation: control.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: Theme.fast } }
    }
    delegate: T.ItemDelegate {
        id: item
        required property var modelData
        required property int index
        width: ListView.view ? ListView.view.width : implicitWidth
        enabled: modelData.enabled !== false
        hoverEnabled: true
        implicitHeight: label.implicitHeight + (reasonText.visible ? reasonText.implicitHeight + Theme.px(4) : 0) + Theme.px(16)
        highlighted: control.highlightedIndex === index
        background: Rectangle {
            radius: Theme.radiusS
            color: item.highlighted || item.hovered ? Theme.surfaceHover : "transparent"
        }
        contentItem: Column {
            spacing: Theme.px(2)
            leftPadding: Theme.px(6)
            Row {
                spacing: Theme.px(8)
                AppIcon {
                    name: "check"
                    size: Theme.px(14)
                    color: Theme.accent
                    opacity: item.modelData.value === control.current ? 1 : 0
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    id: label
                    text: item.modelData.label
                    color: item.enabled ? Theme.text : Theme.textFaint
                    font.family: Theme.body
                    font.pixelSize: Theme.px(13)
                    width: item.width - Theme.px(40)
                    wrapMode: Text.Wrap
                }
            }
            Text {
                id: reasonText
                visible: !item.enabled && (item.modelData.reason || "").length > 0
                text: item.modelData.reason || ""
                color: Theme.textFaint
                font.family: Theme.body
                font.pixelSize: Theme.px(11)
                leftPadding: Theme.px(22)
                width: item.width - Theme.px(16)
                wrapMode: Text.Wrap
            }
        }
    }
    popup: T.Popup {
        y: control.height + Theme.px(4)
        width: Math.max(control.width, Theme.px(260))
        implicitHeight: Math.min(contentItem.implicitHeight + Theme.px(12), Theme.px(380))
        padding: Theme.px(6)
        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.fast } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.fast } }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            T.ScrollIndicator.vertical: T.ScrollIndicator {}
        }
        background: Rectangle {
            radius: Theme.radiusM
            color: Theme.surfaceSolid
            border.width: 1
            border.color: Theme.strokeStrong
        }
    }
}
