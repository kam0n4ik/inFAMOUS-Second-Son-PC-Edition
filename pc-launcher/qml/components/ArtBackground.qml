import QtQuick
import QtQuick.Effects
import Launcher

// The game's own art (sce_sys\pic0.png of the game folder, never a downloaded picture) filling the window, its blurred
// copy for the glass panels (an invisible item, rendered into their textures only when the art or the size changes)
// and the theme's fallback gradient.
Item {
    id: root
    property url source: App.background
    property alias blurredItem: blurred
    property real artOpacity: 1
    property real artScale: 1.0
    property int alignment: Image.AlignHCenter
    readonly property bool ready: art.status === Image.Ready

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.bg2 }
            GradientStop { position: 1; color: Theme.bg }
        }
    }
    Image {
        id: art
        anchors.fill: parent
        source: root.source
        asynchronous: true
        cache: true
        smooth: true
        mipmap: true
        fillMode: Image.PreserveAspectCrop
        horizontalAlignment: root.alignment
        sourceSize.width: Math.min(3840, Math.max(1920, root.width))
        opacity: root.ready ? root.artOpacity : 0
        scale: root.artScale
        Behavior on opacity { NumberAnimation { duration: Theme.slow } }
        Behavior on scale { NumberAnimation { duration: Theme.slow * 3; easing.type: Easing.OutQuad } }
    }
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Theme.px(120)
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.rgba(0, 0, 0, 0.55) }
            GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0) }
        }
    }
    MultiEffect {
        id: blurred
        anchors.fill: parent
        source: art
        visible: false
        blurEnabled: true
        blur: 1.0
        blurMax: 48
        blurMultiplier: 0.6
        saturation: -0.15
        brightness: -0.1
        autoPaddingEnabled: false
    }
}
