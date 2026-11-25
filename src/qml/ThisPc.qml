import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: page

    required property QtObject settings

    spacing: 8

    Label { text: qsTr("Device name") }
    TextField {
        Layout.fillWidth: true
        placeholderText: page.settings.defaultDeviceName
        // Not bound: an emptied field stays empty, and means the default
        Component.onCompleted: text = page.settings.deviceName
        onTextEdited: page.settings.deviceName = text
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.7
        text: page.settings.deviceNameWaits
              ? qsTr("Servers see the new name the next time Sendspin starts.")
              : qsTr("What servers call this PC. A name given to it in Music Assistant still wins there.")
    }

    Label {
        Layout.topMargin: 12
        text: qsTr("Play on")
    }
    ComboBox {
        Layout.fillWidth: true
        model: page.settings.outputs
        textRole: "description"
        valueRole: "name"
        currentIndex: {
            const outputs = page.settings.outputs
            for (let i = 0; i < outputs.length; i++) {
                if (outputs[i].name === page.settings.output)
                    return i
            }
            return 0
        }
        onActivated: page.settings.output = currentValue
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        opacity: 0.7
        text: qsTr("Where music a server plays to this PC is heard.")
    }

    Switch {
        Layout.topMargin: 12
        text: qsTr("Open at login")
        checked: page.settings.openAtLogin
        onToggled: page.settings.openAtLogin = checked
    }

    Item { Layout.fillHeight: true }
}
