# Building Librepods-WindowsFix

These are the exact, tested steps to build the Windows client from a clean machine.
Everything runs in **PowerShell**.

## 1. Prerequisites

Install these first:

- **Visual Studio 2022** (Community is fine) with the **"Desktop development with C++"**
  workload — this provides the MSVC v143 compiler and the Windows SDK.
  (Build Tools only, without the full IDE, also works.)
- **Python 3.9+** — used to fetch Qt via `aqtinstall`. (`python --version` to check.)
- **CMake 3.16+**. If you don't have it:
  ```powershell
  winget install Kitware.CMake --accept-package-agreements --accept-source-agreements
  ```
  Restart your shell afterward so `cmake` is on `PATH` (or call it via
  `"C:\Program Files\CMake\bin\cmake.exe"`).

## 2. Install Qt 6 (via aqtinstall)

We use prebuilt Qt binaries — no Qt account or GUI needed.

```powershell
pip install aqtinstall

# Qt 6.8.3 for MSVC x64, plus the Bluetooth (qtconnectivity) and Multimedia modules
python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 `
  -m qtconnectivity qtmultimedia --outputdir C:\Qt
```

This installs Qt to `C:\Qt\6.8.3\msvc2022_64`.

## 3. Install OpenSSL 3.x

Use the OpenSSL build that matches Qt (also via aqt):

```powershell
python -m aqt install-tool windows desktop tools_opensslv3_x64 --outputdir C:\Qt
```

This installs OpenSSL to `C:\Qt\Tools\OpenSSLv3\Win_x64`
(headers in `include\`, libs in `lib\`, DLLs in `bin\`).

## 4. Configure

From the repository root:

```powershell
cd windows
& "C:\Program Files\CMake\bin\cmake.exe" -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" `
  -DOPENSSL_ROOT_DIR="C:/Qt/Tools/OpenSSLv3/Win_x64"
```

You should see `Found OpenSSL ... (found version "3.x")` and a successful configure.

## 5. Build

```powershell
& "C:\Program Files\CMake\bin\cmake.exe" --build build --config Release
```

The executable lands at `windows\build\Release\librepods-windows.exe`.

## 6. Deploy the runtime DLLs

The exe needs Qt + OpenSSL DLLs beside it to run as a standalone app:

```powershell
# Copy the Qt runtime + QML next to the exe
& "C:\Qt\6.8.3\msvc2022_64\bin\windeployqt.exe" --release --qmldir . build\Release\librepods-windows.exe

# Copy the two OpenSSL runtime DLLs
Copy-Item "C:\Qt\Tools\OpenSSLv3\Win_x64\bin\libssl-3-x64.dll","C:\Qt\Tools\OpenSSLv3\Win_x64\bin\libcrypto-3-x64.dll" `
  -Destination build\Release
```

`build\Release\` is now a self-contained, runnable folder. (For packaging, drop the
build-only `librepods-windows.exp` and `librepods-windows.lib` files.)

## 7. Run

The app talks to the AirPods over L2CAP through a kernel driver, so before it can connect:

1. Enable Windows **Test Mode** (`bcdedit /set testsigning on`, reboot — Secure Boot
   must be off).
2. Install the [l2cap-windowsdriver](https://github.com/will-ch-h/l2cap-windowsdriver)
   driver: the running installer (see `installer.iss`) does this automatically, but when
   running the raw exe from a source build, install it yourself — grab the signed
   `driver-latest` release from that repo and run:
   ```powershell
   certutil -addstore Root l2cap-bridge.cer
   certutil -addstore TrustedPublisher l2cap-bridge.cer
   pnputil /add-driver BthEchoSampleCli.inf /install
   ```
3. Pair your AirPods in Windows Bluetooth settings.
4. Launch `librepods-windows.exe`. It lives in the **system tray** — left-click for
   battery, right-click for noise-control modes and settings.

Run with `--debug` for verbose logging:

```powershell
.\build\Release\librepods-windows.exe --debug
```

## Notes / troubleshooting

- **Different Qt version**: any Qt 6.5+ should work; adjust the paths in steps 2/4/6.
- **`qwindows.dll` / blank start**: make sure `windeployqt` ran — the `platforms\qwindows.dll`
  it copies is mandatory.
- **"cmake not found"** in Git Bash: call CMake by full path, or use PowerShell where
  `winget` put it on `PATH`.
- The first configure may warn about a QML resource-prefix policy; this is handled by
  `qt_standard_project_setup(REQUIRES 6.5)` in `CMakeLists.txt` and is safe to ignore.
