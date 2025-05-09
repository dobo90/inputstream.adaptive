set(SOURCES
  DrmEngine.cpp
  DrmFactory.cpp
  Helpers.cpp
  HelperPr.cpp
  HelperWv.cpp
)

set(HEADERS
  DrmEngine.h
  DrmEngineDefines.h
  DrmFactory.h
  Helpers.h
  HelperPr.h
  HelperWv.h
  IDecrypter.h
)

add_dir_sources(SOURCES HEADERS)