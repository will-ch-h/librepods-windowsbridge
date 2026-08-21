#include "mediacontroller.h"
#include "logger.h"
#include "eardetection.hpp"
#include "playerstatuswatcher.h"
#include "windowsaudiocontroller.h"

#include <QByteArray>
#include <QString>
#include <QTimer>

MediaController::MediaController(QObject *parent) : QObject(parent) {
  m_windowsAudio = new WindowsAudioController(this);
  if (!m_windowsAudio->initialize())
  {
    LOG_ERROR("Failed to initialize Windows Audio controller");
  }
}

void MediaController::handleEarDetection(EarDetection *earDetection)
{
  if (earDetectionBehavior == Disabled)
  {
    LOG_DEBUG("Ear detection is disabled, ignoring status");
    return;
  }

  bool primaryInEar = earDetection->isPrimaryInEar();
  bool secondaryInEar = earDetection->isSecondaryInEar();

  LOG_DEBUG("Ear detection status: primaryInEar="
            << primaryInEar << ", secondaryInEar=" << secondaryInEar
            << ", isAirPodsActive=" << isActiveOutputDeviceAirPods());

  // First handle playback pausing based on selected behavior
  bool shouldPause = false;
  bool shouldResume = false;

  if (earDetectionBehavior == PauseWhenOneRemoved)
  {
    shouldPause = !primaryInEar || !secondaryInEar;
    shouldResume = primaryInEar && secondaryInEar;
  }
  else if (earDetectionBehavior == PauseWhenBothRemoved)
  {
    shouldPause = !primaryInEar && !secondaryInEar;
    shouldResume = primaryInEar || secondaryInEar;
  }

  if (shouldPause && isActiveOutputDeviceAirPods())
  {
    if (getCurrentMediaState() == Playing)
    {
      LOG_DEBUG("Pausing playback for ear detection");
      pause();
    }
  }

  // Then handle device profile switching
  if (primaryInEar || secondaryInEar)
  {
    LOG_INFO("At least one AirPod is in ear");
    activateA2dpProfile();

    // Resume if conditions are met and we previously paused
    if (shouldResume && m_pausedByEarDetection && isActiveOutputDeviceAirPods())
    {
      play();
    }
  }
  else
  {
    LOG_INFO("Both AirPods are out of ear");
    removeAudioOutputDevice();
  }
}

void MediaController::setEarDetectionBehavior(EarDetectionBehavior behavior)
{
  earDetectionBehavior = behavior;
  LOG_INFO("Set ear detection behavior to: " << behavior);
}

void MediaController::followMediaChanges() {
  playerStatusWatcher = new PlayerStatusWatcher("", this);
  connect(playerStatusWatcher, &PlayerStatusWatcher::playbackStatusChanged,
          this, [this](const QString &status)
          {
            LOG_DEBUG("Playback status changed: " << status);
            MediaState state = mediaStateFromPlayerctlOutput(status);
            emit mediaStateChanged(state);
          });
}

bool MediaController::isActiveOutputDeviceAirPods() {
  // Windows audio endpoint IDs are device-interface GUIDs that don't contain the
  // MAC address, so match on the default render endpoint's friendly name instead.
  return m_windowsAudio && m_windowsAudio->isDefaultOutputAirPods();
}

namespace {
// Volume to duck to while the wearer is speaking, as a fraction of their
// original volume. The AirPods report a coarse "level" byte, but rather than
// chase it (which makes the volume pump as the device ramps in/out per word)
// we duck to a fixed depth and smooth the transitions ourselves.
constexpr double kCaDuckFloor = 0.20;

// Per-tick step sizes (percentage points): duck fast when speech starts,
// restore gently so the music eases back rather than snapping.
constexpr int kCaRampIntervalMs = 30;
constexpr int kCaDuckStep = 9;     // ~0.27s to fully duck
constexpr int kCaRestoreStep = 2;  // ~1.2s to fully restore

// The AirPods drop back to "normal" between every word/sentence. Wait this long
// with no fresh speech before actually restoring, so the duck bridges those
// gaps instead of pumping up and down as you talk.
constexpr int kCaHoldMs = 1200;
} // namespace

