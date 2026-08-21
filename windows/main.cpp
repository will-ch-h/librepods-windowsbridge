#include <QSettings>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSharedMemory>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QFont>
#include <QBluetoothLocalDevice>
#include <QBluetoothSocket>
#include <QQuickWindow>
#include <QLoggingCategory>
#include <QThread>
#include <QTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QDateTime>

#include "airpods_packets.h"
#include "logger.h"
#include "media/mediacontroller.h"
#include "trayiconmanager.h"
#include "enums.h"
#include "battery.hpp"
#include "autostartmanager.hpp"
#include "deviceinfo.hpp"
#include "ble/blemanager.h"
#include "ble/bleutils.h"

#include "windowssleepmonitor.hpp"
#include "winl2capsocket.h"

// The AirPods AAP control channel runs over L2CAP. Qt's QBluetoothSocket does
// not support L2CAP on Windows (the MS stack blocks user-mode L2CAP), so we
// route through a kernel profile driver via WinL2capSocket, which is
// API-compatible with the subset used here.
using AapSocket = WinL2capSocket;

using namespace AirpodsTrayApp::Enums;

Q_LOGGING_CATEGORY(librepods, "librepods")

class AirPodsTrayApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool airpodsConnected READ areAirpodsConnected NOTIFY airPodsStatusChanged)
    Q_PROPERTY(int earDetectionBehavior READ earDetectionBehavior WRITE setEarDetectionBehavior NOTIFY earDetectionBehaviorChanged)
    Q_PROPERTY(bool crossDeviceEnabled READ crossDeviceEnabled WRITE setCrossDeviceEnabled NOTIFY crossDeviceEnabledChanged)
    Q_PROPERTY(AutoStartManager *autoStartManager READ autoStartManager CONSTANT)
    Q_PROPERTY(bool notificationsEnabled READ notificationsEnabled WRITE setNotificationsEnabled NOTIFY notificationsEnabledChanged)
    Q_PROPERTY(int retryAttempts READ retryAttempts WRITE setRetryAttempts NOTIFY retryAttemptsChanged)
    Q_PROPERTY(bool hideOnStart READ hideOnStart CONSTANT)
    Q_PROPERTY(DeviceInfo *deviceInfo READ deviceInfo CONSTANT)
    Q_PROPERTY(QString phoneMacStatus READ phoneMacStatus NOTIFY phoneMacStatusChanged)
    Q_PROPERTY(bool hearingAidEnabled READ hearingAidEnabled WRITE setHearingAidEnabled NOTIFY hearingAidEnabledChanged)

public:
    AirPodsTrayApp(bool debugMode, bool hideOnStart, QQmlApplicationEngine *parent = nullptr)
        : QObject(parent), debugMode(debugMode), m_settings(new QSettings("AirPodsTrayApp", "AirPodsTrayApp"))
        , m_autoStartManager(new AutoStartManager(this)), m_hideOnStart(hideOnStart), parent(parent)
        , m_deviceInfo(new DeviceInfo(this)), m_bleManager(new BleManager(this))
        , m_systemSleepMonitor(new WindowsSleepMonitor(this))
    {
        QLoggingCategory::setFilterRules(QString("librepods.debug=%1").arg(debugMode ? "true" : "false"));
        LOG_INFO("Initializing LibrePods");

        // Initialize tray icon and connect signals
        trayManager = new TrayIconManager(this);
        trayManager->setNotificationsEnabled(loadNotificationsEnabled());
        connect(trayManager, &TrayIconManager::trayClicked, this, &AirPodsTrayApp::onTrayIconActivated);
        connect(trayManager, &TrayIconManager::openApp, this, &AirPodsTrayApp::onOpenApp);
        connect(trayManager, &TrayIconManager::openSettings, this, &AirPodsTrayApp::onOpenSettings);
        connect(trayManager, &TrayIconManager::noiseControlChanged, this, &AirPodsTrayApp::setNoiseControlMode);
        connect(trayManager, &TrayIconManager::conversationalAwarenessToggled, this, &AirPodsTrayApp::setConversationalAwareness);
        connect(m_deviceInfo, &DeviceInfo::batteryStatusChanged, trayManager, &TrayIconManager::updateBatteryStatus);
        connect(m_deviceInfo, &DeviceInfo::noiseControlModeChanged, trayManager, &TrayIconManager::updateNoiseControlState);
        connect(m_deviceInfo, &DeviceInfo::conversationalAwarenessChanged, trayManager, &TrayIconManager::updateConversationalAwareness);
        connect(trayManager, &TrayIconManager::notificationsEnabledChanged, this, &AirPodsTrayApp::saveNotificationsEnabled);
        connect(trayManager, &TrayIconManager::notificationsEnabledChanged, this, &AirPodsTrayApp::notificationsEnabledChanged);

        // Initialize MediaController and connect signals
        mediaController = new MediaController(this);
        connect(mediaController, &MediaController::mediaStateChanged, this, &AirPodsTrayApp::handleMediaStateChange);
        mediaController->followMediaChanges();

        connect(m_bleManager, &BleManager::deviceFound, this, &AirPodsTrayApp::bleDeviceFound);
        connect(m_deviceInfo->getBattery(), &Battery::primaryChanged, this, &AirPodsTrayApp::primaryChanged);
        connect(m_deviceInfo->getEarDetection(), &EarDetection::statusChanged, this, &AirPodsTrayApp::checkCaseRemoval);
        
        if (m_systemSleepMonitor->initialize())
        {
            connect(m_systemSleepMonitor, &WindowsSleepMonitor::systemGoingToSleep, this, &AirPodsTrayApp::onSystemGoingToSleep);
            connect(m_systemSleepMonitor, &WindowsSleepMonitor::systemWakingUp, this, &AirPodsTrayApp::onSystemWakingUp);
        }
        else
        {
            LOG_ERROR("Failed to initialize Windows Sleep Monitor");
        }

        // Load settings
        CrossDevice.isEnabled = loadCrossDeviceEnabled();
        setEarDetectionBehavior(loadEarDetectionSettings());
        setRetryAttempts(loadRetryAttempts());

        LOG_INFO("AirPodsTrayApp initialized");

        // Detect AirPods via the AAP profile driver's device interface
        // (serviceUuids-based detection doesn't work here). Each connected
        // AirPods exposes one instance.
        {
            const QList<QBluetoothAddress> winPods = WinL2capSocket::connectedAirPods();
            for (const QBluetoothAddress &address : winPods) {
                LOG_INFO("Found connected AirPods via AAP driver: " << address.toString());
                QBluetoothDeviceInfo device(address, "AirPods", 0);
                connectToDevice(device);
            }
        }

        // Continuously watch for AirPods connecting/disconnecting at runtime
        // (Windows has no BlueZ-style connect/disconnect signals).
        m_winConnPoller = new QTimer(this);
        m_winConnPoller->setInterval(2000);
        connect(m_winConnPoller, &QTimer::timeout, this, &AirPodsTrayApp::pollWindowsConnection);
        m_winConnPoller->start();

        QBluetoothLocalDevice localDevice;

        const QList<QBluetoothAddress> connectedDevices = localDevice.connectedDevices();
        for (const QBluetoothAddress &address : connectedDevices) {
            QBluetoothDeviceInfo device(address, "", 0);
            if (isAirPodsDevice(device)) {
                connectToDevice(device);

                // On startup after reboot, activate A2DP profile for already connected AirPods
                QTimer::singleShot(2000, this, [this, address]()
                {
                    QString formattedAddress = address.toString().replace(":", "_");
                    mediaController->setConnectedDeviceMacAddress(formattedAddress);
                    mediaController->activateA2dpProfile();
                    LOG_INFO("A2DP profile activation attempted for AirPods found on startup");
                });
                return;
            }
        }

        initializeBluetooth();
    }

    ~AirPodsTrayApp() {
        saveCrossDeviceEnabled();
        saveEarDetectionSettings();

        delete socket;
        delete phoneSocket;
    }

    bool areAirpodsConnected() const { return socket && socket->isOpen() && socket->state() == AapSocket::SocketState::ConnectedState; }
    int earDetectionBehavior() const { return mediaController->getEarDetectionBehavior(); }
    bool crossDeviceEnabled() const { return CrossDevice.isEnabled; }
    AutoStartManager *autoStartManager() const { return m_autoStartManager; }
    bool notificationsEnabled() const { return trayManager->notificationsEnabled(); }
    void setNotificationsEnabled(bool enabled) { trayManager->setNotificationsEnabled(enabled); }
    int retryAttempts() const { return m_retryAttempts; }
    bool hideOnStart() const { return m_hideOnStart; }
    DeviceInfo *deviceInfo() const { return m_deviceInfo; }
    QString phoneMacStatus() const { return m_phoneMacStatus; }
    bool hearingAidEnabled() const { return m_deviceInfo->hearingAidEnabled(); }

