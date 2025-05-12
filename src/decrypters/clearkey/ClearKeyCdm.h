#pragma once

#include "decrypters/Cdm.h"
#include "decrypters/Helpers.h"

#include <string_view>
#include <vector>

using namespace DRM;

class ClearKeyCdm : public Cdm
{
public:
  ClearKeyCdm() {}
  virtual ~ClearKeyCdm() {}
  virtual const std::string GetName() const override { return "ClearKey"; }
  virtual bool Initialize(const DRM::Config& drmConfig, std::string_view decrypterPath) override;
  virtual std::string_view GetKeySystem() const override { return KS_CLEARKEY; };
  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) override;
};
