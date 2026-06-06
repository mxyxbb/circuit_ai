add_library(circuit_frontend STATIC
    view_model/scope_model.cpp
    view_model/schematic_model.cpp
    view_model/main_view_model.cpp
    views/main_view.cpp
    views/scope_view.cpp
    views/settings_view.cpp
    views/palette_view.cpp
    views/schematic_view.cpp
    views/schematic_svg_export.cpp
    views/schematic_clipboard_win32.cpp
    platform/file_dialog_win32.cpp
)
target_include_directories(circuit_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(circuit_frontend
    PUBLIC circuit_backend imgui implot glfw OpenGL::GL
    PRIVATE comdlg32
)
target_include_directories(circuit_frontend SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/external/imgui
    ${CMAKE_SOURCE_DIR}/external/implot
)
