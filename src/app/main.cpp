#include "views/main_view.h"
#include "view_model/main_view_model.h"
#include "engine/diag.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include <GLFW/glfw3.h>
#include <cstdio>

// Diagnostic: run sim synchronously and dump CSV before starting GUI
static void runDiag() {
    // Try relative and absolute paths
    const char* rcPaths[] = {
        "rc_filter.cir",
        "netlists/rc_filter.cir",
        "../netlists/rc_filter.cir",
        "../../netlists/rc_filter.cir",
        nullptr
    };
    const char* rcPath = nullptr;
    for (auto p : rcPaths) {
        if (p && fopen(p, "r")) { rcPath = p; break; }
    }
    if (rcPath) {
        diagRunAndDump(rcPath, "diag_rc.csv", 200);
    } else {
        fprintf(stderr, "Cannot find rc_filter.cir\n");
    }

    const char* rlPaths[] = {
        "rl_circuit.cir",
        "netlists/rl_circuit.cir",
        "../netlists/rl_circuit.cir",
        "../../netlists/rl_circuit.cir",
        nullptr
    };
    const char* rlPath = nullptr;
    for (auto p : rlPaths) {
        if (p && fopen(p, "r")) { rlPath = p; break; }
    }
    if (rlPath) {
        diagRunAndDump(rlPath, "diag_rl.csv", 2000);
    } else {
        fprintf(stderr, "Cannot find rl_circuit.cir\n");
    }
}

int main() {
    // Run diagnostic simulation dump before GUI
    runDiag();

    // GLFW init
    if (!glfwInit()) return 1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1600, 900, "CircuitAI", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // ImGui init
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImPlot::CreateContext();

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

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
        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
