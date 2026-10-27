import QtQuick
import QtQuick.Shapes
import Launcher

// A small turning arc while something is in progress (it turns only while visible).
Item {
    id: spin
    property color color: Theme.text
    property int size: Theme.px(16)
    width: size
    height: size
    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: spin.color
            strokeWidth: Math.max(2, spin.size / 8)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc { centerX: spin.size / 2; centerY: spin.size / 2; radiusX: spin.size / 2 - 2; radiusY: radiusX; startAngle: 0; sweepAngle: 270 }
        }
        RotationAnimator on rotation { from: 0; to: 360; duration: 900; loops: Animation.Infinite; running: spin.visible }
    }
}
