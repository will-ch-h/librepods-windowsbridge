// Windows L2CAP transport for the AirPods AAP channel.
//
// Qt's QBluetoothSocket cannot do L2CAP on Windows (the Microsoft Bluetooth
// stack blocks user-mode L2CAP). Instead we talk to a kernel-mode Bluetooth
// profile driver (bthecho-derived, e.g. MagicAAP) that is bound to the AirPods
// AAP service node and exposes a device interface we open with CreateFile.
//
// This class is API-compatible with the subset of QBluetoothSocket that
// main.cpp uses, so it can be aliased in behind Q_OS_WIN.
#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QMutex>
#include <QQueue>
#include <QBluetoothAddress>
#include <QBluetoothUuid>
#include <QBluetoothServiceInfo>
#include <thread>
#include <atomic>
#include <memory>

class WinL2capSocket : public QObject
{
    Q_OBJECT
public:
    enum SocketState { UnconnectedState, ConnectingState, ConnectedState };
    enum SocketError { NoSocketError, UnknownSocketError, HostNotFoundError, ConnectionRefusedError };

    explicit WinL2capSocket(QObject *parent = nullptr);
    // Match `new QBluetoothSocket(QBluetoothServiceInfo::L2capProtocol)`.
    explicit WinL2capSocket(QBluetoothServiceInfo::Protocol protocol, QObject *parent = nullptr);
    ~WinL2capSocket() override;

    // Enumerate AirPods currently reachable via the AAP profile driver
    // (one device-interface instance exists per connected AirPods).
    static QList<QBluetoothAddress> connectedAirPods();

    // Resolve the driver's device interface for `address`, open the channel.
    void connectToService(const QBluetoothAddress &address, const QBluetoothUuid &uuid);

    bool isOpen() const;
    SocketState state() const { return m_state; }
    QBluetoothAddress peerAddress() const { return m_peer; }
    QString errorString() const { return m_error; }

    qint64 write(const QByteArray &data);
    QByteArray readAll();
    void close();

signals:
    void connected();
    void readyRead();
    void errorOccurred(WinL2capSocket::SocketError error);
    // Emitted when the channel drops on its own (device went away), as opposed
    // to a deliberate close(). API-compatible with QBluetoothSocket::disconnected.
    void disconnected();

private:
    void startReader();
    void stopReader();
    void failWith(SocketError err, const QString &msg);

    void *m_handle = nullptr;       // HANDLE (void* to keep windows.h out of the header)
    void *m_cancelEvent = nullptr;  // HANDLE, signalled to break the read loop
    std::thread m_reader;
    std::atomic<bool> m_running{false};

    SocketState m_state = UnconnectedState;
    QBluetoothAddress m_peer;
    QString m_error;

    QMutex m_rxMutex;
    // One entry per received L2CAP message; preserves AAP packet framing.
    QQueue<QByteArray> m_rxQueue;

    // connectToService() resolves the device path and opens the handle on a
    // detached worker thread (CreateFile can hang for seconds on a stale
    // device node). That thread outlives any single connect attempt, so it
    // must never touch `this` after the socket has been destroyed (e.g. a
    // caller tore it down to retry while the previous attempt was still in
    // flight) -- this flag, kept alive by the thread's own shared_ptr copy,
    // is how it finds out.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};
