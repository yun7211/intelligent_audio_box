# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Force UTF-8 for Python tools so Windows locale settings do not break kconfig file parsing.
set(ENV{PYTHONUTF8} 1)
set(ENV{PYTHONIOENCODING} "utf-8")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.2.6")
project(YUN)
