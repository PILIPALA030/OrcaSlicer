# Temporary diagnostic branch: preserve the original build description verbatim.
# The included file lives in the same directory, so relative paths and directory
# scope remain identical to the feature_picker_rebuild baseline.
include("${CMAKE_CURRENT_LIST_DIR}/CMakeLists.picker-base.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/GUI/PickerDiagnostics.cmake")
