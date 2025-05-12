set(SOURCES
  Cdm.cpp
  DrmEngine.cpp
  DrmFactory.cpp
  Helpers.cpp
  HelperPr.cpp
  HelperWv.cpp
)

set(HEADERS
  Cdm.h
  DrmEngine.h
  DrmEngineDefines.h
  DrmFactory.h
  Helpers.h
  HelperPr.h
  HelperWv.h
)

add_dir_sources(SOURCES HEADERS)

add_subdirectory(widevine)
add_subdirectory(playready)
add_subdirectory(clearkey)