private:
    bool debugMode;
    bool isConnectedLocally = false;

    QQmlApplicationEngine *parent = nullptr;

    struct {
        bool isAvailable = true;
        bool isEnabled = true; // Ability to disable the feature
    } CrossDevice;

    bool isAirPodsDevice(const QBluetoothDeviceInfo &device)
    {
        return device.serviceUuids().contains(QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));
    }

    void notifyAndroidDevice()
    {
        if (!CrossDevice.isEnabled) {
            return;
        }

        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Phone::NOTIFICATION);
            LOG_DEBUG("Sent notification packet to Android: " << AirPodsPackets::Phone::NOTIFICATION.toHex());
        }
        else
        {
            LOG_WARN("Phone socket is not open, cannot send notification packet");
        }
    }

public slots:
    void connectToDevice(const QString &address) {
        LOG_INFO("Connecting to device with address: " << address);
        QBluetoothAddress btAddress(address);
        QBluetoothDeviceInfo device(btAddress, "", 0);
        connectToDevice(device);
    }

    void setNoiseControlMode(NoiseControlMode mode)
    {
        if (m_deviceInfo->noiseControlMode() == mode)
        {
            LOG_INFO("Noise control mode is already set to: " << static_cast<int>(mode));
            return;
        }
        LOG_INFO("Setting noise control mode to: " << mode);
        QByteArray packet = AirPodsPackets::NoiseControl::getPacketForMode(mode);
        writePacketToSocket(packet, "Noise control mode packet written: ");
    }
    void setNoiseControlModeInt(int mode)
    {
        if (mode < 0 || mode > static_cast<int>(NoiseControlMode::Adaptive))
        {
            LOG_ERROR("Invalid noise control mode: " << mode);
            return;
        }
        setNoiseControlMode(static_cast<NoiseControlMode>(mode));
    }

    void setConversationalAwareness(bool enabled)
    {
        LOG_INFO("Setting conversational awareness to: " << (enabled ? "enabled" : "disabled"));
        // Build at call time: the namespace ENABLED/DISABLED statics can be empty
        // due to static-initialization order (they copy a template static).
        QByteArray packet = ControlCommand::createCommand(0x28, enabled ? 0x01 : 0x02);

        writePacketToSocket(packet, "Conversational awareness packet written: ");
        m_deviceInfo->setConversationalAwareness(enabled);
    }

    void setOneBudANCMode(bool enabled)
    {
        if (m_deviceInfo->oneBudANCMode() == enabled)
        {
            LOG_INFO("One Bud ANC mode is already " << (enabled ? "enabled" : "disabled"));
            return;
        }

        LOG_INFO("Setting One Bud ANC mode to: " << (enabled ? "enabled" : "disabled"));
        // Build at call time to avoid empty static-init-order packets (see setConversationalAwareness).
        QByteArray packet = ControlCommand::createCommand(0x1B, enabled ? 0x01 : 0x02);

        if (writePacketToSocket(packet, "One Bud ANC mode packet written: "))
        {
            m_deviceInfo->setOneBudANCMode(enabled);
        }
        else
        {
            LOG_ERROR("Failed to send One Bud ANC mode command: socket not open");
        }
    }

    void setRetryAttempts(int attempts)
    {
        if (m_retryAttempts != attempts)
        {
            LOG_DEBUG("Setting retry attempts to: " << attempts);
            m_retryAttempts = attempts;
            emit retryAttemptsChanged(attempts);
            saveRetryAttempts(attempts);
        }
    }

    void initiateMagicPairing()
    {
        if (!socket || !socket->isOpen())
        {
            LOG_ERROR("Socket not open, Magic Pairing cannot be started");
            return;
        }

        writePacketToSocket(AirPodsPackets::MagicPairing::REQUEST_MAGIC_CLOUD_KEYS, "Magic Pairing packet written: ");
    }

    void setAdaptiveNoiseLevel(int level)
    {
        level = qBound(0, level, 100);
        if (m_deviceInfo->adaptiveNoiseLevel() != level && m_deviceInfo->adaptiveModeActive())
        {
            QByteArray packet = AirPodsPackets::AdaptiveNoise::getPacket(level);
            writePacketToSocket(packet, "Adaptive noise level packet written: ");
            m_deviceInfo->setAdaptiveNoiseLevel(level);
        }
    }

    void renameAirPods(const QString &newName)
    {
        if (newName.isEmpty())
        {
            LOG_WARN("Cannot set empty name");
            return;
        }
        if (newName.size() > 32)
        {
            LOG_WARN("Name is too long, must be 32 characters or less");
            return;
        }
        if (newName == m_deviceInfo->deviceName())
        {
            LOG_INFO("Name is already set to: " << newName);
            return;
        }

        QByteArray packet = AirPodsPackets::Rename::getPacket(newName);
        if (writePacketToSocket(packet, "Rename packet written: "))
        {
            LOG_INFO("Sent rename command for new name: " << newName);
            m_deviceInfo->setDeviceName(newName);
        }
        else
        {
            LOG_ERROR("Failed to send rename command: socket not open");
        }
    }

    void setEarDetectionBehavior(int behavior)
    {
        if (behavior == earDetectionBehavior())
        {
            LOG_INFO("Ear detection behavior is already set to: " << behavior);
            return;
        }

        mediaController->setEarDetectionBehavior(static_cast<MediaController::EarDetectionBehavior>(behavior));
        saveEarDetectionSettings();
        emit earDetectionBehaviorChanged(behavior);
    }

    void setCrossDeviceEnabled(bool enabled)
    {
        if (CrossDevice.isEnabled == enabled)
        {
            LOG_INFO("Cross-device feature is already " << (enabled ? "enabled" : "disabled"));
            return;
        }

        CrossDevice.isEnabled = enabled;
        saveCrossDeviceEnabled();
        connectToPhone();
        emit crossDeviceEnabledChanged(enabled);
    }

    void setPhoneMac(const QString &mac)
    {
        if (mac.isEmpty()) {
            LOG_WARN("Empty MAC provided, ignoring");
            m_phoneMacStatus = QStringLiteral("No MAC provided (ignoring)");
            emit phoneMacStatusChanged();
            return;
        }

        // Basic MAC address validation (accepts formats like AA:BB:CC:DD:EE:FF, AABBCCDDEEFF, AA-BB-CC-DD-EE-FF)
        QRegularExpression re("^([0-9A-Fa-f]{2}([-:]?)){5}[0-9A-Fa-f]{2}$");
        if (!re.match(mac).hasMatch()) {
            LOG_ERROR("Invalid MAC address format: " << mac);
            m_phoneMacStatus = QStringLiteral("Invalid MAC: ") + mac;
            emit phoneMacStatusChanged();
            return;
        }

        // Set environment variable for the running process
        qputenv("PHONE_MAC_ADDRESS", mac.toUtf8());
        LOG_INFO("PHONE_MAC_ADDRESS environment variable set to: " << mac);

        m_phoneMacStatus = QStringLiteral("Updated MAC: ") + mac;
        emit phoneMacStatusChanged();

        // Update QML context property so UI placeholders reflect the new value
        if (parent) {
            parent->rootContext()->setContextProperty("PHONE_MAC_ADDRESS", mac);
        }

        // If a phone socket exists, restart connection using the new MAC
        if (phoneSocket && phoneSocket->isOpen()) {
            phoneSocket->close();
            phoneSocket->deleteLater();
            phoneSocket = nullptr;
        }
        connectToPhone();
    }

    void updatePhoneMacStatus(const QString &status)
    {
        m_phoneMacStatus = status;
        emit phoneMacStatusChanged();
    }

    void setHearingAidEnabled(bool enabled)
    {
        LOG_INFO("Setting hearing aid to: " << (enabled ? "enabled" : "disabled"));
        // Build at call time to avoid empty static-init-order packets (see setConversationalAwareness).
        QByteArray packet = enabled ? ControlCommand::createCommand(0x2C, 0x01, 0x01)
                                    : ControlCommand::createCommand(0x2C, 0x02, 0x02);

        writePacketToSocket(packet, "Hearing aid packet written: ");
        m_deviceInfo->setHearingAidEnabled(enabled);
    }

    bool writePacketToSocket(const QByteArray &packet, const QString &logMessage)
    {
        if (socket && socket->isOpen())
        {
            socket->write(packet);
            LOG_DEBUG(logMessage << packet.toHex());
            return true;
        }
        else
        {
            LOG_ERROR("Socket is not open, cannot write packet");
            return false;
        }
    }

    bool loadCrossDeviceEnabled() { return m_settings->value("crossdevice/enabled", false).toBool(); }
    void saveCrossDeviceEnabled() { m_settings->setValue("crossdevice/enabled", CrossDevice.isEnabled); }

    int loadEarDetectionSettings() { return m_settings->value("earDetection/setting", MediaController::EarDetectionBehavior::PauseWhenOneRemoved).toInt(); }
    void saveEarDetectionSettings() { m_settings->setValue("earDetection/setting", mediaController->getEarDetectionBehavior()); }

    bool loadNotificationsEnabled() const { return m_settings->value("notifications/enabled", true).toBool(); }
    void saveNotificationsEnabled(bool enabled) { m_settings->setValue("notifications/enabled", enabled); }

    int loadRetryAttempts() const { return m_settings->value("bluetooth/retryAttempts", 3).toInt(); }
    void saveRetryAttempts(int attempts) { m_settings->setValue("bluetooth/retryAttempts", attempts); }

    void onSystemGoingToSleep()
    {
        if (m_bleManager->isScanning())
        {
            LOG_INFO("Stopping BLE scan before going to sleep");
            m_bleManager->stopScan();
        }
    }
    void onSystemWakingUp()
    {
        LOG_INFO("System is waking up, starting ble scan");
        m_bleManager->startScan();

        // Check if AirPods are already connected and activate A2DP profile
        if (areAirpodsConnected() && m_deviceInfo && !m_deviceInfo->bluetoothAddress().isEmpty())
        {
            LOG_INFO("AirPods already connected after wake-up, re-activating A2DP profile");
            mediaController->setConnectedDeviceMacAddress(m_deviceInfo->bluetoothAddress().replace(":", "_"));

            // Always activate A2DP profile after system wake since the profile might have been lost
            QTimer::singleShot(1000, this, [this]()
            {
                mediaController->activateA2dpProfile();
                LOG_INFO("A2DP profile activation attempted after system wake-up");
            });
        }
    }

