cmake_minimum_required(VERSION 3.20)
# Release builds pass the version from the git tag (see .github/workflows/release.yml):
#   -DTCL_VERSION=1.2.3 [-DTCL_VERSION_LABEL=1.2.3-beta1]
set(TCL_VERSION "0.1.0" CACHE STRING "Numeric version (MAJOR.MINOR.PATCH)")
set(TCL_VERSION_LABEL "" CACHE STRING "Full version label, e.g. 1.2.3-beta1 (defaults to TCL_VERSION)")
project(tcl_csvvw VERSION ${TCL_VERSION} LANGUAGES CXX)
if(NOT TCL_VERSION_LABEL)
  set(TCL_VERSION_LABEL ${PROJECT_VERSION})
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT for MSVC: no VC++ redistributable needed on the user's machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32)
  set(_plugin_default ON)
else()
  set(_plugin_default OFF)
endif()
option(TCL_BUILD_PLUGIN "Build the Total Commander plugin (.wlx64, Windows only)" ${_plugin_default})
option(TCL_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /Zc:__cplusplus /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------- core (portable)
add_library(csv_core STATIC
  src/core/TextUtil.cpp
  src/core/TextDecoder.cpp
  src/core/IniDocument.cpp
  src/core/Config.cpp
  src/core/OptionSchema.cpp
  src/core/Settings.cpp
  src/csv/Parser.cpp
  src/csv/ValueParse.cpp
  src/csv/Table.cpp
  src/csv/Ops.cpp
  src/csv/TextLayout.cpp
)
target_include_directories(csv_core PUBLIC src)

# ---------------------------------------------------------------- plugin (Windows)
if(TCL_BUILD_PLUGIN)
  if(NOT WIN32)
    message(FATAL_ERROR "TCL_BUILD_PLUGIN requires a Windows target (MSVC or MinGW cross toolchain)")
  endif()
  enable_language(RC)
  add_library(tcl-csvvw SHARED
    src/plugin/exports.cpp
    src/plugin/WinUtil.cpp
    src/plugin/DebugLog.cpp
    src/view/ListerWindow.cpp
    src/view/GridView.cpp
    src/view/StatusBar.cpp
    src/view/InfoBar.cpp
    src/ui/ConfigDialog.cpp
    src/ui/ConfigPages.cpp
    src/ui/DarkMode.cpp
    src/ui/resources.rc
    src/plugin/tcl-csvvw.def
  )
  target_link_libraries(tcl-csvvw PRIVATE csv_core user32 gdi32 comctl32 uxtheme shell32 comdlg32 dwmapi)
  configure_file(src/ui/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)
  target_include_directories(tcl-csvvw PRIVATE ${CMAKE_BINARY_DIR}/generated)
  target_compile_definitions(tcl-csvvw PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
  set_target_properties(tcl-csvvw PROPERTIES PREFIX "" SUFFIX ".wlx64")
  if(MINGW)
    target_link_options(tcl-csvvw PRIVATE -static -static-libgcc -static-libstdc++ $<$<CONFIG:Release>:-s>)
  endif()
  # Copy the INI next to the built DLL so it can be used/tested in place.
  add_custom_command(TARGET tcl-csvvw POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/dist $<TARGET_FILE_DIR:tcl-csvvw>)

  # Release layout: build/package/tcl-csvvw/* and build/tcl-csvvw-<ver>.zip
  set(_pkg_dir ${CMAKE_BINARY_DIR}/package/tcl-csvvw)
  set(_zip ${CMAKE_BINARY_DIR}/tcl-csvvw-${TCL_VERSION_LABEL}-x64.zip)
  add_custom_target(package_zip
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/package
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_pkg_dir}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:tcl-csvvw> ${_pkg_dir}/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/dist ${_pkg_dir}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/README.md ${_pkg_dir}/
    COMMAND ${CMAKE_COMMAND} -E chdir ${_pkg_dir} ${CMAKE_COMMAND} -E tar cf ${_zip} --format=zip .
    DEPENDS tcl-csvvw
    COMMENT "Packaging ${_zip}")
endif()

# ---------------------------------------------------------------- tests
if(TCL_BUILD_TESTS)
  enable_testing()
  add_executable(csv_tests
    tests/test_main.cpp
    tests/test_parser.cpp
    tests/test_values.cpp
    tests/test_table.cpp
    tests/test_ops.cpp
    tests/test_layout.cpp
    tests/test_config.cpp
    tests/test_samples.cpp
  )
  target_include_directories(csv_tests PRIVATE tests tests/third_party)
  target_link_libraries(csv_tests PRIVATE csv_core)
  target_compile_definitions(csv_tests PRIVATE CSVVW_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(MSVC)
    target_compile_options(csv_tests PRIVATE /wd5285)  # third-party doctest.h
  endif()
  if(MINGW)
    target_link_options(csv_tests PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  add_test(NAME csv_tests COMMAND csv_tests)
endif()

# ---------------------------------------------------------------- end-to-end smoke test (Windows)
if(TCL_BUILD_PLUGIN AND TCL_BUILD_TESTS)
  add_executable(plugin_smoke tools/plugin_smoke.cpp)
  target_link_libraries(plugin_smoke PRIVATE csv_core user32 gdi32)
  target_compile_definitions(plugin_smoke PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  if(MINGW)
    target_link_options(plugin_smoke PRIVATE -municode -static -static-libgcc -static-libstdc++)
  endif()
  add_dependencies(plugin_smoke tcl-csvvw)
  add_test(NAME plugin_smoke
    COMMAND plugin_smoke $<TARGET_FILE:tcl-csvvw> ${CMAKE_SOURCE_DIR}/tests/samples)
endif()
