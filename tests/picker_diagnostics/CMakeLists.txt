cmake_minimum_required(VERSION 3.13)
project(PickerDiagnosticsTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DIAG_GUI "${CMAKE_CURRENT_LIST_DIR}/../../src/slic3r/GUI")
find_package(Threads REQUIRED)
add_executable(picker_diagnostics_test runtime_test.cpp "${DIAG_GUI}/PickerDiagnostics.cpp")
target_include_directories(picker_diagnostics_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/mock" "${DIAG_GUI}")
target_link_libraries(picker_diagnostics_test PRIVATE Threads::Threads)
if(WIN32)
    target_compile_definitions(picker_diagnostics_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(picker_diagnostics_test PRIVATE Psapi)
endif()
enable_testing()
add_test(NAME picker_diagnostics_enabled COMMAND "${CMAKE_COMMAND}" -E env
    "ORCA_PICKER_DIAG=1" "ORCA_PICKER_DIAG_GPU=1" "ORCA_PICKER_DIAG_DIR=${CMAKE_CURRENT_BINARY_DIR}/logs-on"
    $<TARGET_FILE:picker_diagnostics_test>)
add_test(NAME picker_diagnostics_disabled COMMAND "${CMAKE_COMMAND}" -E env
    "ORCA_PICKER_DIAG=0" "ORCA_PICKER_DIAG_DIR=${CMAKE_CURRENT_BINARY_DIR}/logs-off"
    $<TARGET_FILE:picker_diagnostics_test> disabled)
