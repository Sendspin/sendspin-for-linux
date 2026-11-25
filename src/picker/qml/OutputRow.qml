import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Sendspin

// A groupable output: this PC's own, or a Sendspin speaker. Its name plays there only; its check
// is the group.
RowLayout {
    id: row

    // A speaker's fill themselves in from SpeakerModel's roles
    required property string speakerId
    required property string name
    required property int check
    required property string statusText
    required property bool warning
    property bool clickable: true
    property string iconName
    property url iconSource

    // What a screen reader hears for the row: its name and how it stands
    readonly property string stateText: {
        switch (check) {
        case Picker.Filled:
            return qsTr("playing");
        case Picker.Grey:
            return qsTr("added, can't play");
        case Picker.Spinner:
            return qsTr("added, connecting");
        case Picker.Empty:
            return qsTr("not added");
        default:
            return "";
        }
    }
    readonly property string accessibleName:
        [name, stateText, statusText].filter(part => part !== "").join(", ")

    signal toggled
    signal nameClicked

    Layout.fillWidth: true
    spacing: 4

    ItemDelegate {
        id: label

        Layout.fillWidth: true
        text: row.name
        icon.name: row.iconName
        icon.source: row.iconSource
        icon.color: palette.windowText
        onClicked: row.nameClicked()

        Accessible.name: row.accessibleName
        Accessible.description: qsTr("Play here only")
    }

    Label {
        objectName: "status"
        visible: text !== ""
        text: row.warning ? qsTr("⚠ %1").arg(row.statusText) : row.statusText
        color: palette.placeholderText
    }

    AbstractButton {
        id: box

        objectName: "check"
        visible: row.check !== Picker.NoCheck
        enabled: row.clickable
        implicitWidth: 32
        implicitHeight: 32
        focusPolicy: Qt.StrongFocus
        onClicked: row.toggled()

        Accessible.role: Accessible.CheckBox
        Accessible.checkable: true
        Accessible.checked: row.check !== Picker.Empty
        Accessible.name: (row.check === Picker.Empty ? qsTr("Add %1") : qsTr("Remove %1")).arg(row.name)

        contentItem: Item {
            Rectangle {
                anchors.centerIn: parent
                width: 20
                height: 20
                radius: 10
                visible: row.check !== Picker.Spinner
                color: row.check === Picker.Filled ? palette.highlight
                     : row.check === Picker.Grey ? palette.mid : "transparent"
                border.width: box.visualFocus ? 2 : row.check === Picker.Empty ? 1 : 0
                border.color: box.visualFocus ? palette.windowText : palette.mid

                Label {
                    anchors.centerIn: parent
                    visible: row.check === Picker.Filled || row.check === Picker.Grey
                    text: "✓"
                    color: row.check === Picker.Filled ? palette.highlightedText : palette.window
                    font.bold: true
                }
            }

            BusyIndicator {
                anchors.fill: parent
                anchors.margins: 4
                visible: row.check === Picker.Spinner
                running: visible
            }
        }
    }
}