void MediaController::handleConversationalAwareness(const QByteArray &data) {
  if (data.size() < 10) {
    return;
  }
  const quint8 level = static_cast<quint8>(data[9]);
  LOG_DEBUG("Handling conversational awareness data: " << data.toHex() << " level=" << level);

  if (!isActiveOutputDeviceAirPods()) {
    LOG_DEBUG("AirPods not the active output, ignoring conversational awareness");
    return;
  }

  if (!m_caRampTimer) {
    m_caRampTimer = new QTimer(this);
    m_caRampTimer->setInterval(kCaRampIntervalMs);
    connect(m_caRampTimer, &QTimer::timeout, this, &MediaController::stepCaVolumeRamp);
  }
  if (!m_caHoldTimer) {
    m_caHoldTimer = new QTimer(this);
    m_caHoldTimer->setSingleShot(true);
    connect(m_caHoldTimer, &QTimer::timeout, this, &MediaController::beginCaRestore);
  }

  // Levels 0x01/0x02 mean the wearer is actively speaking; anything higher is
  // the device ramping back toward normal (i.e. a pause/end of speech).
  const bool speaking = (level <= 0x02);

  if (speaking) {
    // Capture the user's baseline volume the first time we duck.
    if (initialVolume == -1) {
      const QString defaultSink = getDefaultSink();
      initialVolume = getSinkVolume(defaultSink);
      if (initialVolume == -1) {
        LOG_ERROR("Failed to get initial volume");
        return;
      }
      m_caCurrentVolume = initialVolume;
      m_caSink = defaultSink;
      LOG_DEBUG("CA: captured baseline volume " << initialVolume << "%");
    }

    // Active speech cancels any pending restore and ducks immediately.
    m_caHoldTimer->stop();
    m_caTargetVolume = qRound(initialVolume * kCaDuckFloor);
    LOG_DEBUG("CA level " << level << " (speaking) -> duck target " << m_caTargetVolume << "%");
    if (!m_caRampTimer->isActive()) {
      m_caRampTimer->start();
    }
  } else {
    // Not speaking. If nothing is ducked there's nothing to do; otherwise hold
    // the current duck and (re)arm the restore timer. Every non-speech packet
    // restarts it, so we only restore once speech has truly stopped.
    if (initialVolume == -1) {
      return;
    }
    m_caHoldTimer->start(kCaHoldMs);
  }
}

void MediaController::beginCaRestore() {
  if (initialVolume == -1) {
    return;
  }
  LOG_DEBUG("CA: hold elapsed, restoring volume to baseline " << initialVolume << "%");
  m_caTargetVolume = initialVolume;
  if (!m_caRampTimer->isActive()) {
    m_caRampTimer->start();
  }
}

void MediaController::stepCaVolumeRamp() {
  if (m_caTargetVolume < 0 || m_caCurrentVolume < 0) {
    m_caRampTimer->stop();
    return;
  }

  const int diff = m_caTargetVolume - m_caCurrentVolume;
  if (diff == 0) {
    // Reached the target; stop ticking until the next packet moves the target.
    m_caRampTimer->stop();
    // If we're fully back to the user's baseline, the conversation is over:
    // release everything so the next one re-captures the current volume.
    if (m_caTargetVolume >= initialVolume) {
      LOG_INFO("CA: volume restored to baseline " << initialVolume << "%");
      initialVolume = -1;
      m_caTargetVolume = -1;
      m_caCurrentVolume = -1;
    }
    return;
  }

  if (diff < 0) {
    m_caCurrentVolume = qMax(m_caTargetVolume, m_caCurrentVolume - kCaDuckStep);
  } else {
    m_caCurrentVolume = qMin(m_caTargetVolume, m_caCurrentVolume + kCaRestoreStep);
  }
  setSinkVolume(m_caSink, m_caCurrentVolume);
}

namespace { constexpr int kMaxWinAudioRetries = 8; } // ~12s at 1.5s/attempt

