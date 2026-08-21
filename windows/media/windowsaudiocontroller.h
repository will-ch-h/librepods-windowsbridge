#ifndef WINDOWSAUDIOCONTROLLER_H
#define WINDOWSAUDIOCONTROLLER_H

#include <QString>
#include <QObject>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#endif

class WindowsAudioController : public QObject
{
    Q_OBJECT

public:
    explicit WindowsAudioController(QObject *parent = nullptr);
    ~WindowsAudioController();

    bool initialize();
    QString getDefaultSink();
    int getSinkVolume(const QString &sinkName);
    bool setSinkVolume(const QString &sinkName, int volumePercent);
    bool setCardProfile(const QString &cardName, const QString &profileName);
    QString getCardNameForDevice(const QString &macAddress);

    // Media transport control via Windows System Media Transport Controls (SMTC).
    // getMediaPlaybackStatus returns 0=Playing, 1=Paused, 2=Stopped, -1=no session.
    int getMediaPlaybackStatus();
    bool pauseMedia();
    bool playMedia();

    // True if the current default render endpoint is the AirPods.
    bool isDefaultOutputAirPods();

    // Make the AirPods the default playback device (all roles). Returns false if
    // no AirPods render endpoint is enumerated yet (e.g. just after reconnect),
    // so the caller can retry. Prefers the stereo endpoint over hands-free.
    bool makeAirPodsDefaultOutput(const QString &macAddress);

private:
#ifdef Q_OS_WIN
    IMMDeviceEnumerator *m_deviceEnumerator;
    IAudioEndpointVolume *m_endpointVolume;
#endif
    bool m_initialized;

    QString getDeviceFriendlyName(void *device);
    bool setDefaultAudioDevice(const QString &deviceId);
};

#endif // WINDOWSAUDIOCONTROLLER_H
