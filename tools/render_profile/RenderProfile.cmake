# Included after libslic3r_gui is created, before its PCH is configured.
option(SLIC3R_RENDER_PROFILE "Build opt-in nonblocking CPU/GPU rendering diagnostics" OFF)
if(NOT SLIC3R_RENDER_PROFILE)
    return()
endif()

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(_orp_tools "${CMAKE_CURRENT_LIST_DIR}")
set(_orp_gui "${CMAKE_CURRENT_SOURCE_DIR}/GUI")
set(_orp_output "${CMAKE_CURRENT_BINARY_DIR}/render-profile")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_orp_tools}/instrument.py"
            --gui-dir "${_orp_gui}" --output-dir "${_orp_output}"
    RESULT_VARIABLE _orp_result OUTPUT_VARIABLE _orp_stdout ERROR_VARIABLE _orp_stderr)
if(NOT _orp_result EQUAL 0)
    message(FATAL_ERROR "${_orp_stdout}\n${_orp_stderr}")
endif()
message(STATUS "${_orp_stdout}")

get_target_property(_orp_sources libslic3r_gui SOURCES)
foreach(_orp_name GLCanvas3D.cpp GLModel.cpp 3DScene.cpp GLShader.cpp)
    set(_orp_original "GUI/${_orp_name}")
    list(FIND _orp_sources "${_orp_original}" _orp_index)
    if(_orp_index EQUAL -1)
        message(FATAL_ERROR "Render profiling: target source missing: ${_orp_original}")
    endif()
    list(REMOVE_AT _orp_sources ${_orp_index})
    list(INSERT _orp_sources ${_orp_index} "${_orp_output}/${_orp_name}")
    set_source_files_properties("${_orp_output}/${_orp_name}" PROPERTIES
        INCLUDE_DIRECTORIES "${_orp_tools};${_orp_gui}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_orp_gui}/${_orp_name}")
endforeach()
set_property(TARGET libslic3r_gui PROPERTY SOURCES "${_orp_sources}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_orp_tools}/instrument.py" "${_orp_tools}/RenderProfile.hpp")
if(EXISTS "${_orp_gui}/ShadowMeshProxy.hpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_orp_gui}/ShadowMeshProxy.hpp")
endif()
# Instrumented copies are confined to the build directory. The ordinary source
# files and resource shaders stay untouched; OFF restores the original target.
