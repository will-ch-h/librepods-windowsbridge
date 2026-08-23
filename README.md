# Librepods-WindowsBridge
This fork only contains files for a windows "bridge" of librepods

Thanks to:
- The Librepods project (https://github.com/kavishdevar/librepods)
- Tblob18 (https://github.com/Tblob18/librepods-windows)

This fork successfully allows for airpods to work on windows with the ability to change listening modes and more.

Recomended for advanced/technical users only. 
A custom driver is utilized that has to be used in Windows Test mode. 
This impacts the security of your system and should not be done lightly.
## How does it work?

LibrePods communicates with AirPods over the Bluetooth **L2CAP** protocol, but Windows
blocks user-mode L2CAP, which is why a normal build can't connect. To work around this,
this fork ships its own [l2cap-windowsdriver](https://github.com/will-ch-h/l2cap-windowsdriver)
(a retargeted fork of Microsoft's `bthecho` sample) to talk to the AirPods at the kernel
level. It's signed with the project's own test-signing certificate and installed
automatically by the setup below.

## Current Features 
- See Airpod Battery and Connection Status
- Display when Airpod(s) is/are out of ear
- Change between listening modes (ANC-Transparency-Adaptive)
- Conversation Awareness
- Pause currently playing audio when airpod is removed. (can be turned off)
- Notification fly up when airpods connected.

## Screenshots
<picture>
  <img alt="LibrePods" src="./imgs/windowshowcase.png" />
</picture>
<picture>
  <img alt="LibrePods" src="./imgs/notifbanner.png" />
</picture>

## Steps to install

> [!CAUTION]
> This project is still very much in beta, 
> don't expect a smooth experience.

1. Enable Windows **Test Mode** (Secure Boot must be off first):
   ```powershell
   bcdedit /set testsigning on
   ```
   Reboot. You should see a "Test Mode" watermark in the bottom-right of the desktop.

2. Download the Setup.exe from [releases](https://github.com/will-ch-h/librepods-windowsbridge/releases)
   and run it. It bundles and auto-installs the project's own signed
   [l2cap-windowsdriver](https://github.com/will-ch-h/l2cap-windowsdriver)

3. Turn Bluetooth Off and On

4. Pair your AirPods in Windows Bluetooth settings if you haven't already.

> [!WARNING]
> **What the installer changes on your system.** Two things worth understanding before
> you agree to them:
>
> - **Test Mode weakens driver security machine-wide.** With `testsigning on`, Windows will
>   load *any* test-signed kernel driver, not just this one. Turn it back off with
>   `bcdedit /set testsigning off` if you stop using this app.
> - **The installer adds this project's certificate to your Trusted Root store.** That means
>   anything signed with our key is trusted as code on your machine. The cert is restricted
>   to code signing only (it can't be used to intercept HTTPS), but if our signing key were
>   ever leaked or the repo compromised, that trust could be abused. Uninstalling removes
>   the cert and the driver again.
>
> This is inherent to shipping a self-signed kernel driver — there's no way around it short
> of a real (expensive, attested) Microsoft-cross-signed EV certificate. Install it only if
> you're comfortable with that tradeoff.

4. On first run it might be a little weird, click in and out of the window.

5. It is recommended to move the tray icon onto your taskbar but you don't have to :).


## Needed Improvements 
- Hearing Aid is not yet implemented 
- After first install the window will not close until clicked into
- Sometimes airpods show up as disconnected and out of ear when connected
  > This happens when put back into and taken out of the case really quickly
- Installed size is a little heavy (100+mb), try and slim down

------------------------------------
### Notice of AI Use
Both upstream repos used generative AI at their own discretion. AI was used for the build instructions and to solve some bugs 
