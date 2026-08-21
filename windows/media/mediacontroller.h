#ifndef MEDIACONTROLLER_H
#define MEDIACONTROLLER_H

#include <QObject>

#include "windowsaudiocontroller.h"

class QTimer;
class EarDetection;
class PlayerStatusWatcher;

class MediaController : public QObject
{
  Q_OBJECT
public:
  enum MediaState
  {
    Playing,
    Paused,
    Stopped
  };
  Q_ENUM(MediaState)
  enum EarDetectionBehavior
  {
    PauseWhenOneRemoved,
    PauseWhenBothRemoved,
    Disabled
  };
  Q_ENUM(EarDetectionBehavior)

  explicit MediaController(QObject *parent = nullptr);
  ~MediaController();

  void handleEarDetection(EarDetection*);
  void followMediaChanges();
  bool isActiveOutputDeviceAirPods();
  void handleConversationalAwareness(const QByteArray &data);
  void activateA2dpProfile();
  void removeAudioOutputDevice();
  void setConnectedDeviceMacAddress(const QString &macAddress);

  void setEarDetectionBehavior(EarDetectionBehavior behavior);
  inline EarDetectionBehavior getEarDetectionBehavior() const { return earDetectionBehavior; }

  void play();
  void pause();
  MediaState getCurrentMediaState() const;

Q_SIGNALS:
  void mediaStateChanged(MediaState state);

private:
  MediaState mediaStateFromPlayerctlOutput(const QString &output) const;
  QString getAudioDeviceName();
  QString getDefaultSink();
  int getSinkVolume(const QString &sinkName);
  bool setSinkVolume(const QString &sinkName, int volumePercent);
  QString getCardNameForDevice(const QString &macAddress);
  bool setCardProfile(const QString &cardName, const QString &profileName);

  // Drives a smooth conversational-awareness volume transition toward
  // m_caTargetVolume (fast when ducking, gentle when restoring) instead of
  // snapping to the coarse level the AirPods send.
  void stepCaVolumeRamp();
  // Fires after a quiet period to ramp the volume back to baseline. Held off
  // (restarted) while the AirPods keep reporting non-speech so the duck bridges
  // the short gaps between words/sentences.
  void beginCaRestore();

  // Make the AirPods the default Windows output, retrying while the audio
  // endpoint comes up (it lags the control channel after a reconnect).
  void activateWindowsAudioOutput();
  QTimer *m_winAudioRetryTimer = nullptr;
  int m_winAudioRetries = 0;

  bool m_pausedByEarDetection = false;
  int initialVolume = -1;
  QTimer *m_caRampTimer = nullptr;
  QTimer *m_caHoldTimer = nullptr;
  int m_caTargetVolume = -1;   // desired sink volume %, -1 when idle
  int m_caCurrentVolume = -1;  // our tracked current %, avoids re-reading the sink each tick
  QString m_caSink;
  QString connectedDeviceMacAddress;
  EarDetectionBehavior earDetectionBehavior = PauseWhenOneRemoved;
  QString m_deviceOutputName;
  PlayerStatusWatcher *playerStatusWatcher = nullptr;

  WindowsAudioController *m_windowsAudio = nullptr;
};

#endif // MEDIACONTROLLER_H
