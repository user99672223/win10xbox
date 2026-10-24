# unit.xinput_exports: our xinput1_4.dll exports exactly the real DLL's names and ordinals (docs/slices/xinput2.md).
if(TARGET xinput1_4)
  add_test(NAME unit.xinput_exports
    COMMAND ${CMAKE_COMMAND} -DDLL=$<TARGET_FILE:xinput1_4> -DEXPECTED_DOC=${CMAKE_SOURCE_DIR}/docs/slices/xinput2.md
      -DOBJDUMP=${CMAKE_OBJDUMP} -DLINKER=${CMAKE_LINKER}
      -P ${CMAKE_CURRENT_LIST_DIR}/check_dll_exports.cmake)
  set_tests_properties(unit.xinput_exports PROPERTIES SKIP_REGULAR_EXPRESSION "SKIP:")
endif()
