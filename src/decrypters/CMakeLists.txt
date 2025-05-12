set(SOURCES
  Cdm.cpp
  DrmFactory.cpp
  Helpers.cpp
)

set(HEADERS
  Cdm.h
  DrmFactory.h
  Helpers.h
)

add_dir_sources(SOURCES HEADERS)

add_subdirectory(widevine)
add_subdirectory(playready)
