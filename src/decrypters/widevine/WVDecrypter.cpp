/*
 *  Copyright (C) 2016 liberty-developer (https://github.com/liberty-developer)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WVDecrypter.h"

#include "WVCencSingleSampleDecrypter.h"
#include "decrypters/Helpers.h"
#include "utils/Base64Utils.h"
#include "utils/FileUtils.h"
#include "utils/log.h"

#include <kodi/Filesystem.h>

using namespace UTILS;

CWVDecrypter::~CWVDecrypter()
{
  if (m_cdm != nullptr)
  {
    widevine_cdm_free(m_cdm);
    m_cdm = nullptr;
  }
}

std::vector<std::string_view> CWVDecrypter::SelectKeySystems(std::string_view keySystem)
{
  std::vector<std::string_view> keySystems;
  if (keySystem == KS_WIDEVINE)
    keySystems.emplace_back(URN_WIDEVINE);

  return keySystems;
}

bool CWVDecrypter::OpenDRMSystem(std::string_view licenseURL,
                                 const std::vector<uint8_t>& serverCertificate,
                                 const uint8_t config)
{
  if (licenseURL.empty())
  {
    LOG::LogF(LOGERROR, "License Key property cannot be empty");
    return false;
  }

  m_licenseUrl = licenseURL;
  m_serverCertificate = serverCertificate;

  char* error = nullptr;
  const std::string wvdPath = FILESYS::PathCombine(m_libraryPath, "device.wvd");
  m_cdm = widevine_cdm_create_from_wvd(wvdPath.c_str(), &error);

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to instantiate cdm: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
  }

  return m_cdm != nullptr;
}

Adaptive_CencSingleSampleDecrypter* CWVDecrypter::CreateSingleSampleDecrypter(
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

  CWVCencSingleSampleDecrypter* decrypter = new CWVCencSingleSampleDecrypter(
      licenseUrl, initData, defaultKeyId, cryptoMode, m_serverCertificate, m_keys, m_cdm, this);

  if (!decrypter->HasKeyId(defaultKeyId))
  {
    delete decrypter;
    decrypter = nullptr;
  }

  return decrypter;
}

void CWVDecrypter::DestroySingleSampleDecrypter(Adaptive_CencSingleSampleDecrypter* decrypter)
{
  if (decrypter)
  {
    delete static_cast<CWVCencSingleSampleDecrypter*>(decrypter);
  }
}

bool CWVDecrypter::HasLicenseKey(Adaptive_CencSingleSampleDecrypter* decrypter,
                                 const std::vector<uint8_t>& keyId)
{
  if (decrypter)
  {
    return static_cast<CWVCencSingleSampleDecrypter*>(decrypter)->HasKeyId(keyId);
  }
  return false;
}

std::string CWVDecrypter::GetChallengeB64Data(Adaptive_CencSingleSampleDecrypter* decrypter)
{
  if (!decrypter)
    return "";

  AP4_DataBuffer challengeData =
      static_cast<CWVCencSingleSampleDecrypter*>(decrypter)->GetChallengeData();
  return BASE64::Encode(challengeData.GetData(), challengeData.GetDataSize());
}