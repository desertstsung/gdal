add_gdal_driver(
  TARGET gdal_DTED
  SOURCES dted_api.h
          dted_api.c
          dted_create.c
          dted_ptstream.c
          dteddataset.cpp
  PLUGIN_CAPABLE
  NO_DEPS
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(gdal_DTED)

if (NOT GDAL_ENABLE_DRIVER_DTED_PLUGIN)
  add_executable(dted_test EXCLUDE_FROM_ALL dted_test.c)
  target_include_directories(dted_test PRIVATE $<TARGET_PROPERTY:gdal_DTED,SOURCE_DIR>)
  target_link_libraries(dted_test PRIVATE $<TARGET_NAME:${GDAL_LIB_TARGET_NAME}> gdal_DTED)
endif ()
