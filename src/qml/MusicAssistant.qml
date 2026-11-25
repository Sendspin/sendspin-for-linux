import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ScrollView {
    id: page

    required property QtObject settings

    contentWidth: availableWidth

    ColumnLayout {
        width: page.availableWidth
        spacing: 8

        // Server names come from the servers: always plain text
        Label {
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            font.bold: true
            text: page.settings.serverStatus
        }

        ColumnLayout {
            visible: page.settings.pairingCode.length > 0
            spacing: 4

            Label {
                text: page.settings.pairingCode
                font.pointSize: 28
                font.family: "monospace"
            }
            Label { text: qsTr("Enter this code in the server to pair this PC.") }
        }

        ColumnLayout {
            visible: page.settings.pairingQr.length > 0
            spacing: 4

            Image {
                source: page.settings.pairingQr
                sourceSize: Qt.size(220, 220)
                cache: false
                smooth: false
            }
            Label { text: qsTr("Scan this code with the server's app to pair this PC.") }
        }

        RowLayout {
            visible: page.settings.pairingApprovalNeeded
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("A server wants to pair. Pairing was paused after too many wrong codes.")
            }
            Button {
                text: qsTr("Allow pairing")
                onClicked: page.settings.allowPairing()
            }
        }

        Label {
            Layout.topMargin: 12
            text: qsTr("Paired servers")
            font.bold: true
        }
        Label {
            visible: page.settings.pairedServers.length === 0
            opacity: 0.7
            text: qsTr("None yet.")
        }
        Repeater {
            model: page.settings.pairedServers

            RowLayout {
                required property var modelData

                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    text: modelData.name
                }
                Label {
                    opacity: 0.7
                    text: qsTr("Paired %1").arg(modelData.pairedAt)
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: page.settings.pairedServers.length > 0
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("A server that has since unpaired this PC may still be listed.")
        }

        RowLayout {
            Layout.topMargin: 12
            spacing: 8

            Button {
                text: qsTr("Copy pairing token")
                onClicked: {
                    copied.text = page.settings.copyPairingToken()
                        ? qsTr("Copied") : qsTr("There is no token yet")
                    copiedTimer.restart()
                }
            }
            Label { id: copied }
            Timer {
                id: copiedTimer
                interval: 3000
                onTriggered: copied.text = ""
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("Paste it into a server to pair without a code. Whoever has it can pair with this PC.")
        }

        Switch {
            Layout.topMargin: 12
            text: qsTr("Allow playback without pairing")
            checked: page.settings.unpairedAccess
            onToggled: page.settings.unpairedAccess = checked
        }
        Switch {
            text: qsTr("Show what plays here in the desktop's media controls")
            checked: page.settings.mediaControls
            onToggled: page.settings.mediaControls = checked
        }
    }
}
