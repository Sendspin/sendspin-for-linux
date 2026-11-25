import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Sendspin

// The picker: docs/SPEC.md, "The picker". Rows from top to bottom as "Rows" lists them.
Window {
    id: root

    required property Picker picker
    // The CastEngine: the speakers and the group's volume
    required property QtObject cast

    signal settingsRequested

    width: 340
    height: Math.min(content.implicitHeight + 16, 600)
    minimumWidth: width
    maximumWidth: width
    title: qsTr("Sendspin")
    flags: Qt.Dialog
    color: palette.window

    // Like a popup
    onActiveChanged: if (!active) hide()

    Shortcut {
        sequences: [StandardKey.Cancel]
        onActivated: root.hide()
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.margins: 8
        spacing: 4

        RowLayout {
            spacing: 8

            Rectangle {
                id: dot

                objectName: "dot"
                Layout.leftMargin: 8
                implicitWidth: 12
                implicitHeight: 12
                radius: 6
                color: root.picker.dot === Picker.Plays ? palette.highlight
                     : root.picker.dot === Picker.InUse ? palette.mid : "transparent"
                border.width: root.picker.dot === Picker.NotInUse ? 2 : 0
                border.color: palette.mid

                Accessible.role: Accessible.Indicator
                Accessible.name: root.picker.dotText

                HoverHandler {
                    id: dotHover
                }

                ToolTip.visible: dotHover.hovered
                ToolTip.text: root.picker.dotText
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Sendspin")
                font.bold: true
            }

            ToolButton {
                objectName: "settings"
                icon.name: "preferences-system-symbolic"
                icon.source: "qrc:/icons/settings.svg"
                icon.color: palette.windowText
                text: qsTr("Settings")
                display: AbstractButton.IconOnly
                onClicked: root.settingsRequested()

                ToolTip.visible: hovered
                ToolTip.text: text
            }
        }

        // The picker's one slider: the group's volume
        RowLayout {
            objectName: "volume"
            visible: root.picker.volumeShown
            spacing: 4

            ToolButton {
                objectName: "mute"
                checkable: true
                checked: root.cast.groupMuted
                icon.name: checked ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic"
                icon.source: checked ? "qrc:/icons/speaker-muted.svg" : "qrc:/icons/speaker.svg"
                icon.color: palette.windowText
                text: checked ? qsTr("Unmute the speakers") : qsTr("Mute the speakers")
                display: AbstractButton.IconOnly
                onToggled: root.cast.groupMuted = checked
            }

            Slider {
                objectName: "volumeSlider"
                Layout.fillWidth: true
                from: 0
                to: 100
                stepSize: 1
                value: root.cast.groupVolume
                onMoved: root.cast.groupVolume = Math.round(value)

                Accessible.name: qsTr("Volume of the speakers")
            }
        }

        Label {
            objectName: "notice"
            Layout.fillWidth: true
            Layout.margins: 8
            visible: text !== ""
            text: root.picker.notice
            wrapMode: Text.WordWrap
        }

        // Without PipeWire there is nothing to choose between
        ColumnLayout {
            objectName: "rows"
            visible: root.picker.hasOutputs
            spacing: 0

            OutputRow {
                objectName: "localRow"
                speakerId: root.picker.localId
                name: root.picker.localName
                check: root.picker.localCheck
                clickable: root.picker.localClickable
                statusText: ""
                warning: false
                iconName: "computer-symbolic"
                iconSource: "qrc:/icons/computer.svg"
                onToggled: root.picker.toggle(speakerId)
                onNameClicked: root.picker.playOnly(speakerId)
            }

            Repeater {
                model: root.cast.speakers

                OutputRow {
                    required property int click

                    objectName: "speakerRow"
                    clickable: click !== 0
                    iconName: "audio-speakers-symbolic"
                    iconSource: "qrc:/icons/speaker.svg"
                    onToggled: root.picker.toggle(speakerId)
                    onNameClicked: root.picker.playOnly(speakerId)
                }
            }

            Label {
                objectName: "looking"
                Layout.fillWidth: true
                Layout.margins: 8
                visible: root.picker.looking
                text: qsTr("Looking for speakers… <a href=\"%1\">Which speakers work?</a>")
                    .arg("https://www.sendspin-audio.com/#speakers")
                textFormat: Text.StyledText
                wrapMode: Text.WordWrap
                onLinkActivated: link => Qt.openUrlExternally(link)

                Accessible.role: Accessible.Link
                Accessible.name: qsTr("Looking for speakers. Which speakers work?")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 4
                Layout.bottomMargin: 4
                visible: root.picker.serverShown
                implicitHeight: 1
                color: palette.mid
            }

            ServerRow {
                objectName: "serverRow"
                visible: root.picker.serverShown
                picker: root.picker
            }
        }
    }
}
