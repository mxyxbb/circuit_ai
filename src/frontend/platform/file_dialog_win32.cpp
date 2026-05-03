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

const std::string& exeDir() {
    static const std::string dir = []() -> std::string {
        char buf[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (n == 0) return ".";
        std::string p(buf, n);
        auto pos = p.find_last_of("\\/");
        return (pos == std::string::npos) ? "." : p.substr(0, pos);
    }();
    return dir;
}

std::string appDataPath(const std::string& filename) {
    return exeDir() + "\\" + filename;
}

std::string openFileDialog() {
    char filename[MAX_PATH] = "";

    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_window ? glfwGetWin32Window(g_window) : nullptr;
    ofn.lpstrFilter = "Circuit Netlist (*.cir)\0*.cir\0All Files\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    // OFN_NOCHANGEDIR: prevents the dialog from changing the process CWD when
    // the user navigates to another folder — required so persistence files
    // (imgui.ini, session.txt, winstate.txt, autosave_*.sch) stay co-located
    // with the executable.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "cir";

    if (GetOpenFileNameA(&ofn)) {
        return std::string(filename);
    }
    return {};
}

}  // namespace platform
