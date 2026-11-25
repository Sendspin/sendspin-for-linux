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

            RowLayout {
                spacing: 12

                // Selectable, so the digits can also be taken by hand
                TextEdit {
                    readOnly: true
                    selectByMouse: true
                    activeFocusOnTab: false
                    textFormat: TextEdit.PlainText
                    text: page.settings.pairingCode
                    color: palette.windowText
                    selectionColor: palette.highlight
                    selectedTextColor: palette.highlightedText
                    font.pointSize: 28
                    font.family: "monospace"
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Pairing code: %1").arg(text)
                }
                Button {
                    objectName: "copyCode"
                    text: qsTr("Copy")
                    Accessible.name: qsTr("Copy the pairing code")
                    onClicked: page.settings.copyPairingCode()
                }
                Label {
                    id: codeCopied

                    Accessible.role: Accessible.AlertMessage
                    Accessible.name: text
                }
                Timer {
                    id: codeCopiedTimer
                    interval: 3000
                    onTriggered: codeCopied.text = ""
                }
                Connections {
                    target: page.settings

                    function onPairingCodeCopied() {
                        codeCopied.text = qsTr("Copied")
                        codeCopiedTimer.restart()
                    }
                }
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
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("Pairing QR code")
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
