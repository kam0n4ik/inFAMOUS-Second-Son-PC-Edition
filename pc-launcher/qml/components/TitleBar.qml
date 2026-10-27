import QtQuick
import QtQuick.Window
import Launcher

// The top bar: drag to move the window, double-click to maximize, the wordmark on the left, the controls on the right.
Item {
    id: bar
    property bool showBrand: true
    property color fill: "transparent"
    default property alias middle: middleHolder.data
    implicitHeight: Theme.px(46)

    Rectangle { anchors.fill: parent; color: bar.fill }
    DragHandler {
        target: null
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onActiveChanged: if (active) bar.Window.window.startSystemMove()
    }
    TapHandler {
        onDoubleTapped: {
            const w = bar.Window.window
            w.visibility === Window.Maximized ? w.showNormal() : w.showMaximized()
        }
    }
    BrandMark {
        visible: bar.showBrand
        anchors.left: parent.left
        anchors.leftMargin: Theme.px(18)
        anchors.verticalCenter: parent.verticalCenter
    }
    Item {
        id: middleHolder
        anchors.left: parent.left
        anchors.right: controls.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
    }
    WindowControls {
        id: controls
        anchors.right: parent.right
        anchors.top: parent.top
        visible: !App.screenshotMode || true
    }
}
