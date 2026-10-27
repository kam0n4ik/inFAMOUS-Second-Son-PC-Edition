import QtQuick
import Launcher

// Smoke and embers (Second Son) or neon lines and dust (First Light) over the art (ambient.frag). Updated by a 30 Hz
// timer only while `running` (the window active and visible, the game not running, effects not reduced): the window
// is not redrawn at the monitor's 144-165 Hz for it, and not at all while idle in the background.
ShaderEffect {
    id: fx
    property bool running: false
    property real time: 0
    property real mode: Theme.firstLight ? 1 : 0
    property real intensity: 1.0
    property size resolution: Qt.size(width, height)
    property color colorA: Theme.firstLight ? "#8b5cff" : "#3a3330"
    property color colorB: Theme.firstLight ? "#ff3fa4" : "#ff7a22"
    fragmentShader: "qrc:/ambient.frag.qsb"
    blending: true
    Timer {
        interval: 33
        repeat: true
        running: fx.running && fx.visible
        onTriggered: fx.time += 0.033
    }
}
