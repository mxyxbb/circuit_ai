#pragma once
#include <string>

struct GLFWwindow;

namespace platform {

void setMainWindow(GLFWwindow* window);

// Opens a native file dialog for .cir files.
// Returns selected path, or empty string if cancelled.
std::string openFileDialog();

// Returns the absolute directory of the running executable (no trailing slash).
// On non-Windows builds returns "." as a fallback.
const std::string& exeDir();

// Returns absolute path to a persistence file located beside the executable.
// Use this for all session/auto-save artifacts so they remain co-located with
// the exe regardless of the current working directory.
std::string appDataPath(const std::string& filename);

}  // namespace platform
