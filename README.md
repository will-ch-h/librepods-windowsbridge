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
this fork uses the [MagicAAP driver](https://magicpods.app/magicaap/) to talk to the
AirPods at the kernel level. (own driver in the works)

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

1. Install the [**MagicAAP driver**](https://magicpods.app/magicaap/) in Windows **Test Mode**.

> **Do NOT install the community-signed driver.** It relies on a code-signing exploit
> (a leaked certificate) this opens a potential backdoor on your
> system.

2. Download the Setup.exe from releases.

4. Go through the setup process.

6. On first run it might be a little weird, click in and out of the window.

7. It is recomended to move the tray icon onto your taskbar but you don't have to :).


## Needed Improvements 
- After first install the window will not close until clicked into
- Sometimes airpods show up as disconnected and out of ear when connected
  > This happens when put back into and taken out of the case really quickly
- Installed size is a little heavy (100+mb), try and slim down

------------------------------------
### Notice of AI Use
Both upstream repos used generative AI at their own discretion. AI was used for the build instructions and to solve some bugs 
