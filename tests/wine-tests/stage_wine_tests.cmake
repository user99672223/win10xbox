# Stages the Wine conformance guests into the machine folder (cmake -P).
#   -DMACHINE=<machine dir> -DGUEST_DIR=<bin/guest-wine> -DRESULTS_DIR=<dir>
# Copies GUEST_DIR/*.exe to <MACHINE>/guest-wine (guest path C:\guest-wine\) and
# empties RESULTS_DIR so the report only shows this run. Having no guests is not
# an error: the tests then SKIP (run_wine_test.cmake).
if(NOT MACHINE)
  message(FATAL_ERROR "stage_wine_tests: MACHINE not set")
endif()
file(MAKE_DIRECTORY "${MACHINE}/guest-wine")
if(RESULTS_DIR)
  file(REMOVE_RECURSE "${RESULTS_DIR}")
  file(MAKE_DIRECTORY "${RESULTS_DIR}")
endif()
set(_n 0)
if(GUEST_DIR AND IS_DIRECTORY "${GUEST_DIR}")
  file(GLOB _files "${GUEST_DIR}/*.exe")
  foreach(f ${_files})
    get_filename_component(name "${f}" NAME)
    file(COPY_FILE "${f}" "${MACHINE}/guest-wine/${name}" ONLY_IF_DIFFERENT)
    math(EXPR _n "${_n} + 1")
  endforeach()
endif()
message(STATUS "stage_wine_tests: ${_n} Wine test guests -> ${MACHINE}/guest-wine")
