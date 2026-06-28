cmake_minimum_required(VERSION 3.16)

project(librepods-windows VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Qt6 REQUIRED COMPONENTS Quick Widgets Bluetooth Multimedia QuickControls2)
find_package(OpenSSL REQUIRED)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(librepods-windows
    main.cpp
    logger.h
    media/mediacontroller.cpp
    media/mediacontroller.h
    media/windowsaudiocontroller.cpp
    media/windowsaudiocontroller.h
    media/playerstatuswatcher.cpp
    media/playerstatuswatcher.h
    airpods_packets.h
    trayiconmanager.cpp
    trayiconmanager.h
    enums.h
    battery.hpp
    autostartmanager.hpp
    BasicControlCommand.hpp
    deviceinfo.hpp
    ble/bleutils.cpp
    ble/bleutils.h
    ble/blemanager.cpp
    ble/blemanager.h
    thirdparty/QR-Code-generator/qrcodegen.cpp
    thirdparty/QR-Code-generator/qrcodegen.hpp
    QRCodeImageProvider.hpp
    eardetection.hpp
    windowssleepmonitor.hpp
    windowssleepmonitor.cpp
    winl2capsocket.h
    winl2capsocket.cpp
)

qt_add_qml_module(librepods-windows
    URI windows
    VERSION 1.0
    QML_FILES
        Main.qml
        BatteryIndicator.qml
        SegmentedControl.qml
        PodColumn.qml
        Icon.qml
        KeysQRDialog.qml
        AirPodsConnectPopup.qml
)

# Add the resource file
qt_add_resources(librepods-windows "resources"
    PREFIX "/icons"
    FILES
        assets/airpods.png
        assets/pod.png
        assets/pod_case.png
        assets/pod3.png
        assets/pod3_case.png
        assets/pod4_case.png
        assets/podpro.png
        assets/podpro_case.png
        assets/podmax.png
        assets/fonts/SF-Symbols-6.ttf
)

# Link against required libraries
target_link_libraries(librepods-windows
    PRIVATE 
    Qt6::Quick
    Qt6::Widgets
    Qt6::Bluetooth
    Qt6::Multimedia
    Qt6::QuickControls2
    OpenSSL::SSL 
    OpenSSL::Crypto
    # Windows-specific libraries
    Ole32
    User32
    PowrProf
    SetupAPI
    # C++/WinRT runtime (System Media Transport Controls for media play/pause)
    WindowsApp
)

# Set Windows subsystem to console for debugging, change to WINDOWS for release
set_target_properties(librepods-windows PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# Installation
include(GNUInstallDirs)
install(TARGETS librepods-windows
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ---------------------------------------------------------------------------
# Distribution: `cmake --build build --config Release --target deploy`
# Produces a self-contained, runnable folder in build/dist (exe + Qt runtime +
# QML modules + plugins + OpenSSL). Note: end users still need the AAP L2CAP
# driver installed and test-signing enabled; this only bundles the app itself.
# ---------------------------------------------------------------------------
if(WIN32)
    # Locate windeployqt next to qmake in the Qt install.
    get_target_property(_qmake_exe Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")

    # Resolve the MSVC runtime DLLs (vcruntime140*, msvcp140) so the app runs on
    # machines without the VC++ redistributable installed. windeployqt's
    # --compiler-runtime doesn't reliably copy these.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)

    # OpenSSL runtime DLLs live in <openssl_root>/bin (the app links libcrypto
    # directly for AES, so they must ship even though windeployqt won't copy them).
    get_filename_component(_openssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    file(GLOB _openssl_dlls
        "${_openssl_root}/bin/libcrypto-*.dll"
        "${_openssl_root}/bin/libssl-*.dll")

    set(_deploy_dir "${CMAKE_BINARY_DIR}/dist")

    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_target(deploy
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${_deploy_dir}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_deploy_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:librepods-windows>" "${_deploy_dir}/"
            COMMAND "${WINDEPLOYQT_EXECUTABLE}" --release --compiler-runtime
                    --qmldir "${CMAKE_CURRENT_SOURCE_DIR}"
                    "${_deploy_dir}/librepods-windows.exe"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_openssl_dlls} "${_deploy_dir}/"
            DEPENDS librepods-windows
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Deploying a self-contained build to ${_deploy_dir}"
            VERBATIM
        )
        if(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            add_custom_command(TARGET deploy POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} "${_deploy_dir}/"
                VERBATIM)
        endif()
    else()
        message(WARNING "windeployqt not found; the 'deploy' target is unavailable.")
    endif()
endif()