void MediaController::activateWindowsAudioOutput() {
  if (!m_windowsAudio) {
    return;
  }
  if (m_windowsAudio->makeAirPodsDefaultOutput(connectedDeviceMacAddress)) {
    LOG_INFO("AirPods set as default Windows audio output");
    m_deviceOutputName = getAudioDeviceName(); // now resolvable; also fixes pause gate
    m_winAudioRetries = 0;
    return;
  }
  // The audio endpoint lags the control channel after a reconnect; retry.
  if (m_winAudioRetries >= kMaxWinAudioRetries) {
    LOG_WARN("AirPods audio endpoint never appeared; giving up output takeover");
    m_winAudioRetries = 0;
    return;
  }
  m_winAudioRetries++;
  if (!m_winAudioRetryTimer) {
    m_winAudioRetryTimer = new QTimer(this);
    m_winAudioRetryTimer->setSingleShot(true);
    connect(m_winAudioRetryTimer, &QTimer::timeout, this, &MediaController::activateWindowsAudioOutput);
  }
  LOG_DEBUG("AirPods audio endpoint not ready, retry " << m_winAudioRetries << "/" << kMaxWinAudioRetries);
  m_winAudioRetryTimer->start(1500);
}

void MediaController::activateA2dpProfile() {
  activateWindowsAudioOutput();
}

void MediaController::removeAudioOutputDevice() {
  if (connectedDeviceMacAddress.isEmpty() || m_deviceOutputName.isEmpty()) {
    LOG_WARN("Connected device MAC address or output name is empty, cannot remove audio output device");
    return;
  }

  LOG_INFO("Removing AirPods as audio output device");
  if (!setCardProfile(m_deviceOutputName, "off")) {
    LOG_ERROR("Failed to remove AirPods as audio output device");
  }
}

void MediaController::setConnectedDeviceMacAddress(const QString &macAddress) {
  connectedDeviceMacAddress = macAddress;
  m_deviceOutputName = getAudioDeviceName();
  m_winAudioRetries = 0; // fresh retry budget for this connection
  LOG_INFO("Device output name set to: " << m_deviceOutputName);
}

MediaController::MediaState MediaController::mediaStateFromPlayerctlOutput(
    const QString &output) const {
  if (output == "Playing") {
    return MediaState::Playing;
  } else if (output == "Paused") {
    return MediaState::Paused;
  } else {
    return MediaState::Stopped;
  }
}

MediaController::MediaState MediaController::getCurrentMediaState() const
{
  if (m_windowsAudio)
  {
    switch (m_windowsAudio->getMediaPlaybackStatus())
    {
    case 0:
      return Playing;
    case 1:
      return Paused;
    default:
      return Stopped;
    }
  }
  return Stopped;
}

void MediaController::play()
{
  if (!m_pausedByEarDetection)
  {
    LOG_INFO("Nothing to resume");
    return;
  }

  if (m_windowsAudio && m_windowsAudio->playMedia())
  {
    LOG_INFO("Resumed playback via Windows SMTC");
  }
  else
  {
    LOG_WARN("Failed to resume playback via Windows SMTC");
  }
  m_pausedByEarDetection = false;
}

void MediaController::pause()
{
  if (m_windowsAudio && m_windowsAudio->pauseMedia())
  {
    LOG_INFO("Paused playback via Windows SMTC");
    m_pausedByEarDetection = true;
  }
  else
  {
    LOG_WARN("Failed to pause playback via Windows SMTC");
  }
}

MediaController::~MediaController() {
}

QString MediaController::getAudioDeviceName()
{
  if (connectedDeviceMacAddress.isEmpty()) { return QString(); }

  QString cardName = getCardNameForDevice(connectedDeviceMacAddress);
  if (cardName.isEmpty()) {
    LOG_ERROR("No matching Bluetooth card found for MAC address: " << connectedDeviceMacAddress);
  }
  return cardName;
}

QString MediaController::getDefaultSink()
{
  return m_windowsAudio ? m_windowsAudio->getDefaultSink() : QString();
}

int MediaController::getSinkVolume(const QString &sinkName)
{
  return m_windowsAudio ? m_windowsAudio->getSinkVolume(sinkName) : -1;
}

bool MediaController::setSinkVolume(const QString &sinkName, int volumePercent)
{
  return m_windowsAudio ? m_windowsAudio->setSinkVolume(sinkName, volumePercent) : false;
}

QString MediaController::getCardNameForDevice(const QString &macAddress)
{
  return m_windowsAudio ? m_windowsAudio->getCardNameForDevice(macAddress) : QString();
}

bool MediaController::setCardProfile(const QString &cardName, const QString &profileName)
{
  return m_windowsAudio ? m_windowsAudio->setCardProfile(cardName, profileName) : true;
}
