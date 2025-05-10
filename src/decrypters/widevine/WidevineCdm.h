#pragma once

#include "decrypters/Cdm.h"
#include "decrypters/Helpers.h"

typedef struct widevine_cdm widevine_cdm_t;

using namespace DRM;

class WidevineCdm : public Cdm
{
public:
  WidevineCdm() { m_strSession = "widevine-rs"; }
  virtual ~WidevineCdm() override;
  virtual const std::string GetName() const override { return "Widevine"; }
  virtual bool Initialize(const DRM::Config& drmConfig, std::string_view decrypterPath) override;
  virtual std::string_view GetKeySystem() const override { return KS_WIDEVINE; };
  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) override;

private:
  widevine_cdm_t* m_cdm{nullptr};
};
