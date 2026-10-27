import QtQuick
import QtQuick.Controls.impl
import Launcher

// An SVG icon of assets/icons (drawn white), tinted to `color` by IconImage.
Item {
    id: icon
    property string name: ""
    property int size: Theme.px(18)
    property color color: Theme.text
    implicitWidth: size
    implicitHeight: size
    width: size
    height: size
    IconImage {
        anchors.fill: parent
        sourceSize: Qt.size(icon.size, icon.size)
        source: icon.name.length > 0 ? "qrc:/icons/" + icon.name + ".svg" : ""
        color: icon.color
        fillMode: Image.PreserveAspectFit
    }
}
