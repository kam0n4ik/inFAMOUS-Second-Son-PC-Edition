import QtQuick
import Launcher

// The four pages of a shell: each created the first time it is shown, then kept; switching fades and lifts
// (Theme.normal, OutCubic). Glass panels re-sample the backdrop once a transition has ended.
Item {
    id: stack
    property string page: "home"
    property Component home: null
    // where the pages other than home sit (home always fills the stack)
    property real topInset: 0
    property real bottomInset: 0
    property real leftInset: 0
    property real rightInset: 0

    component Slot: Loader {
        id: slot
        property string name
        property bool visited: false
        readonly property bool shown: stack.page === name
        anchors.fill: parent
        anchors.topMargin: name === "home" ? 0 : stack.topInset
        anchors.bottomMargin: name === "home" ? 0 : stack.bottomInset
        anchors.leftMargin: name === "home" ? 0 : stack.leftInset
        anchors.rightMargin: name === "home" ? 0 : stack.rightInset
        active: visited || shown
        onShownChanged: if (shown) visited = true
        visible: opacity > 0.01
        opacity: shown ? 1 : 0
        enabled: shown
        transform: Translate { y: (1 - slot.opacity) * Theme.px(16) }
        Behavior on opacity {
            NumberAnimation {
                duration: Theme.normal
                easing.type: Theme.easing
                onRunningChanged: if (!running) Theme.layoutTick++
            }
        }
    }

    Slot { name: "home"; sourceComponent: stack.home }
    Slot { name: "settings"; sourceComponent: Component { SettingsPage {} } }
    Slot { name: "saves"; sourceComponent: Component { SavesPage {} } }
    Slot { name: "diagnostics"; sourceComponent: Component { DiagnosticsPage {} } }
}
