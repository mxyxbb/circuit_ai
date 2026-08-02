add_executable(circuit_ai main.cpp)
target_link_libraries(circuit_ai PRIVATE circuit_frontend)

# Release builds: hide the console window (GUI subsystem). /ENTRY keeps the
# plain main() entry point so no WinMain is needed. Debug keeps the console
# for printf/log diagnostics.
if (WIN32)
    if (MSVC)
        target_link_options(circuit_ai PRIVATE
            $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup>)
    else()
        target_link_options(circuit_ai PRIVATE $<$<CONFIG:Release>:-mwindows>)
    endif()
endif()
