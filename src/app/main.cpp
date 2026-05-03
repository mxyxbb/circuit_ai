#include "views/main_view.h"
#include "view_model/main_view_model.h"
#include "platform/file_dialog.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include <fstream>
#include <string>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

// ── Window geometry persistence ─────────────────────────────────────────────
// Resolve via platform::appDataPath so the file follows the exe regardless of
// CWD changes triggered by native file dialogs.
static std::string kWinGeoFile() { return platform::appDataPath("wingeo.txt"); }

static void saveWindowGeo(GLFWwindow* w) {
    int width, height, x, y;
    glfwGetWindowSize(w, &width, &height);
    glfwGetWindowPos(w, &x, &y);
    std::ofstream f(kWinGeoFile());
    if (f) {
        f << "W=" << width  << '\n';
        f << "H=" << height << '\n';
        f << "X=" << x      << '\n';
        f << "Y=" << y      << '\n';
    }
}

static void loadWindowGeo(int& outW, int& outH, int& outX, int& outY) {
    outW = 1600; outH = 900; outX = -1; outY = -1;
    std::ifstream f(kWinGeoFile());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.size() < 3 || line[1] != '=') continue;
        try {
            int v = std::stoi(line.substr(2));
            switch (line[0]) {
                case 'W': outW = v; break;
                case 'H': outH = v; break;
                case 'X': outX = v; break;
                case 'Y': outY = v; break;
            }
        } catch (...) {}
    }
    // Sanity-clamp stored size
    if (outW < 400)  outW = 1600;
    if (outH < 300)  outH = 900;
}

int main() {
#ifdef _WIN32
    // Pin CWD to the exe directory so imgui.ini and all session files
    // (session.txt, winstate.txt, autosave.sch) always land in the same
    // place regardless of how the exe is launched (IDE vs double-click).
    {
        char exePath[MAX_PATH];
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        char* sep = strrchr(exePath, '\\');
        if (sep) { *sep = '\0'; SetCurrentDirectoryA(exePath); }
    }
#endif

    // GLFW init
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    // Restore previous window geometry (size + position)
    int winW, winH, winX, winY;
    loadWindowGeo(winW, winH, winX, winY);

    GLFWwindow* window = glfwCreateWindow(winW, winH, "CircuitAI", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    if (winX >= 0 && winY >= 0)
        glfwSetWindowPos(window, winX, winY);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Register window handle for native dialogs
    platform::setMainWindow(window);

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    // Pin imgui.ini to the exe directory. ImGui caches the pointer internally,
    // so keep the string alive for the lifetime of the context.
    static const std::string s_iniPath = platform::appDataPath("imgui.ini");
    io.IniFilename = s_iniPath.c_str();

    ImPlot::CreateContext();

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    // App objects
    MainViewModel viewModel;
    MainView mainView;

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        viewModel.update();
        mainView.render(viewModel);

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Multi-viewport: render any windows dragged outside the main window
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            GLFWwindow* backupCtx = glfwGetCurrentContext();
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
            glfwMakeContextCurrent(backupCtx);
        }

        glfwSwapBuffers(window);
    }

    // Stop any in-progress background build before saving, so that the scope
    // model is fully populated (autoPopulateScope complete) when we write the
    // session state.  The build thread will be joined by the destructor anyway,
    // but we need it done *before* performAutoSave, not after.
    viewModel.stopBuildAndWait();

    // Persist window geometry so the next launch restores the same size/position.
    saveWindowGeo(window);

    // Auto-save session state before destroying the GUI context
    mainView.performAutoSave(viewModel);

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
