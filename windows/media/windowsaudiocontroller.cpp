#include "windowsaudiocontroller.h"
#include "../logger.h"

#ifdef Q_OS_WIN
#include <comdef.h>
#include <Mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Propvarutil.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <thread>

// IPolicyConfig is an undocumented COM interface used to set the default audio
// endpoint (the documented MMDevice API can read the default but not change it).
// Only SetDefaultEndpoint is needed here; other vtable slots are stubbed so the
// layout matches.
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig;
class DECLSPEC_UUID("870af99c-171d-4f9e-af0d-e63df40c2bc9") CPolicyConfigClient;

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, void *, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace
{
// The Qt UI thread runs in an STA, where blocking on a WinRT async operation
// with .get() can deadlock. Run the WinRT work on a short-lived MTA thread so
// the blocking wait is safe. These calls are infrequent (only on ear-detection
// events), so the cost of spinning up a thread is negligible.
template <typename Fn>
auto runOnMtaThread(Fn fn) -> decltype(fn())
{
    using Ret = decltype(fn());
    Ret result{};
    std::thread t([&]()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try
        {
            result = fn();
        }
        catch (...)
        {
        }
        winrt::uninit_apartment();
    });
    t.join();
    return result;
}
} // namespace
#endif

WindowsAudioController::WindowsAudioController(QObject *parent)
    : QObject(parent)
#ifdef Q_OS_WIN
    , m_deviceEnumerator(nullptr)
    , m_endpointVolume(nullptr)
#endif
    , m_initialized(false)
{
}

WindowsAudioController::~WindowsAudioController()
{
#ifdef Q_OS_WIN
    if (m_endpointVolume)
    {
        m_endpointVolume->Release();
        m_endpointVolume = nullptr;
    }
    if (m_deviceEnumerator)
    {
        m_deviceEnumerator->Release();
        m_deviceEnumerator = nullptr;
    }
    CoUninitialize();
#endif
}

bool WindowsAudioController::initialize()
{
#ifdef Q_OS_WIN
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        LOG_ERROR("Failed to initialize COM");
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&m_deviceEnumerator);
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to create device enumerator");
        CoUninitialize();
        return false;
    }

    m_initialized = true;
    LOG_INFO("Windows Audio Controller initialized successfully");
    return true;
#else
    LOG_ERROR("Windows Audio Controller can only be used on Windows");
    return false;
#endif
}

QString WindowsAudioController::getDefaultSink()
{
#ifdef Q_OS_WIN
    if (!m_initialized || !m_deviceEnumerator)
        return QString();

    IMMDevice *defaultDevice = nullptr;
    HRESULT hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, &defaultDevice);
    
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to get default audio endpoint");
        return QString();
    }

    LPWSTR deviceId = nullptr;
    hr = defaultDevice->GetId(&deviceId);
    QString result;
    if (SUCCEEDED(hr))
    {
        result = QString::fromWCharArray(deviceId);
        CoTaskMemFree(deviceId);
    }

    defaultDevice->Release();
    return result;
#else
    return QString();
#endif
}

int WindowsAudioController::getSinkVolume(const QString &sinkName)
{
#ifdef Q_OS_WIN
    if (!m_initialized || !m_deviceEnumerator)
        return -1;

    IMMDevice *device = nullptr;
    std::wstring deviceIdW = sinkName.toStdWString();
    HRESULT hr = m_deviceEnumerator->GetDevice(deviceIdW.c_str(), &device);
    
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to get audio device");
        return -1;
    }

    IAudioEndpointVolume *endpointVolume = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                          nullptr, (void**)&endpointVolume);
    
    if (FAILED(hr))
    {
        device->Release();
        LOG_ERROR("Failed to activate endpoint volume");
        return -1;
    }

    float volumeLevel = 0.0f;
    hr = endpointVolume->GetMasterVolumeLevelScalar(&volumeLevel);
    
    int volumePercent = -1;
    if (SUCCEEDED(hr))
    {
        volumePercent = static_cast<int>(volumeLevel * 100.0f);
    }

    endpointVolume->Release();
    device->Release();
    
    return volumePercent;
#else
    Q_UNUSED(sinkName);
    return -1;
