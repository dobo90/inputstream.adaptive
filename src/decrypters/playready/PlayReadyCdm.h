#pragma once

#include "decrypters/Cdm.h"

typedef struct playready_cdm playready_cdm_t;

using namespace DRM;

class PlayreadyCdm : public Cdm
{
public:
  PlayreadyCdm() { m_strSession = "playready-rs"; }
  virtual ~PlayreadyCdm() override;
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
  playready_cdm_t* m_cdm{nullptr};
};
