import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Sendspin

ScrollView {
    id: page

    required property QtObject settings

    contentWidth: availableWidth

    // Names and messages come from the speakers: always plain text
    component SpeakerRow: RowLayout {
        id: row

        property alias title: nameLabel.text
        property alias caption: statusLabel.text
        property alias action: button.text
        property alias actionEnabled: button.enabled
        signal triggered()

        Layout.fillWidth: true
        spacing: 8

        Label {
            id: nameLabel
            textFormat: Text.PlainText
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            id: statusLabel
            textFormat: Text.PlainText
            opacity: 0.7
        }
        Button {
            id: button
            onClicked: row.triggered()
        }
    }

    ColumnLayout {
        width: page.availableWidth
        spacing: 8

        Label {
            text: qsTr("Paired speakers")
            font.bold: true
        }
        Label {
            visible: page.settings.pairedSpeakerCount === 0
            opacity: 0.7
            text: qsTr("None yet.")
        }
        Repeater {
            model: page.settings.speakers

            SpeakerRow {
                required property string speakerId
                required property string name
                required property string statusText
                required property bool paired

                visible: paired
                title: name
                caption: statusText
                action: qsTr("Forget")
                onTriggered: page.settings.forget(speakerId)
            }
        }

        Label {
            Layout.topMargin: 12
            text: qsTr("On the network")
            font.bold: true
        }
        Label {
            visible: page.settings.unpairedSpeakerCount === 0
            opacity: 0.7
            text: qsTr("No other speakers found.")
        }
        Repeater {
            model: page.settings.speakers

            SpeakerRow {
                required property string speakerId
                required property string name
                required property bool paired
                required property bool canPair

                visible: !paired
                title: name
                action: qsTr("Pair…")
                actionEnabled: canPair
                onTriggered: page.settings.pair(speakerId)
            }
        }

        Label {
            Layout.topMargin: 12
            text: qsTr("Pair with a token")
            font.bold: true
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: tokenField
                Layout.fillWidth: true
                placeholderText: qsTr("Paste a speaker's pairing token (SP:0…)")
                onTextEdited: tokenError.visible = false
                onAccepted: tokenButton.clicked()
            }
            Button {
                id: tokenButton
                text: qsTr("Pair")
                enabled: tokenField.text.trim().length > 0
                onClicked: {
                    if (page.settings.pairByToken(tokenField.text))
                        tokenField.clear()
                    else
                        tokenError.visible = true
                }
            }
        }
        Label {
            id: tokenError
            visible: false
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("No speaker on the network has this token.")
        }
    }

    Connections {
        target: page.settings

        function onPairingChanged() {
            if (page.settings.pairingStep === SettingsModel.Idle)
                dialog.close()
            else
                dialog.open()
        }
    }

    Dialog {
        id: dialog

        readonly property int step: page.settings.pairingStep
        readonly property bool entering: step === SettingsModel.EnterCode
                                         || step === SettingsModel.EnterToken

        function submit() {
            if (step === SettingsModel.EnterCode)
                page.settings.submitPairingCode(entry.text)
            else if (step === SettingsModel.EnterToken)
                page.settings.submitPairingToken(entry.text)
        }

        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 400)
        modal: true
        closePolicy: Popup.CloseOnEscape
        title: qsTr("Pair %1").arg(page.settings.pairingName)
        onStepChanged: entry.clear()
        // However it closes, nothing is left pairing
        onClosed: page.settings.cancelPairing()

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                visible: dialog.step === SettingsModel.Starting
                         || dialog.step === SettingsModel.Checking
                         || dialog.step === SettingsModel.Waiting
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: {
                    switch (dialog.step) {
                    case SettingsModel.Starting:
                        return qsTr("Asking the speaker…")
                    case SettingsModel.Checking:
                        return qsTr("Pairing…")
                    case SettingsModel.EnterToken:
                        return qsTr("Paste the speaker's pairing token. It starts with SP:0.")
                    case SettingsModel.EnterCode:
                        return page.settings.pairingDigits === 8
                            ? qsTr("Enter the 8-digit code from the speaker or its leaflet.")
                            : qsTr("Enter the 6-digit code the speaker shows or says.")
                    default:
                        return ""
                    }
                }
                visible: text.length > 0
            }
            TextField {
                id: entry
                Layout.fillWidth: true
                visible: dialog.entering
                onVisibleChanged: if (visible) forceActiveFocus()
                onAccepted: dialog.submit()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                textFormat: Text.PlainText
                text: page.settings.pairingMessage
                visible: text.length > 0
            }
        }

        footer: DialogButtonBox {
            Button {
                text: dialog.step === SettingsModel.Failed ? qsTr("Close") : qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: qsTr("Pair")
                visible: dialog.entering
                enabled: entry.text.trim().length > 0
                onClicked: dialog.submit()
            }
        }
    }
}