private slots:
    void onTrayIconActivated()
    {
        // Route through the QML reopen() so the window is repositioned near the
        // tray and shown reliably (there are now multiple top-level windows).
        reopenOrLoadModule("app");
    }

    void onOpenApp()
    {
        reopenOrLoadModule("app");
    }

    void onOpenSettings()
    {
        reopenOrLoadModule("settings");
    }

    void sendHandshake() {
        LOG_INFO("Connected to device, sending initial packets");
        writePacketToSocket(AirPodsPackets::Connection::HANDSHAKE, "Handshake packet written: ");
    }

    void onDeviceDisconnected(const QBluetoothAddress &address)
    {
        LOG_INFO("Device disconnected: " << address.toString());
        if (socket)
        {
            LOG_WARN("Socket is still open, closing it");
            socket->close();
            socket = nullptr;
        }
        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Connection::AIRPODS_DISCONNECTED);
            LOG_DEBUG("AIRPODS_DISCONNECTED packet written: " << AirPodsPackets::Connection::AIRPODS_DISCONNECTED.toHex());
        }

        // Clear the device name and model
        m_deviceInfo->reset();
        m_bleManager->startScan();
        emit airPodsStatusChanged();

        // Show system notification
        //trayManager->showNotification(
            //tr("AirPods Disconnected"),
            //tr("Your AirPods have been disconnected"));
        trayManager->resetTrayIcon();
    }

    // Windows has no working BlueZ-style connect/disconnect monitor, so poll the
    // AAP driver's device interface to reconcile our connection state. This
    // detects both AirPods connecting after the app launched and AirPods
    // disconnecting while the app is running.
    void pollWindowsConnection()
    {
        // Don't interfere while a connection attempt is already in flight.
        if (socket && socket->state() == AapSocket::ConnectingState)
            return;

        const QList<QBluetoothAddress> pods = WinL2capSocket::connectedAirPods();
        const bool present = !pods.isEmpty();

        if (present && !areAirpodsConnected())
        {
            LOG_INFO("Poller: AirPods present but not connected, connecting to " << pods.first().toString());
            connectToDevice(QBluetoothDeviceInfo(pods.first(), "AirPods", 0));
        }
        else if (!present && socket && socket->isOpen())
        {
            LOG_INFO("Poller: AirPods no longer present, handling disconnect");
            onDeviceDisconnected(socket->peerAddress());
        }
    }

    void parseMetadata(const QByteArray &data)
    {
        // Verify the data starts with the METADATA header
        if (!data.startsWith(AirPodsPackets::Parse::METADATA))
        {
            LOG_ERROR("Invalid metadata packet: Incorrect header");
            return;
        }

        int pos = AirPodsPackets::Parse::METADATA.size(); // Start after the header

        // Check if there is enough data to skip the initial bytes (based on example structure)
        if (data.size() < pos + 6)
        {
            LOG_ERROR("Metadata packet too short to parse initial bytes");
            return;
        }
        pos += 6; // Skip 6 bytes after the header as per example structure

        auto extractString = [&data, &pos]() -> QString
        {
            if (pos >= data.size())
            {
                return QString();
            }
            int start = pos;
            while (pos < data.size() && data.at(pos) != '\0')
            {
                ++pos;
            }
            QString str = QString::fromUtf8(data.mid(start, pos - start));
            if (pos < data.size())
            {
                ++pos; // Move past the null terminator
            }
            return str;
        };

        m_deviceInfo->setDeviceName(extractString());
        m_deviceInfo->setModelNumber(extractString());
        m_deviceInfo->setManufacturer(extractString());

        m_deviceInfo->setModel(parseModelNumber(m_deviceInfo->modelNumber()));
        emit modelChanged();

        // Log extracted metadata
        LOG_INFO("Parsed AirPods metadata:");
        LOG_INFO("Device Name: " << m_deviceInfo->deviceName());
        LOG_INFO("Model Number: " << m_deviceInfo->modelNumber());
        LOG_INFO("Manufacturer: " << m_deviceInfo->manufacturer());
    }

    void connectToDevice(const QBluetoothDeviceInfo &device)
    {
        if (socket && socket->isOpen() && socket->peerAddress() == device.address())
        {
            LOG_INFO("Already connected to the device: " << device.name());
            return;
        }

        if (socket && socket->state() == AapSocket::ConnectingState)
        {
            LOG_INFO("Connection attempt already in progress, ignoring: " << device.name());
            return;
        }

        LOG_INFO("Connecting to device: " << device.name());

        // Clean up any existing socket
        if (socket)
        {
            socket->close();
            socket->deleteLater();
            socket = nullptr;
        }

        AapSocket *localSocket = new AapSocket(QBluetoothServiceInfo::L2capProtocol);
        socket = localSocket;

        // Connection handler
        auto handleConnection = [this, localSocket]()
        {
            connect(localSocket, &AapSocket::readyRead, this, [this, localSocket]()
                    {
            QByteArray data = localSocket->readAll();
            QMetaObject::invokeMethod(this, "parseData", Qt::QueuedConnection, Q_ARG(QByteArray, data));
            QMetaObject::invokeMethod(this, "relayPacketToPhone", Qt::QueuedConnection, Q_ARG(QByteArray, data)); });
            sendHandshake();
        };

        // Error handler with retry
        auto handleError = [this, device, localSocket](AapSocket::SocketError error)
        {
            LOG_ERROR("Socket error: " << error << ", " << localSocket->errorString());

            static int retryCount = 0;
            if (retryCount < m_retryAttempts)
            {
                retryCount++;
                LOG_INFO("Retrying connection (attempt " << retryCount << ")");
                QTimer::singleShot(1500, this, [this, device]()
                                   { connectToDevice(device); });
            }
            else
            {
                LOG_ERROR("Failed to connect after 3 attempts");
                retryCount = 0;
            }
        };

        connect(localSocket, &AapSocket::connected, this, handleConnection);
        connect(localSocket, QOverload<AapSocket::SocketError>::of(&AapSocket::errorOccurred),
                this, handleError);
        // Clean up immediately when the channel drops (device went away), rather
        // than waiting for the slower device-interface poll to notice.
        connect(localSocket, &AapSocket::disconnected, this, [this, localSocket]() {
            if (socket == localSocket) {
                LOG_INFO("AAP channel dropped, handling disconnect");
                onDeviceDisconnected(localSocket->peerAddress());
            }
        });

        localSocket->connectToService(device.address(), QBluetoothUuid("74ec2172-0bad-4d01-8f77-997b2be0722a"));
        m_deviceInfo->setBluetoothAddress(device.address().toString());
        notifyAndroidDevice();
    }

    void parseData(const QByteArray &data)
    {
        LOG_DEBUG("Received: " << data.toHex());

        if (data.startsWith(AirPodsPackets::Parse::HANDSHAKE_ACK))
        {
            writePacketToSocket(AirPodsPackets::Connection::SET_SPECIFIC_FEATURES, "Set specific features packet written: ");
        }
        else if (data.startsWith(AirPodsPackets::Parse::FEATURES_ACK))
        {
            writePacketToSocket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS, "Request notifications packet written: ");

            QTimer::singleShot(2000, this, [this]() {
                if (m_deviceInfo->batteryStatus().isEmpty()) {
                    writePacketToSocket(AirPodsPackets::Connection::REQUEST_NOTIFICATIONS, "Request notifications packet written: ");
                }
            });
        }
        // Magic Cloud Keys Response
        else if (data.startsWith(AirPodsPackets::MagicPairing::MAGIC_CLOUD_KEYS_HEADER))
        {
            auto keys = AirPodsPackets::MagicPairing::parseMagicCloudKeysPacket(data);
            LOG_INFO("Received Magic Cloud Keys:");
            LOG_INFO("MagicAccIRK: " << keys.magicAccIRK.toHex());
            LOG_INFO("MagicAccEncKey: " << keys.magicAccEncKey.toHex());

            // Store the keys
            m_deviceInfo->setMagicAccIRK(keys.magicAccIRK);
            m_deviceInfo->setMagicAccEncKey(keys.magicAccEncKey);
            m_deviceInfo->saveToSettings(*m_settings);
        }
        // Metadata (device name / model / serial) — distinctive header, must be
        // matched before the generic control-command dispatch below.
        else if (data.startsWith(AirPodsPackets::Parse::METADATA))
        {
            parseMetadata(data);
            initiateMagicPairing();
            mediaController->setConnectedDeviceMacAddress(m_deviceInfo->bluetoothAddress().replace(":", "_"));
            if (m_deviceInfo->getEarDetection()->oneOrMorePodsInEar()) // AirPods get added as output device only after this
            {
                mediaController->activateA2dpProfile();
            }
            m_bleManager->stopScan();
            emit airPodsStatusChanged();
        }
        // Battery Status
        else if ((data.size() == 22 || data.size() == 12) && data.startsWith(AirPodsPackets::Parse::BATTERY_STATUS))
        {
            m_deviceInfo->getBattery()->parsePacket(data);
            m_deviceInfo->updateBatteryStatus();
            LOG_INFO("Battery status: " << m_deviceInfo->batteryStatus());
        }
        // Ear Detection
        else if (data.size() == 8 && data.startsWith(AirPodsPackets::Parse::EAR_DETECTION))
        {
            m_deviceInfo->getEarDetection()->parseData(data);
            mediaController->handleEarDetection(m_deviceInfo->getEarDetection());
        }
        // Conversational Awareness streaming data
        else if (data.size() == 10 && data.startsWith(AirPodsPackets::ConversationalAwareness::DATA_HEADER))
        {
            LOG_INFO("Received conversational awareness data");
            mediaController->handleConversationalAwareness(data);
        }
        // Generic control-command notification: 04 00 04 00 09 00 <id> <value> ...
        // We dispatch on the command-id byte using the stable ControlCommand
        // prefix. The per-setting HEADER constants (ConversationalAwareness,
        // OneBudANCMode, ... built from a template static) can end up empty due
        // to static-initialization order, and QByteArray::startsWith("") is
        // always true, which previously caused the first such branch to swallow
        // every packet (metadata, battery, ...). Routing by id avoids that.
        else if (data.size() >= 8 && data.startsWith(ControlCommand::HEADER))
        {
            const quint8 id = static_cast<quint8>(data.at(6));
            switch (id)
            {
            case 0x0D: // Noise control mode
                if (auto value = AirPodsPackets::NoiseControl::parseMode(data))
                {
                    m_deviceInfo->setNoiseControlMode(value.value());
                    LOG_INFO("Noise control mode received: " << m_deviceInfo->noiseControlMode());
                }
                break;
            case 0x28: // Conversational awareness
                if (auto result = AirPodsPackets::ConversationalAwareness::parseState(data))
                {
                    m_deviceInfo->setConversationalAwareness(result.value());
                    LOG_INFO("Conversational awareness state received: " << m_deviceInfo->conversationalAwareness());
                }
                break;
            case 0x2C: // Hearing aid
                if (auto result = AirPodsPackets::HearingAid::parseState(data))
                {
                    m_deviceInfo->setHearingAidEnabled(result.value());
                    LOG_INFO("Hearing aid state received: " << m_deviceInfo->hearingAidEnabled());
                }
                break;
            case 0x1B: // One-bud ANC
                if (auto value = AirPodsPackets::OneBudANCMode::parseState(data))
                {
                    m_deviceInfo->setOneBudANCMode(value.value());
                    LOG_INFO("One Bud ANC mode received: " << m_deviceInfo->oneBudANCMode());
                }
                break;
            default:
                LOG_DEBUG("Unhandled control-command id " << id << ": " << data.toHex());
                break;
            }
        }
        else
        {
            LOG_DEBUG("Unrecognized packet format: " << data.toHex());
        }
    }

    void connectToPhone() {
        if (!CrossDevice.isEnabled) {
            return;
        }

        if (phoneSocket && phoneSocket->isOpen()) {
            LOG_INFO("Already connected to the phone");
            return;
        }
        QBluetoothAddress phoneAddress("00:00:00:00:00:00"); // Default address, will be overwritten if PHONE_MAC_ADDRESS is set
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

        if (!env.value("PHONE_MAC_ADDRESS").isEmpty())
        {
            phoneAddress = QBluetoothAddress(env.value("PHONE_MAC_ADDRESS"));
        }
        phoneSocket = new QBluetoothSocket(QBluetoothServiceInfo::L2capProtocol);
        connect(phoneSocket, &QBluetoothSocket::connected, this, [this]() {
            LOG_INFO("Connected to phone");
            if (!lastBatteryStatus.isEmpty()) {
                phoneSocket->write(lastBatteryStatus);
                LOG_DEBUG("Sent last battery status to phone: " << lastBatteryStatus.toHex());
            }
            if (!lastEarDetectionStatus.isEmpty()) {
                phoneSocket->write(lastEarDetectionStatus);
                LOG_DEBUG("Sent last ear detection status to phone: " << lastEarDetectionStatus.toHex());
            }
        });

        connect(phoneSocket, QOverload<QBluetoothSocket::SocketError>::of(&QBluetoothSocket::errorOccurred), this, [this](QBluetoothSocket::SocketError error) {
            LOG_ERROR("Phone socket error: " << error << ", " << phoneSocket->errorString());
        });

        phoneSocket->connectToService(phoneAddress, QBluetoothUuid("1abbb9a4-10e4-4000-a75c-8953c5471342"));
    }

    void relayPacketToPhone(const QByteArray &packet)
    {
        if (!CrossDevice.isEnabled) {
            return;
        }
        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Phone::NOTIFICATION + packet);
        }
        else
        {
            connectToPhone();
            LOG_WARN("Phone socket is not open, cannot relay packet");
        }
    }

    void handlePhonePacket(const QByteArray &packet) {
        if (packet.startsWith(AirPodsPackets::Phone::NOTIFICATION))
        {
            QByteArray airpodsPacket = packet.mid(4);
            if (socket && socket->isOpen()) {
                socket->write(airpodsPacket);
                LOG_DEBUG("Relayed packet to AirPods: " << airpodsPacket.toHex());
            } else {
                LOG_ERROR("Socket is not open, cannot relay packet to AirPods");
            }
        }
        else if (packet.startsWith(AirPodsPackets::Phone::CONNECTED))
        {
            LOG_INFO("AirPods connected");
            isConnectedLocally = true;
            CrossDevice.isAvailable = false;
        }
        else if (packet.startsWith(AirPodsPackets::Phone::DISCONNECTED))
        {
            LOG_INFO("AirPods disconnected");
            isConnectedLocally = false;
            CrossDevice.isAvailable = true;
        }
        else if (packet.startsWith(AirPodsPackets::Phone::STATUS_REQUEST))
        {
            LOG_INFO("Connection status request received");
            QByteArray response = (socket && socket->isOpen()) ? AirPodsPackets::Phone::CONNECTED
                                                               : AirPodsPackets::Phone::DISCONNECTED;
            phoneSocket->write(response);
            LOG_DEBUG("Sent connection status response: " << response.toHex());
        }
        else if (packet.startsWith(AirPodsPackets::Phone::DISCONNECT_REQUEST))
        {
            LOG_INFO("Disconnect request received");
            if (socket && socket->isOpen()) {
                socket->close();
                LOG_INFO("Disconnected from AirPods");
                QProcess process;
                process.start("bluetoothctl", QStringList() << "disconnect" << m_deviceInfo->bluetoothAddress());
                process.waitForFinished();
                QString output = process.readAllStandardOutput().trimmed();
                LOG_INFO("Bluetoothctl output: " << output);
                isConnectedLocally = false;
                CrossDevice.isAvailable = true;
            }
        }
        else
        {
            if (socket && socket->isOpen()) {
                socket->write(packet);
                LOG_DEBUG("Relayed packet to AirPods: " << packet.toHex());
            } else {
                LOG_ERROR("Socket is not open, cannot relay packet to AirPods");
            }
        }
    }

    void onPhoneDataReceived() {
        QByteArray data = phoneSocket->readAll();
        LOG_DEBUG("Data received from phone: " << data.toHex());
        QMetaObject::invokeMethod(this, "handlePhonePacket", Qt::QueuedConnection, Q_ARG(QByteArray, data));
    }

    void bleDeviceFound(const BleInfo &device)
    {
        if (BLEUtils::isValidIrkRpa(m_deviceInfo->magicAccIRK(), device.address)) {
            m_deviceInfo->setModel(device.modelName);
            auto decryptet = BLEUtils::decryptLastBytes(device.encryptedPayload, m_deviceInfo->magicAccEncKey());
            m_deviceInfo->getBattery()->parseEncryptedPacket(decryptet, device.primaryLeft, device.isThisPodInTheCase, isModelHeadset(m_deviceInfo->model()));
            m_deviceInfo->getEarDetection()->overrideEarDetectionStatus(device.isPrimaryInEar, device.isSecondaryInEar);
        }
    }

    // Detect a pod being removed from the case (InCase -> out of case) so the UI
    // can show the connect flyout, like iOS does when you take an AirPod out.
    void checkCaseRemoval()
    {
        using Status = EarDetection::EarDetectionStatus;
        auto *ed = m_deviceInfo->getEarDetection();
        const Status p = ed->getprimaryStatus();
        const Status s = ed->getsecondaryStatus();

        const int inCaseCount = (p == Status::InCase ? 1 : 0) + (s == Status::InCase ? 1 : 0);
        const bool anyOutOfCasePresent =
            p == Status::InEar || p == Status::NotInEar ||
            s == Status::InEar || s == Status::NotInEar;

        // A pod just left the case (count dropped) and is now in hand/ear.
        if (inCaseCount < m_prevInCaseCount && anyOutOfCasePresent)
        {
            emit airPodsTakenOutOfCase();
        }
        m_prevInCaseCount = inCaseCount;
    }

