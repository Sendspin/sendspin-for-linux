import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property QtObject settings
    // 0 This PC, 1 Speakers, 2 Music Assistant
    property alias section: tabs.currentIndex

    title: qsTr("Sendspin Settings")
    width: 560
    height: 540
    minimumWidth: 460
    minimumHeight: 400

    header: TabBar {
        id: tabs

        TabButton { text: qsTr("This PC") }
        TabButton { text: qsTr("Speakers") }
        TabButton { text: qsTr("Music Assistant") }
    }

    StackLayout {
        anchors.fill: parent
        anchors.margins: 16
        currentIndex: tabs.currentIndex

        ThisPc { settings: window.settings }
        Speakers { settings: window.settings }
        MusicAssistant { settings: window.settings }
    }
}
