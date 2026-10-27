import QtQuick
import QtQuick.Window
import Launcher

// Resizing the frameless window from its edges and corners (the system's own resize, startSystemResize).
Item {
    id: frame
    property int grip: 6
    anchors.fill: parent
    z: 1000
    visible: Window.window && Window.window.visibility !== Window.Maximized && Window.window.visibility !== Window.FullScreen

    component Edge: MouseArea {
        property int edges: 0
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: edges === (Qt.LeftEdge | Qt.TopEdge) || edges === (Qt.RightEdge | Qt.BottomEdge) ? Qt.SizeFDiagCursor
                   : edges === (Qt.RightEdge | Qt.TopEdge) || edges === (Qt.LeftEdge | Qt.BottomEdge) ? Qt.SizeBDiagCursor
                   : (edges & (Qt.LeftEdge | Qt.RightEdge)) ? Qt.SizeHorCursor : Qt.SizeVerCursor
        onPressed: frame.Window.window.startSystemResize(edges)
    }
    Edge { edges: Qt.LeftEdge; x: 0; y: frame.grip; width: frame.grip; height: parent.height - 2 * frame.grip }
    Edge { edges: Qt.RightEdge; x: parent.width - frame.grip; y: frame.grip; width: frame.grip; height: parent.height - 2 * frame.grip }
    Edge { edges: Qt.TopEdge; x: frame.grip; y: 0; width: parent.width - 2 * frame.grip; height: frame.grip - 2 }
    Edge { edges: Qt.BottomEdge; x: frame.grip; y: parent.height - frame.grip; width: parent.width - 2 * frame.grip; height: frame.grip }
    Edge { edges: Qt.LeftEdge | Qt.TopEdge; x: 0; y: 0; width: frame.grip; height: frame.grip }
    Edge { edges: Qt.RightEdge | Qt.TopEdge; x: parent.width - frame.grip; y: 0; width: frame.grip; height: frame.grip }
    Edge { edges: Qt.LeftEdge | Qt.BottomEdge; x: 0; y: parent.height - frame.grip; width: frame.grip; height: frame.grip }
    Edge { edges: Qt.RightEdge | Qt.BottomEdge; x: parent.width - frame.grip; y: parent.height - frame.grip; width: frame.grip; height: frame.grip }
}
