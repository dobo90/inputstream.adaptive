#pragma once

#include "decrypters/Cdm.h"

#include <string_view>
#include <vector>

using namespace DRM;

class ClearKeyCdm : public Cdm
{
public:
  ClearKeyCdm() {}
  virtual ~ClearKeyCdm() {}
  virtual SResult OpenDRMSystem(const DRM::Config& config) override;
  virtual bool IsInitialised() override { return m_isInitialised; }
  virtual void SetLibraryPath(std::string_view libraryPath) override {}
  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) override;

private:
  bool m_isInitialised{false};
};
