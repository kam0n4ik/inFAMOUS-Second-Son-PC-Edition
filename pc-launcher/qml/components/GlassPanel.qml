import QtQuick
import Launcher

// A translucent panel: its own part of the blurred art behind it, tinted (glass.frag). The blur itself is computed
// once (ArtBackground); this only samples it. With "reduce visual effects" or no art: a plain surface.
Item {
    id: glass
    property real radius: Theme.radiusL
    property color tint: Theme.surfaceSolid
    property real tintAmount: 0.58
    property real light: 1.0
    property bool bordered: true
    readonly property bool sampling: Theme.backdrop !== null && !Theme.reduced && App.background.toString().length > 0
    default property alias content: holder.data

    readonly property rect area: {
        Theme.layoutTick; glass.width; glass.height; glass.x; glass.y
        if (!sampling) return Qt.rect(0, 0, 1, 1)
        const p = glass.mapToItem(Theme.backdrop, 0, 0)
        return Qt.rect(p.x, p.y, Math.max(1, glass.width), Math.max(1, glass.height))
    }

    ShaderEffectSource {
        id: src
        sourceItem: glass.sampling ? Theme.backdrop : null
        sourceRect: glass.area
        live: true
        hideSource: false
        visible: false
        smooth: true
    }
    ShaderEffect {
        anchors.fill: parent
        visible: glass.sampling
        property variant source: src
        property real tintAmount: glass.tintAmount
        property real light: glass.light
        property real radius: Math.min(glass.radius, Math.min(width, height) / 2)
        property size size: Qt.size(width, height)
        property color tint: glass.tint
        fragmentShader: "qrc:/glass.frag.qsb"
    }
    Rectangle {
        anchors.fill: parent
        visible: !glass.sampling
        radius: glass.radius
        color: Qt.rgba(glass.tint.r, glass.tint.g, glass.tint.b, 0.94)
    }
    Rectangle {
        anchors.fill: parent
        radius: glass.radius
        color: "transparent"
        visible: glass.bordered
        border.width: 1
        border.color: Theme.stroke
    }
    Item {
        id: holder
        anchors.fill: parent
    }
}
