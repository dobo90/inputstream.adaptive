#include "PlayReadyDecrypter.h"

#include "PlayReadyCencSingleSampleDecrypter.h"
#include "decrypters/Helpers.h"
#include "utils/FileUtils.h"
#include "utils/log.h"

#include <kodi/Filesystem.h>

using namespace UTILS;

CPlayReadyDecrypter::~CPlayReadyDecrypter()
{
  if (m_cdm != nullptr)
  {
    playready_cdm_free(m_cdm);
    m_cdm = nullptr;
  }
}

std::vector<std::string_view> CPlayReadyDecrypter::SelectKeySystems(std::string_view keySystem)
{
  std::vector<std::string_view> keySystems;
  if (keySystem == KS_PLAYREADY)
  {
    keySystems.emplace_back(URN_PLAYREADY);
  }
  return keySystems;
}

bool CPlayReadyDecrypter::OpenDRMSystem(std::string_view licenseURL,
                                        const std::vector<uint8_t>& serverCertificate,
                                        const uint8_t config)
{
  if (licenseURL.empty())
  {
    LOG::LogF(LOGERROR, "License Key property cannot be empty");
    return false;
  }

  m_licenseUrl = licenseURL;

  char* error = nullptr;
  const std::string prdPath = FILESYS::PathCombine(m_libraryPath, "device.prd");
  m_cdm = playready_cdm_create_from_prd(prdPath.c_str(), &error);

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to instantiate cdm: %s", error);
    playready_error_message_free(error);
    error = nullptr;
  }

  return m_cdm != nullptr;
}

Adaptive_CencSingleSampleDecrypter* CPlayReadyDecrypter::CreateSingleSampleDecrypter(
    std::vector<uint8_t>& initData,
    std::string_view optionalKeyParameter,
    const std::vector<uint8_t>& defaultKeyId,
    std::string_view licenseUrl,
    bool skipSessionMessage,
    CryptoMode cryptoMode)
{
  if (licenseUrl.empty())
  {
    licenseUrl = m_licenseUrl;
  }

  CPlayReadyCencSingleSampleDecrypter* decrypter = new CPlayReadyCencSingleSampleDecrypter(
      licenseUrl, initData, defaultKeyId, cryptoMode, m_keys, m_cdm);

  if (!decrypter->HasKeyId(defaultKeyId))
  {
    delete decrypter;
    decrypter = nullptr;
  }

  return decrypter;
}

void CPlayReadyDecrypter::DestroySingleSampleDecrypter(
    Adaptive_CencSingleSampleDecrypter* decrypter)
{
  if (decrypter)
  {
    delete static_cast<CPlayReadyCencSingleSampleDecrypter*>(decrypter);
  }
}

bool CPlayReadyDecrypter::HasLicenseKey(Adaptive_CencSingleSampleDecrypter* decrypter,
                                        const std::vector<uint8_t>& keyId)
{
  if (decrypter)
  {
    return static_cast<CPlayReadyCencSingleSampleDecrypter*>(decrypter)->HasKeyId(keyId);
  }

  return false;
}
