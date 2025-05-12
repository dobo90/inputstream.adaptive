#pragma once

#include "decrypters/Cdm.h"

#include <string>
#include <vector>

typedef struct playready_cdm playready_cdm_t;

using namespace DRM;

class PlayreadyCdm : public Cdm
{
public:
  PlayreadyCdm() {}
  virtual ~PlayreadyCdm() override;
  virtual std::vector<std::string_view> SelectKeySystems(std::string_view keySystem) override;
  virtual bool OpenDRMSystem(std::string_view licenseURL,
                             const std::vector<uint8_t>& serverCertificate,
                             const uint8_t config) override;
  virtual bool IsInitialised() override { return m_cdm != nullptr; }
  virtual void SetLibraryPath(std::string_view libraryPath) override
  {
    m_libraryPath = libraryPath;
  }
  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) override;

private:
  std::string m_libraryPath;
  std::string m_licenseUrl;
  playready_cdm_t* m_cdm{nullptr};
};