#endif
}

bool WindowsAudioController::setSinkVolume(const QString &sinkName, int volumePercent)
{
#ifdef Q_OS_WIN
    if (!m_initialized || !m_deviceEnumerator)
        return false;

    if (volumePercent < 0 || volumePercent > 100)
        return false;

    IMMDevice *device = nullptr;
    std::wstring deviceIdW = sinkName.toStdWString();
    HRESULT hr = m_deviceEnumerator->GetDevice(deviceIdW.c_str(), &device);
    
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to get audio device");
        return false;
    }

    IAudioEndpointVolume *endpointVolume = nullptr;
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                          nullptr, (void**)&endpointVolume);
    
    if (FAILED(hr))
    {
        device->Release();
        LOG_ERROR("Failed to activate endpoint volume");
        return false;
    }

    float volumeLevel = static_cast<float>(volumePercent) / 100.0f;
    hr = endpointVolume->SetMasterVolumeLevelScalar(volumeLevel, nullptr);
    
    bool success = SUCCEEDED(hr);
    if (!success)
    {
        LOG_ERROR("Failed to set volume");
    }

    endpointVolume->Release();
    device->Release();
    
    return success;
#else
    Q_UNUSED(sinkName);
    Q_UNUSED(volumePercent);
    return false;
#endif
}

bool WindowsAudioController::setCardProfile(const QString &cardName, const QString &profileName)
{
    // Windows handles audio profiles differently than Linux
    // This is a placeholder for potential future implementation
    Q_UNUSED(cardName);
    Q_UNUSED(profileName);
    LOG_DEBUG("setCardProfile not implemented on Windows");
    return true;
}

QString WindowsAudioController::getCardNameForDevice(const QString &macAddress)
{
#ifdef Q_OS_WIN
    if (!m_initialized || !m_deviceEnumerator)
        return QString();

    IMMDeviceCollection *deviceCollection = nullptr;
    HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(
        eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    
    if (FAILED(hr))
    {
        LOG_ERROR("Failed to enumerate audio endpoints");
        return QString();
    }

    UINT count = 0;
    deviceCollection->GetCount(&count);

    QString result;
    for (UINT i = 0; i < count; i++)
    {
        IMMDevice *device = nullptr;
        hr = deviceCollection->Item(i, &device);
        if (FAILED(hr))
            continue;

        QString friendlyName = getDeviceFriendlyName(device);
        
        // Check if device name contains MAC address or AirPods identifier
        if (friendlyName.contains(macAddress, Qt::CaseInsensitive) ||
            friendlyName.contains("AirPods", Qt::CaseInsensitive))
        {
            LPWSTR deviceId = nullptr;
            if (SUCCEEDED(device->GetId(&deviceId)))
            {
                result = QString::fromWCharArray(deviceId);
                CoTaskMemFree(deviceId);
                device->Release();
                break;
            }
        }
        
        device->Release();
    }

    deviceCollection->Release();
    return result;
#else
    Q_UNUSED(macAddress);
    return QString();
#endif
}

QString WindowsAudioController::getDeviceFriendlyName(void *devicePtr)
{
#ifdef Q_OS_WIN
    IMMDevice *device = static_cast<IMMDevice*>(devicePtr);
    if (!device)
        return QString();

    IPropertyStore *propertyStore = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
    if (FAILED(hr))
        return QString();

    PROPVARIANT friendlyName;
    PropVariantInit(&friendlyName);
    
    hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName);
    QString name;
    if (SUCCEEDED(hr) && friendlyName.vt == VT_LPWSTR)
    {
        name = QString::fromWCharArray(friendlyName.pwszVal);
    }

    PropVariantClear(&friendlyName);
    propertyStore->Release();
    
    return name;
#else
    Q_UNUSED(devicePtr);
    return QString();
#endif
}

bool WindowsAudioController::setDefaultAudioDevice(const QString &deviceId)
{
#ifdef Q_OS_WIN
    if (deviceId.isEmpty())
        return false;

    IPolicyConfig *policyConfig = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfig), (void **)&policyConfig);
    if (FAILED(hr) || !policyConfig)
    {
        LOG_ERROR("Failed to create IPolicyConfig");
        return false;
    }

    std::wstring idW = deviceId.toStdWString();
    // Set the device as default for all three roles (media, console, comms).
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications})
    {
        HRESULT r = policyConfig->SetDefaultEndpoint(idW.c_str(), role);
        if (FAILED(r))
            ok = false;
    }

    policyConfig->Release();
    return ok;
