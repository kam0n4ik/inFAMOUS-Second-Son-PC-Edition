import QtQuick
import QtQuick.Layouts
import Launcher

// The launcher's own preferences (launcher.ini in %LOCALAPPDATA%\inFAMOUS PC Launcher): interface language,
// concept, effects, what happens to the window while the game runs.
ScrollPage {
    id: prefs
    spacing: Theme.px(4)

    component PrefRow: Item {
        id: pr
        property string title
        property string help
        default property alias control: holder.data
        width: prefs.width - Theme.px(16)
        implicitHeight: Math.max(texts.implicitHeight, holder.childrenRect.height) + Theme.px(24)
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.px(14)
            anchors.rightMargin: Theme.px(14)
            spacing: Theme.px(20)
            ColumnLayout {
                id: texts
                Layout.fillWidth: true
                spacing: Theme.px(4)
                Text { text: pr.title; color: Theme.text; font.family: Theme.body; font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Text { text: pr.help; visible: pr.help.length > 0; color: Theme.textDim; font.family: Theme.body; font.pixelSize: Theme.px(12); Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
            Item {
                id: holder
                Layout.preferredWidth: childrenRect.width
                Layout.preferredHeight: childrenRect.height
            }
        }
    }

    Text {
        text: qsTr("Interface")
        color: Theme.accent2
        font.family: Theme.display
        font.pixelSize: Theme.px(14)
        font.weight: Font.Bold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1.5
        topPadding: Theme.px(10)
        leftPadding: Theme.px(14)
    }
    PrefRow {
        title: qsTr("Launcher language")
        help: qsTr("Switches at once. The game's own language is in Settings → Game.")
        LChoice {
            implicitWidth: Theme.px(240)
            choices: [{ value: "ru", label: "Русский", enabled: true }, { value: "en", label: "English", enabled: true }]
            current: App.language
            onPicked: (v) => App.language = v
        }
    }
    PrefRow {
        title: qsTr("Design")
        help: qsTr("Three layouts of the same launcher: Cinematic (the art in full), Neon Control (a side rail and tiles), Modern Desktop (a sidebar and cards).")
        LChoice {
            implicitWidth: Theme.px(240)
            choices: [{ value: "cinematic", label: qsTr("Cinematic"), enabled: true },
                      { value: "neon", label: qsTr("Neon Control"), enabled: true },
                      { value: "desktop", label: qsTr("Modern Desktop"), enabled: true }]
            current: App.concept
            onPicked: (v) => App.concept = v
        }
    }
    PrefRow {
        title: qsTr("Reduce visual effects")
        help: qsTr("No smoke, particles, glow or glass; transitions without animation. Less work for the GPU.")
        LSwitch { checked: App.reducedEffects; onToggled: App.reducedEffects = checked }
    }
    Text {
        text: qsTr("While the game runs")
        color: Theme.accent2
        font.family: Theme.display
        font.pixelSize: Theme.px(14)
        font.weight: Font.Bold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1.5
        topPadding: Theme.px(14)
        leftPadding: Theme.px(14)
    }
    PrefRow {
        title: qsTr("Minimize the launcher")
        help: qsTr("The launcher stops its animations and stays out of the way; closing it never closes the game.")
        LSwitch { checked: App.minimizeOnLaunch; onToggled: App.minimizeOnLaunch = checked }
    }
    PrefRow {
        title: qsTr("Bring the launcher back after the game")
        help: qsTr("When the game ends, the launcher's window returns (always after a failed start).")
        LSwitch { checked: App.restoreAfterExit; onToggled: App.restoreAfterExit = checked }
    }
}
