#pragma once
#include <string>

struct GLFWwindow;

namespace platform {

void setMainWindow(GLFWwindow* window);

// Opens a native file dialog for .cir files.
// Returns selected path, or empty string if cancelled.
std::string openFileDialog();

}  // namespace platform