public:
    void handleMediaStateChange(MediaController::MediaState state) {
        if (state == MediaController::MediaState::Playing) {
            LOG_INFO("Media started playing, sending disconnect request to Android and taking over audio");
            sendDisconnectRequestToAndroid();
            connectToAirPods(true);
        }
    }

    void sendDisconnectRequestToAndroid()
    {
        if (!CrossDevice.isEnabled) return;

        if (phoneSocket && phoneSocket->isOpen())
        {
            phoneSocket->write(AirPodsPackets::Phone::DISCONNECT_REQUEST);
            LOG_DEBUG("Sent disconnect request to Android: " << AirPodsPackets::Phone::DISCONNECT_REQUEST.toHex());
        }
        else
        {
            LOG_WARN("Phone socket is not open, cannot send disconnect request");
        }
    }

    void connectToAirPods(bool force) {
        if (socket && socket->isOpen()) {
            LOG_INFO("Already connected to AirPods");
            return;
        }

        if (force) {
            LOG_INFO("Forcing connection to AirPods");
            QProcess process;
            process.start("bluetoothctl", QStringList() << "connect" << m_deviceInfo->bluetoothAddress());
            process.waitForFinished();
            QString output = process.readAllStandardOutput().trimmed();
            LOG_INFO("Bluetoothctl output: " << output);
        }
        QBluetoothLocalDevice localDevice;
        const QList<QBluetoothAddress> connectedDevices = localDevice.connectedDevices();
        for (const QBluetoothAddress &address : connectedDevices) {
            QBluetoothDeviceInfo device(address, "", 0);
            LOG_DEBUG("Connected device: " << device.name() << " (" << device.address().toString() << ")");
            if (isAirPodsDevice(device)) {
                connectToDevice(device);
                return;
            }
        }
        LOG_WARN("AirPods not found among connected devices");
    }

    void initializeBluetooth() {
        connectToPhone();

        m_deviceInfo->loadFromSettings(*m_settings);
        if (!areAirpodsConnected()) {
            m_bleManager->startScan();
        }
    }

    void loadMainModule() {
        parent->load(QUrl(QStringLiteral("qrc:/qt/qml/windows/Main.qml")));
    }

    void reopenOrLoadModule(const QString &pageToLoad = "app") {
        if (parent->rootObjects().isEmpty())
        {
            LOG_WARN("Root objects list is empty, loading main module");
            loadMainModule();
            return;
        }
        QObject *rootObject = parent->rootObjects().first();
        QMetaObject::invokeMethod(rootObject, "reopen", Q_ARG(QVariant, pageToLoad));
    }

