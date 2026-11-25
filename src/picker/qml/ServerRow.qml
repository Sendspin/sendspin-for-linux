import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Sendspin

// The server this PC is a source for. Not groupable: no check, and its icon is highlighted while
// it has the PC as its input.
ColumnLayout {
    id: row

    required property Picker picker

    Layout.fillWidth: true
    spacing: 0

    RowLayout {
        spacing: 4

        ItemDelegate {
            objectName: "server"
            Layout.fillWidth: true
            enabled: row.picker.serverChoosable || row.picker.serverChosen
            text: row.picker.serverName
            icon.name: "network-server-symbolic"
            icon.source: "qrc:/icons/server.svg"
            icon.color: row.picker.serverHighlighted ? palette.highlight : palette.windowText
            onClicked: row.picker.chooseServer()

            Accessible.name: [row.picker.serverName,
                              row.picker.serverChosen ? qsTr("chosen") : "",
                              row.picker.serverHighlighted ? qsTr("playing") : "",
                              row.picker.serverStatusText,
                              row.picker.serverCaption].filter(part => part !== "").join(", ")
        }

        Label {
            Layout.rightMargin: 8
            visible: text !== ""
            text: row.picker.serverStatusText
            color: palette.placeholderText
        }
    }

    Label {
        objectName: "serverCaption"
        Layout.fillWidth: true
        Layout.leftMargin: 8
        Layout.rightMargin: 8
        Layout.bottomMargin: 4
        visible: text !== ""
        text: row.picker.serverCaption
        wrapMode: Text.WordWrap
        color: palette.placeholderText
    }
}
