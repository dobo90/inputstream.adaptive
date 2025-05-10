#pragma once

#include "decrypters/Cdm.h"

typedef struct widevine_cdm widevine_cdm_t;

using namespace DRM;

class WidevineCdm : public Cdm
{
public:
  WidevineCdm() { m_strSession = "widevine-rs"; }
  virtual ~WidevineCdm() override;
  virtual SResult OpenDRMSystem(const DRM::Config& config) override;
  virtual bool IsInitialised() override { return m_cdm != nullptr; }
  virtual void SetLibraryPath(std::string_view libraryPath) override
  {
    m_libraryPath = libraryPath;
  }
  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) override;

private:
  std::string m_libraryPath;
  widevine_cdm_t* m_cdm{nullptr};
};
