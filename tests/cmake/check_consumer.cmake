execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}/tests/cmake/consumer" -B "${BINARY}"
  -G "${GENERATOR}" "-DCMAKE_CXX_COMPILER=${COMPILER}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
  "-DTIEREDCACHE_SOURCE_DIR=${SOURCE}" "-DFETCHCONTENT_SOURCE_DIR_SUB0MEMPAGE=${MEMPAGE}"
  -DFETCHCONTENT_FULLY_DISCONNECTED=ON "-DPROVIDE_MEMPAGE=${PROVIDE}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Consumer configure failed: ${output} ${errors}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY}" --config "${CONFIG}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Consumer build failed: ${output} ${errors}")
endif()
