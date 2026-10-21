# Prints the Wine conformance results of the last run as a Markdown table
# (cmake -P). -DRESULTS_DIR=<dir with <name>.tsv written by run_wine_test.cmake>
file(GLOB _files "${RESULTS_DIR}/*.tsv")
list(SORT _files)
if(NOT _files)
  message("wine conformance report: no results in ${RESULTS_DIR} (tests skipped or not run)")
  return()
endif()
set(_out "| test | outcome | executed | todo | failures | skipped | exit | secs | last test line seen |\n|---|---|---:|---:|---:|---:|---:|---:|---|\n")
foreach(f ${_files})
  file(READ "${f}" row)
  string(STRIP "${row}" row)
  set(_c "([^\t]*)")   # CMake has only 9 capture groups: the flaky column is not captured
  if(NOT row MATCHES "^${_c}\t${_c}\t${_c}\t${_c}\t[^\t]*\t${_c}\t${_c}\t${_c}\t${_c}\t?(.*)$")
    continue()
  endif()
  string(APPEND _out "| ${CMAKE_MATCH_1} | ${CMAKE_MATCH_2} | ${CMAKE_MATCH_3} | ${CMAKE_MATCH_4} | ${CMAKE_MATCH_5} | ${CMAKE_MATCH_6} | ${CMAKE_MATCH_7} | ${CMAKE_MATCH_8} | ${CMAKE_MATCH_9} |\n")
endforeach()
message("${_out}")
