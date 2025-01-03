/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "../IDecrypter.h"
#include "common/AdaptiveCencSampleDecrypter.h"
#include "widevine.h"

#include <optional>

class CWVDecrypter;

using namespace DRM;

class ATTR_DLL_LOCAL CWVCencSingleSampleDecrypter : public Adaptive_CencSingleSampleDecrypter
{
public:
  CWVCencSingleSampleDecrypter(std::string_view licenseUrl,
                               std::vector<uint8_t>& pssh,
                               const std::vector<uint8_t>& defaultKeyId,
                               CryptoMode cryptoMode,
                               const std::vector<uint8_t>& serverCertificate,
                               std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys,
                               widevine_cdm_t* cdm,
                               CWVDecrypter* host);
  virtual ~CWVCencSingleSampleDecrypter();

  virtual AP4_Result SetFragmentInfo(AP4_UI32 poolId,
                                     const std::vector<uint8_t>& keyId,
                                     const AP4_UI08 nalLengthSize,
                                     AP4_DataBuffer& annexbSpsPps,
                                     AP4_UI32 flags,
                                     CryptoInfo cryptoInfo) override
  {
    return AP4_SUCCESS;
  }

  virtual AP4_Result DecryptSampleData(AP4_UI32 poolId,
                                       AP4_DataBuffer& dataIn,
                                       AP4_DataBuffer& dataOut,
                                       const AP4_UI08* iv,
                                       unsigned int subsampleCount,
                                       const AP4_UI16* bytesOfCleartextData,
                                       const AP4_UI32* bytesOfEncryptedData) override;

  void SetDefaultKeyId(const std::vector<uint8_t>& keyId) override {}
  void AddKeyId(const std::vector<uint8_t>& keyId) override {}
  virtual const char* GetSessionId() override { return m_strSession.c_str(); }

  bool HasKeyId(const std::vector<uint8_t>& keyId);
  AP4_DataBuffer GetChallengeData();

private:
  void GetKeysFromCdm(std::string_view licenseUrl,
                      std::vector<uint8_t>& initData,
                      const std::vector<uint8_t>& keyId,
                      const std::vector<uint8_t>& serverCertificate,
                      std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys,
                      widevine_cdm_t* cdm);
  std::string SendSessionMessage(std::string_view licenseUrl);

  std::string m_strSession;
  std::vector<uint8_t> m_pssh;
  AP4_DataBuffer m_challenge;
  std::vector<uint8_t> m_defaultKeyId;
  bool m_hasKey;

  int m_hdcpLimit;
  int m_resolutionLimit;

  AP4_CencSingleSampleDecrypter* m_singleSampleDecrypter{nullptr};
  CWVDecrypter* m_host;
};
