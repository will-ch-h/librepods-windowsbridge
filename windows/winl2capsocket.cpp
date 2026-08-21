#include "winl2capsocket.h"
#include "logger.h"

#include <QMetaObject>
#include <QRegularExpression>
#include <QList>

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")

Q_LOGGING_CATEGORY(winl2cap, "librepods.winl2cap")

// Device interface GUID exposed by the AAP profile driver (bthecho-derived).
// {9eec98bb-3c54-45d4-a843-7900c4635e08}
static const GUID kAapInterfaceGuid =
{ 0x9eec98bb, 0x3c54, 0x45d4, { 0xa8, 0x43, 0x79, 0x00, 0xc4, 0x63, 0x5e, 0x08 } };

WinL2capSocket::WinL2capSocket(QObject *parent) : QObject(parent) {}
WinL2capSocket::WinL2capSocket(QBluetoothServiceInfo::Protocol, QObject *parent) : QObject(parent) {}

WinL2capSocket::~WinL2capSocket()
{
    *m_alive = false;
    close();
}

bool WinL2capSocket::isOpen() const
{
    return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE && m_state == ConnectedState;
}

// Find the driver's device-interface path for the given Bluetooth address.
static QString resolveInterfacePath(const QBluetoothAddress &address)
{
    // e.g. "70:AE:D5:C8:EC:3D" -> "70aed5c8ec3d" to match inside the device path
    QString needle = address.toString().remove(':').toLower();

    HDEVINFO di = SetupDiGetClassDevs(&kAapInterfaceGuid, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE)
        return QString();

    QString result;
    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &kAapInterfaceGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(di, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA *>(malloc(need));
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (SetupDiGetDeviceInterfaceDetail(di, &ifd, detail, need, nullptr, nullptr)) {
            QString path = QString::fromWCharArray(detail->DevicePath);
            if (needle.isEmpty() || path.toLower().contains(needle)) {
                result = path;
                free(detail);
                break;
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(di);
    return result;
}

QList<QBluetoothAddress> WinL2capSocket::connectedAirPods()
{
    QList<QBluetoothAddress> result;
    HDEVINFO di = SetupDiGetClassDevs(&kAapInterfaceGuid, nullptr, nullptr,
                                      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE)
        return result;

    SP_DEVICE_INTERFACE_DATA ifd; ifd.cbSize = sizeof(ifd);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &kAapInterfaceGuid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(di, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA *>(malloc(need));
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (SetupDiGetDeviceInterfaceDetail(di, &ifd, detail, need, nullptr, nullptr)) {
            // Path embeds the address, e.g. ...&0&70aed5c8ec3d_c00000000#{...}
            QString path = QString::fromWCharArray(detail->DevicePath);
            QRegularExpression re("&([0-9a-fA-F]{12})_c", QRegularExpression::CaseInsensitiveOption);
            auto m = re.match(path);
            if (m.hasMatch()) {
                QString hex = m.captured(1).toUpper();
                QString mac = QStringLiteral("%1:%2:%3:%4:%5:%6")
                    .arg(hex.mid(0,2), hex.mid(2,2), hex.mid(4,2), hex.mid(6,2), hex.mid(8,2), hex.mid(10,2));
                QBluetoothAddress addr(mac);
                if (!result.contains(addr)) result.append(addr);
            }
        }
        free(detail);
    }
    SetupDiDestroyDeviceInfoList(di);
    return result;
}

void WinL2capSocket::connectToService(const QBluetoothAddress &address, const QBluetoothUuid &)
{
    m_peer = address;
    m_state = ConnectingState;

    // Resolving the device interface and opening it happens on a worker
    // thread: CreateFile can hang indefinitely on a stale/ghost device node
    // (e.g. Windows still lists the AAP interface for AirPods that are no
    // longer actually reachable), and this is polled periodically from the
    // GUI thread — a hang here used to freeze the whole app.
    //
    // The thread can easily outlive this WinL2capSocket (a caller may tear
    // it down and start a new connect attempt while this one is still stuck
    // in CreateFile), so it must check m_alive -- kept alive via its own
    // shared_ptr copy -- before ever touching `this` again.
    std::shared_ptr<std::atomic<bool>> alive = m_alive;
    std::thread([this, address, alive]() {
        QString path = resolveInterfacePath(address);
        if (path.isEmpty()) {
            if (!*alive) return;
            QMetaObject::invokeMethod(this, [this, alive]() {
                if (!*alive) return;
                failWith(HostNotFoundError,
                         "AAP device interface not found. Is the L2CAP driver installed and the AirPods connected?");
            }, Qt::QueuedConnection);
            return;
        }
        LOG_INFO("WinL2capSocket: opening " << path);

        HANDLE h = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                               GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (!*alive) return;
            QMetaObject::invokeMethod(this, [this, e, alive]() {
                if (!*alive) return;
                failWith(ConnectionRefusedError,
                         QString("CreateFile failed (error %1)").arg(e));
            }, Qt::QueuedConnection);
            return;
        }

        if (!*alive) {
            CloseHandle(h);
            return;
        }
        QMetaObject::invokeMethod(this, [this, h, alive]() {
            if (!*alive || m_state != ConnectingState) {
                // Destroyed, or close() was called (or another connect
                // attempt finished) while this open was in flight; don't
                // resurrect the socket.
                CloseHandle(h);
                return;
            }
            m_handle = h;
            m_state = ConnectedState;
            startReader();
            emit connected();
        }, Qt::QueuedConnection);
    }).detach();
}

void WinL2capSocket::startReader()
{
    m_cancelEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    m_running = true;
    HANDLE h = static_cast<HANDLE>(m_handle);
    HANDLE cancel = static_cast<HANDLE>(m_cancelEvent);

    m_reader = std::thread([this, h, cancel]() {
        OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        char buf[1024];
        while (m_running.load()) {
            ResetEvent(ov.hEvent);
            DWORD got = 0;
            BOOL ok = ReadFile(h, buf, sizeof(buf), &got, &ov);
            if (!ok) {
                DWORD e = GetLastError();
                if (e == ERROR_IO_PENDING) {
                    HANDLE waits[2] = { ov.hEvent, cancel };
                    DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                    if (w != WAIT_OBJECT_0) { CancelIoEx(h, &ov); break; } // cancelled
                    if (!GetOverlappedResult(h, &ov, &got, FALSE)) break;
                } else {
                    break; // read error / device gone
                }
            }
            if (got == 0) continue;
            {
                QMutexLocker lock(&m_rxMutex);
                // Each ReadFile returns one L2CAP SDU; queue it as a discrete
                // message so readAll() hands parseData() one packet at a time.
                m_rxQueue.enqueue(QByteArray(buf, static_cast<int>(got)));
            }
            QMetaObject::invokeMethod(this, [this]() { emit readyRead(); }, Qt::QueuedConnection);
        }
        CloseHandle(ov.hEvent);

        // If we left the loop while still "running", it wasn't a deliberate
        // close()/cancel — the device went away. Report it so the app cleans up
        // and can reconnect when the AirPods come back. exchange() ensures only
        // the device-gone path (not stopReader) fires this.
        if (m_running.exchange(false)) {
            QMetaObject::invokeMethod(this, [this]() {
                m_state = UnconnectedState;
                emit disconnected();
            }, Qt::QueuedConnection);
        }
    });
}

void WinL2capSocket::stopReader()
{
    m_running = false;
    if (m_cancelEvent) SetEvent(static_cast<HANDLE>(m_cancelEvent));
    if (m_reader.joinable()) m_reader.join();
    if (m_cancelEvent) { CloseHandle(static_cast<HANDLE>(m_cancelEvent)); m_cancelEvent = nullptr; }
}

qint64 WinL2capSocket::write(const QByteArray &data)
{
    if (!isOpen()) return -1;
    HANDLE h = static_cast<HANDLE>(m_handle);
    OVERLAPPED ov{}; ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, data.constData(), static_cast<DWORD>(data.size()), &wrote, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 2000);
        GetOverlappedResult(h, &ov, &wrote, FALSE);
    }
    CloseHandle(ov.hEvent);
    return static_cast<qint64>(wrote);
}

QByteArray WinL2capSocket::readAll()
{
    QMutexLocker lock(&m_rxMutex);
    if (m_rxQueue.isEmpty())
        return QByteArray();
    return m_rxQueue.dequeue();
}

void WinL2capSocket::close()
{
    if (m_state == UnconnectedState && m_handle == nullptr) return;
    stopReader();
    if (m_handle && m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
    m_handle = nullptr;
    m_state = UnconnectedState;
}

void WinL2capSocket::failWith(SocketError err, const QString &msg)
{
    m_error = msg;
    m_state = UnconnectedState;
    LOG_ERROR("WinL2capSocket error: " << msg);
    QMetaObject::invokeMethod(this, [this, err]() { emit errorOccurred(err); }, Qt::QueuedConnection);
}
