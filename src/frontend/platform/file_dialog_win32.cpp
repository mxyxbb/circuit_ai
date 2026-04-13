#include "platform/file_dialog.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <windows.h>
#include <commdlg.h>

namespace platform {

static GLFWwindow* g_window = nullptr;

void setMainWindow(GLFWwindow* window) {
    g_window = window;
}

std::string openFileDialog() {
    char filename[MAX_PATH] = "";

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_window ? glfwGetWin32Window(g_window) : nullptr;
    ofn.lpstrFilter = "Circuit Netlist (*.cir)\0*.cir\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = "cir";

    if (GetOpenFileNameA(&ofn)) {
        return std::string(filename);
    }
    return {};
}

}  // namespace platform
