#pragma once

#include "decrypters/Cdm.h"

#include <string>
#include <string_view>
#include <vector>

typedef struct widevine_cdm widevine_cdm_t;

using namespace DRM;

class WidevineCdm : public Cdm
{
public:
  WidevineCdm() {}
  virtual ~WidevineCdm() override;
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
  std::string SendSessionMessage(const std::vector<uint8_t>& kid,
                                 uint8_t* challengePtr,
                                 size_t challengeLen);

  std::string m_strSession{"widevine-rs"};
  std::string m_libraryPath;
  std::string m_licenseUrl;
  std::vector<uint8_t> m_serverCertificate;
  widevine_cdm_t* m_cdm{nullptr};
};
