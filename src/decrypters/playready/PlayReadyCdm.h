#pragma once

#include "decrypters/Cdm.h"
#include "decrypters/Helpers.h"

typedef struct playready_cdm playready_cdm_t;

using namespace DRM;

class PlayreadyCdm : public Cdm
{
public:
  PlayreadyCdm() { m_strSession = "playready-rs"; }
  virtual ~PlayreadyCdm() override;
  virtual const std::string GetName() const override { return "PlayReady"; }
  virtual bool Initialize(const DRM::Config& drmConfig, std::string_view decrypterPath) override;
  virtual std::string_view GetKeySystem() const override { return KS_PLAYREADY; };
  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) override;

private:
  playready_cdm_t* m_cdm{nullptr};
};