#else
    Q_UNUSED(deviceId);
    return false;
#endif
}

bool WindowsAudioController::makeAirPodsDefaultOutput(const QString &macAddress)
{
#ifdef Q_OS_WIN
    if (!m_initialized || !m_deviceEnumerator)
        return false;

    IMMDeviceCollection *deviceCollection = nullptr;
    HRESULT hr = m_deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    if (FAILED(hr) || !deviceCollection)
        return false;

    UINT count = 0;
    deviceCollection->GetCount(&count);

    QString stereoId;   // preferred: an AirPods endpoint that isn't hands-free
    QString fallbackId; // any AirPods endpoint
    for (UINT i = 0; i < count; ++i)
    {
        IMMDevice *device = nullptr;
        if (FAILED(deviceCollection->Item(i, &device)) || !device)
            continue;

        const QString name = getDeviceFriendlyName(device);
        if (name.contains("AirPods", Qt::CaseInsensitive) ||
            (!macAddress.isEmpty() && name.contains(macAddress, Qt::CaseInsensitive)))
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)))
            {
                const QString idStr = QString::fromWCharArray(id);
                CoTaskMemFree(id);
                if (fallbackId.isEmpty())
                    fallbackId = idStr;
                // Prefer the high-quality stereo endpoint over the hands-free one.
                if (stereoId.isEmpty() &&
                    !name.contains("Hands-Free", Qt::CaseInsensitive) &&
                    !name.contains("Hands Free", Qt::CaseInsensitive))
                {
                    stereoId = idStr;
                }
            }
        }
        device->Release();
    }
    deviceCollection->Release();

    const QString chosen = !stereoId.isEmpty() ? stereoId : fallbackId;
    if (chosen.isEmpty())
        return false; // endpoint not enumerated yet

    return setDefaultAudioDevice(chosen);
#else
    Q_UNUSED(macAddress);
    return false;
#endif
}

int WindowsAudioController::getMediaPlaybackStatus()
{
#ifdef Q_OS_WIN
    return runOnMtaThread([]() -> int
    {
        try
        {
            auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            if (!session)
            {
                return -1;
            }

            auto status = session.GetPlaybackInfo().PlaybackStatus();
            using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;
            if (status == Status::Playing)
            {
                return 0;
            }
            if (status == Status::Paused)
            {
                return 1;
            }
            return 2;
        }
        catch (...)
        {
            return -1;
        }
    });
#else
    return -1;
#endif
}

bool WindowsAudioController::pauseMedia()
{
#ifdef Q_OS_WIN
    return runOnMtaThread([]() -> bool
    {
        try
        {
            auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            if (!session)
            {
                return false;
            }
            return session.TryPauseAsync().get();
        }
        catch (...)
        {
            return false;
        }
    });
#else
    return false;
#endif
}

bool WindowsAudioController::playMedia()
{
#ifdef Q_OS_WIN
    return runOnMtaThread([]() -> bool
    {
        try
        {
            auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto session = manager.GetCurrentSession();
            if (!session)
            {
                return false;
            }
            return session.TryPlayAsync().get();
        }
        catch (...)
        {
            return false;
        }
    });
#else
    return false;
#endif
}

bool WindowsAudioController::isDefaultOutputAirPods()
{
#ifdef Q_OS_WIN
    if (!m_initialized || !m_deviceEnumerator)
        return false;

    IMMDevice *defaultDevice = nullptr;
    HRESULT hr = m_deviceEnumerator->GetDefaultAudioEndpoint(
        eRender, eConsole, &defaultDevice);
    if (FAILED(hr) || !defaultDevice)
        return false;

    QString friendlyName = getDeviceFriendlyName(defaultDevice);
    defaultDevice->Release();

    return friendlyName.contains("AirPods", Qt::CaseInsensitive);
#else
    return false;
#endif
}
