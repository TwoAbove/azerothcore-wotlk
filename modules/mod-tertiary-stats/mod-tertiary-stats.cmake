# Test suites are registered directly by this product module, so both modules
# must resolve to the static script loader when tertiary stats is enabled.
set(TERTIARY_LINKAGE "${MODULE_MOD-TERTIARY-STATS}")
set(TEST_HARNESS_LINKAGE "${MODULE_MOD-TEST-HARNESS}")
if(TERTIARY_LINKAGE STREQUAL "default")
  set(TERTIARY_LINKAGE "${MODULES_DEFAULT_LINKAGE}")
endif()
if(TEST_HARNESS_LINKAGE STREQUAL "default")
  set(TEST_HARNESS_LINKAGE "${MODULES_DEFAULT_LINKAGE}")
endif()

if(NOT TERTIARY_LINKAGE STREQUAL "disabled")
  if(NOT TERTIARY_LINKAGE STREQUAL "static"
      OR NOT TEST_HARNESS_LINKAGE STREQUAL "static")
    message(FATAL_ERROR
      "mod-tertiary-stats requires mod-test-harness and static linkage for both modules")
  endif()
endif()
