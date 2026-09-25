include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)

# Fetch dependencies - defensive (FetchAndPopulate is idempotent, a no-op if the host project already
# fetched these, e.g. xLION's own top-level CMakeLists.txt already does for both).
FetchAndPopulate("https://github.com/LIONant-depot/xeditor.git" "main")
FetchAndPopulate("https://github.com/LIONant-depot/xGPU.git"    "main")

DefineInterfaceComponent(xeditor_tools "dependencies/xcore"
  "src/xeditor_tools.cpp"
  "src/xeditor_tools_camera.h"
  "src/xeditor_tools_grid.h"
)