signals:
    void noiseControlModeChanged(NoiseControlMode mode);
    void earDetectionStatusChanged(const QString &status);
    void batteryStatusChanged(const QString &status);
    void conversationalAwarenessChanged(bool enabled);
    void adaptiveNoiseLevelChanged(int level);
    void deviceNameChanged(const QString &name);
    void modelChanged();
    void primaryChanged();
    void airPodsStatusChanged();
    void airPodsTakenOutOfCase();
    void earDetectionBehaviorChanged(int behavior);
    void crossDeviceEnabledChanged(bool enabled);
    void notificationsEnabledChanged(bool enabled);
    void retryAttemptsChanged(int attempts);
    void oneBudANCModeChanged(bool enabled);
    void phoneMacStatusChanged();
    void hearingAidEnabledChanged(bool enabled);

private:
    AapSocket *socket = nullptr;
    QBluetoothSocket *phoneSocket = nullptr;
    QByteArray lastBatteryStatus;
    QByteArray lastEarDetectionStatus;
    MediaController* mediaController;
    TrayIconManager *trayManager;
    QSettings *m_settings;
    AutoStartManager *m_autoStartManager;
    int m_retryAttempts = 3;
    int m_prevInCaseCount = 0;
    bool m_hideOnStart = false;
    DeviceInfo *m_deviceInfo;
    BleManager *m_bleManager;
    WindowsSleepMonitor *m_systemSleepMonitor = nullptr;
    QTimer *m_winConnPoller = nullptr;
    QString m_phoneMacStatus;
};

