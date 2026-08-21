pragma ComponentBehavior: Bound

import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Window

ApplicationWindow {
    id: mainWindow
    visible: !airPodsTrayApp.hideOnStart
    width: 400
    height: 300
    title: "LibrePods"
    objectName: "mainWindowObject"

    // Frameless flyout: no border, no taskbar button, floats above other windows.
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"

    SystemPalette { id: winPalette; colorGroup: SystemPalette.Active }

    // Rounded card background (the window itself is transparent).
    background: Rectangle {
        radius: 12
        color: winPalette.window
        border.width: 1
        border.color: Qt.rgba(winPalette.windowText.r, winPalette.windowText.g, winPalette.windowText.b, 0.12)
    }

    // Dismiss when focus is lost, once the flyout has actually been shown/focused
    // (so it doesn't vanish before it ever gains focus on launch).
    property bool flyoutActivated: false
    onActiveChanged: {
        if (active) {
            flyoutActivated = true
        } else if (flyoutActivated) {
            flyoutActivated = false
            mainWindow.visible = false
        }
    }

    onClosing: mainWindow.visible = false

    // Anchor the window at the bottom-right of the work area, just above the
    // system tray / taskbar (like Surfshark's flyout).
    function positionNearTray() {
        var m = 12
        mainWindow.x = Screen.virtualX + Screen.desktopAvailableWidth - mainWindow.width - m
        mainWindow.y = Screen.virtualY + Screen.desktopAvailableHeight - mainWindow.height - m
    }

    function reopen(pageToLoad) {
        if (pageToLoad == "settings")
        {
            if (stackView.depth == 1)
            {
                stackView.push(settingsPage)
            }
        }
        else
        {
            if (stackView.depth > 1)
            {
                stackView.pop()
            }
        }

        positionNearTray()
        mainWindow.visible = true
        // Defer raise()/requestActivate() a tick: on Windows, activating a
        // frameless always-on-top Tool window in the same call that makes it
        // visible is unreliable (the native window isn't mapped yet), so the
        // window never becomes "active" and the click-outside-to-dismiss
        // logic above never arms itself.
        Qt.callLater(function() {
            mainWindow.raise()
            mainWindow.requestActivate()
        })
    }

    // Mouse area for handling back/forward navigation
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.BackButton | Qt.ForwardButton
        onClicked: (mouse) => {
            if (mouse.button === Qt.BackButton && stackView.depth > 1) {
                stackView.pop()
            } else if (mouse.button === Qt.ForwardButton) {
                console.log("Forward button pressed")
            }
        }
    }

    StackView {
        id: stackView
        anchors.fill: parent
        initialItem: mainPage
    }

    FontLoader {
        id: iconFont
        source: "qrc:/icons/assets/fonts/SF-Symbols-6.ttf"
    }

    // Bottom-of-screen "AirPods connected" flyout, styled like the Win11 OSD.
    AirPodsConnectPopup {
        id: connectPopup
    }

    // Show the flyout only on a genuine connect (false -> true), not every time
    // the app launches while the AirPods happen to already be connected.
    property bool wasConnected: false
    Component.onCompleted: {
        mainWindow.wasConnected = airPodsTrayApp.airpodsConnected
        positionNearTray()
    }
    Connections {
        target: airPodsTrayApp
        function onAirPodsStatusChanged() {
            if (airPodsTrayApp.airpodsConnected && !mainWindow.wasConnected) {
                connectPopup.popUp()
            }
            mainWindow.wasConnected = airPodsTrayApp.airpodsConnected
        }
        // Also show it when a pod is taken out of the case.
        function onAirPodsTakenOutOfCase() {
            connectPopup.popUp()
        }
    }

    Component {
        id: mainPage
        Item {
            Column {
                anchors.fill: parent
                spacing: 20
                padding: 20

                // Connection status indicator (Apple-like pill shape)
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    anchors.topMargin: 10
                    width: 120
                    height: 24
                    radius: 12
                    color: airPodsTrayApp.airpodsConnected ? "#30D158" : "#FF453A"
                    opacity: 0.8
                    visible: !airPodsTrayApp.airpodsConnected

                    Label {
                        anchors.centerIn: parent
                        text: airPodsTrayApp.airpodsConnected ? "Connected" : "Disconnected"
                        color: "white"
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                }

                // Battery Indicator Row
                Row {
                    anchors.horizontalCenter: parent.horizontalCenter
                    spacing: 8

                    PodColumn {
                        visible: airPodsTrayApp.deviceInfo.battery.leftPodAvailable
                        inEar: airPodsTrayApp.deviceInfo.leftPodInEar
                        iconSource: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.podIcon
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.leftPodLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.leftPodCharging
                        indicator: "L"
                    }

                    PodColumn {
                        visible: airPodsTrayApp.deviceInfo.battery.rightPodAvailable
                        inEar: airPodsTrayApp.deviceInfo.rightPodInEar
                        iconSource: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.podIcon
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.rightPodLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.rightPodCharging
                        indicator: "R"
                    }

                    PodColumn {
                        visible: airPodsTrayApp.deviceInfo.battery.caseAvailable
                        inEar: true
                        iconSource: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.caseIcon
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.caseLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.caseCharging
                    }

                    PodColumn {
                        visible: airPodsTrayApp.deviceInfo.battery.headsetAvailable
                        inEar: true
                        iconSource: "qrc:/icons/assets/" + airPodsTrayApp.deviceInfo.podIcon
                        batteryLevel: airPodsTrayApp.deviceInfo.battery.headsetLevel
                        isCharging: airPodsTrayApp.deviceInfo.battery.headsetCharging
                    }
                }

                SegmentedControl {
                    anchors.horizontalCenter: parent.horizontalCenter
                    model: ["Off", "Noise Cancellation", "Transparency", "Adaptive"]
                    currentIndex: airPodsTrayApp.deviceInfo.noiseControlMode
                    onCurrentIndexChanged: airPodsTrayApp.setNoiseControlModeInt(currentIndex)
                    visible: airPodsTrayApp.airpodsConnected
                }

                Slider {
                    visible: airPodsTrayApp.deviceInfo.adaptiveModeActive
                    from: 0
                    to: 100
                    stepSize: 1
                    value: airPodsTrayApp.deviceInfo.adaptiveNoiseLevel

                    Timer {
                        id: debounceTimer
                        interval: 500
                        onTriggered: if (!parent.pressed) airPodsTrayApp.setAdaptiveNoiseLevel(parent.value)
                    }

                    onPressedChanged: if (!pressed) airPodsTrayApp.setAdaptiveNoiseLevel(value)
                    onValueChanged: if (pressed) debounceTimer.restart()

                    Label {
                        text: "Adaptive Noise Level: " + parent.value
                        anchors.top: parent.bottom
                    }
                }

                Switch {
                    visible: airPodsTrayApp.airpodsConnected
                    text: "Conversational Awareness"
                    checked: airPodsTrayApp.deviceInfo.conversationalAwareness
                    onCheckedChanged: airPodsTrayApp.setConversationalAwareness(checked)
                }

                Switch {
                    visible: airPodsTrayApp.airpodsConnected
                    text: "Hearing Aid"
                    checked: airPodsTrayApp.deviceInfo.hearingAidEnabled
                    onCheckedChanged: airPodsTrayApp.setHearingAidEnabled(checked)
                }
            }

            RoundButton {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 10
                font.family: iconFont.name
                font.pixelSize: 18
                text: "\uf958"
                onClicked: stackView.push(settingsPage)
            }
        }
    }

    Component {
        id: settingsPage
        Page {
            id: settingsPageItem
            title: "Settings"
            background: null // let the window's rounded card show through

            ScrollView {
                anchors.fill: parent

                Column {
                    width: parent.width
                    spacing: 20
                    padding: 20

                    Label {
                        text: "Settings"
                        font.pixelSize: 24
                        // center the label
                        anchors.horizontalCenter: parent.horizontalCenter
                    }

                    Column {
                        spacing: 5 // Small gap between label and ComboBox

                        Label {
                            text: "Pause Behavior When Removing AirPods:"
                        }

                        ComboBox {
                            width: parent.width // Ensures full width
                            model: ["One Removed", "Both Removed", "Never"]
                            currentIndex: airPodsTrayApp.earDetectionBehavior
                            onActivated: airPodsTrayApp.earDetectionBehavior = currentIndex
                        }
                    }

                    // Cross-Device (multi-device play) toggle hidden on Windows: doesn't work here yet.
                    // The backend (setCrossDeviceEnabled) is kept intact — might revisit.
                    /*
                    Switch {
                        text: "Cross-Device Connectivity with Android"
                        checked: airPodsTrayApp.crossDeviceEnabled
                        onCheckedChanged: {
                            airPodsTrayApp.setCrossDeviceEnabled(checked)
                        }
                    }
                    */

                    Switch {
                        text: "Auto-Start on Login"
                        checked: airPodsTrayApp.autoStartManager.autoStartEnabled
                        onCheckedChanged: airPodsTrayApp.autoStartManager.autoStartEnabled = checked
                    }

                    Switch {
                        text: "Enable System Notifications"
                        checked: airPodsTrayApp.notificationsEnabled
                        onCheckedChanged: airPodsTrayApp.notificationsEnabled = checked
                    }

                    Switch {
                        visible: airPodsTrayApp.airpodsConnected
                        text: "One Bud ANC Mode"
                        checked: airPodsTrayApp.deviceInfo.oneBudANCMode
                        onCheckedChanged: airPodsTrayApp.deviceInfo.oneBudANCMode = checked

                        ToolTip {
                            visible: parent.hovered
                            text: "Enable ANC when using one AirPod\n(More noise reduction, but uses more battery)"
                            delay: 500
                        }
                    }

                    Row {
                        spacing: 5
                        Label {
                            text: "Bluetooth Retry Attempts:"
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        SpinBox {
                            from: 1
                            to: 10
                            value: airPodsTrayApp.retryAttempts
                            onValueChanged: airPodsTrayApp.retryAttempts = value
                        }
                    }

                    Row {
                        spacing: 10
                        visible: airPodsTrayApp.airpodsConnected

                        TextField {
                            id: newNameField
                            placeholderText: airPodsTrayApp.deviceInfo.deviceName
                            maximumLength: 32
                        }

                        Button {
                            text: "Rename"
                            onClicked: airPodsTrayApp.renameAirPods(newNameField.text)
                        }
                    }

                    Row {
                        spacing: 10
                        visible: airPodsTrayApp.airpodsConnected

                        TextField {
                            id: newPhoneMacField
                            placeholderText: (PHONE_MAC_ADDRESS !== "" ? PHONE_MAC_ADDRESS : "00:00:00:00:00:00")
                            maximumLength: 32
                        }

                        Button {
                            text: "Change Phone MAC"
                            onClicked: airPodsTrayApp.setPhoneMac(newPhoneMacField.text)
                        }
                    }

                }
            }

            // Floating back button
            RoundButton {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.margins: 10
                font.family: iconFont.name
                font.pixelSize: 18
                text: "\uecb1" // U+ECB1
                onClicked: stackView.pop()
            }
        }
    }
}
