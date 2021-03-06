set(steamvr-dir "")
set(steamvr-dll "")
set(steamvr-lib "")

if(LINUX)
    if (opentrack-64bit)
        set(steamvr-dir "linux64")
    else()
        set(steamvr-dir "linux32")
    endif()
    set(steamvr-dll "libopenvr_api.so")
endif()

if(WIN32)
    if(opentrack-64bit)
        set(steamvr-dir "win64")
    else()
        set(steamvr-dir "win32")
    endif()
    set(steamvr-dll "openvr_api.dll")
    set(steamvr-lib "openvr_api.lib")
endif()

if(APPLE)
    # expect user compiled it as a non-framework version
    if(opentrack-64bit)
        set(steamvr-dir "osx64")
    else()
        set(steamvr-dir "osx32")
    endif()
    set(steamvr-dll "libopenvr_api.dylib")
endif()

if(steamvr-dll AND opentrack-intel)
    if(steamvr-lib STREQUAL "")
        set(steamvr-lib "${steamvr-dll}")
    endif()
    SET(SDK_VALVE_STEAMVR "" CACHE PATH "Valve's SteamVR")
    if(SDK_VALVE_STEAMVR)
        otr_module(tracker-steamvr)
        install(FILES "${SDK_VALVE_STEAMVR}/bin/${steamvr-dir}/${steamvr-dll}" DESTINATION "${opentrack-libexec}")

        target_include_directories(opentrack-tracker-steamvr SYSTEM PRIVATE "${SDK_VALVE_STEAMVR}/headers")
        target_link_libraries(opentrack-tracker-steamvr "${SDK_VALVE_STEAMVR}/lib/${steamvr-dir}/${steamvr-lib}")
    endif()
endif()