// Only write the on-disk log when launched with --debug; normal launches log nothing.
static bool g_fileLogging = false;

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    // Install message handler to ensure logs are printed to console on Windows
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &msg) {
        QByteArray localMsg = msg.toLocal8Bit();
        FILE *output = (type == QtDebugMsg || type == QtInfoMsg) ? stdout : stderr;

        const char *level = "INFO";
        switch (type) {
        case QtDebugMsg:    level = "DEBUG";   break;
        case QtInfoMsg:     level = "INFO";    break;
        case QtWarningMsg:  level = "WARNING"; break;
        case QtCriticalMsg: level = "ERROR";   break;
        case QtFatalMsg:    level = "FATAL";   break;
        }
        fprintf(output, "[%s] %s\n", level, localMsg.constData());
        fflush(output);

        // With --debug, also append to a log file we can inspect (a WIN32 GUI app
        // has no console): %APPDATA%\LibrePods\librepods.log
        if (g_fileLogging) {
            static QFile logFile;
            static bool inited = false;
            if (!inited) {
                inited = true;
                QString dir = QString::fromLocal8Bit(qgetenv("APPDATA")) + "/LibrePods";
                QDir().mkpath(dir);
                logFile.setFileName(dir + "/librepods.log");
                logFile.open(QIODevice::Append | QIODevice::Text);
            }
            if (logFile.isOpen()) {
                QTextStream ts(&logFile);
                ts << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
                   << " [" << level << "] " << msg << "\n";
                ts.flush();
            }
        }

        if (type == QtFatalMsg)
            abort();
    });

    QSharedMemory sharedMemory;
    sharedMemory.setKey("TcpServer-Key2");

    // Check if app is already open
    if(sharedMemory.create(1) == false)
    {
        LOG_INFO("Another instance already running! Opening App Window Instead");
        QLocalSocket socket;
        // Connect to the original app, then trigger the reopen signal
        socket.connectToServer("app_server");
        if (socket.waitForConnected(500)) {
            socket.write("reopen");
            socket.flush();
            socket.waitForBytesWritten(500);
            socket.disconnectFromServer();
            app.exit(); // exit; process already running
            return 0;
        }
        else
        {
            // Failed connection, log and open the app (assume it's not running)
            LOG_ERROR("Failed to connect to the original app instance. Assuming it is not running.");
            LOG_DEBUG("Socket error: " << socket.errorString());
        }
    }
    app.setDesktopFileName("me.kavishdevar.librepods");
    app.setQuitOnLastWindowClosed(false);

    bool debugMode = false;
    bool hideOnStart = false;
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "--debug")
            debugMode = true;

        if (QString(argv[i]) == "--hide")
            hideOnStart = true;
    }
    g_fileLogging = debugMode; // only write the on-disk log under --debug

    // Use the Windows 11 Fluent / WinUI 3 control style. Must be set before any
    // QML is loaded. Standard controls (Switch, Slider, ComboBox, Button) adopt
    // it automatically; hand-drawn components keep their custom look.
    QQuickStyle::setStyle("FluentWinUI3");

    // Use the Windows 11 system UI font (Segoe UI Variable Text), falling back to
    // Segoe UI on Windows 10 and earlier.
    {
        QFont uiFont;
        uiFont.setFamilies({QStringLiteral("Segoe UI Variable Text"), QStringLiteral("Segoe UI")});
        uiFont.setPointSize(9);
        app.setFont(uiFont);
    }

    QQmlApplicationEngine engine;
    qmlRegisterType<Battery>("me.kavishdevar.Battery", 1, 0, "Battery");
    qmlRegisterType<DeviceInfo>("me.kavishdevar.DeviceInfo", 1, 0, "DeviceInfo");
    AirPodsTrayApp *trayApp = new AirPodsTrayApp(debugMode, hideOnStart, &engine);
    engine.rootContext()->setContextProperty("airPodsTrayApp", trayApp);

    // Expose PHONE_MAC_ADDRESS environment variable to QML for placeholder in settings
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString phoneMacEnv = env.value("PHONE_MAC_ADDRESS", "");
        engine.rootContext()->setContextProperty("PHONE_MAC_ADDRESS", phoneMacEnv);
        // Initialize the visible status in the GUI
        trayApp->updatePhoneMacStatus(phoneMacEnv.isEmpty() ? QStringLiteral("No phone MAC set") : phoneMacEnv);
    }

    trayApp->loadMainModule();
    
    // Verify QML loaded successfully
    if (engine.rootObjects().isEmpty())
    {
        LOG_ERROR("Failed to load QML module - root objects list is empty");
        LOG_ERROR("Critical error: Unable to load main QML interface. The application cannot start.");
        return 1;
    }
    LOG_INFO("Main QML module loaded successfully");

    QLocalServer server;
    QLocalServer::removeServer("app_server");

    if (!server.listen("app_server"))
    {
        LOG_ERROR("Unable to start the listening server");
        LOG_DEBUG("Server error: " << server.errorString());
    }
    else
    {
        LOG_DEBUG("Server started, waiting for connections...");
    }
    QObject::connect(&server, &QLocalServer::newConnection, [&]() {
        QLocalSocket* socket = server.nextPendingConnection();
        // Handles Proper Connection
        QObject::connect(socket, &QLocalSocket::readyRead, [socket, &trayApp]() {
            QString msg = socket->readAll();
            // Check if the message is "reopen", if so, trigger onOpenApp function
            if (msg == "reopen") {
                LOG_INFO("Reopening app window");
                trayApp->reopenOrLoadModule("app");
            }
            else
            {
                LOG_ERROR("Unknown message received: " << msg);
            }
            socket->disconnectFromServer();
        });
        // Handles connection errors
        QObject::connect(socket, &QLocalSocket::errorOccurred, [socket]() {
            LOG_ERROR("Failed to connect to the duplicate app instance");
            LOG_DEBUG("Connection error: " << socket->errorString());
        });

        // Handle server-level errors
        QObject::connect(&server, &QLocalServer::serverError, [&]() {
            LOG_ERROR("Server failed to accept a new connection");
            LOG_DEBUG("Server error: " << server.errorString());
        });
    });

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&]() {
        LOG_DEBUG("Application is about to quit. Cleaning up...");
        sharedMemory.detach();
    });
    return app.exec();
}

#include "main.moc"
