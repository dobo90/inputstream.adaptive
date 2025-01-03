/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WVCencSingleSampleDecrypter.h"

#include "CompSettings.h"
#include "SrvBroker.h"
#include "WVDecrypter.h"
#include "decrypters/Helpers.h"
#include "jsmn.h"
#include "utils/Base64Utils.h"
#include "utils/CurlUtils.h"
#include "utils/DigestMD5Utils.h"
#include "utils/FileUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

using namespace UTILS;
using namespace kodi::tools;

CWVCencSingleSampleDecrypter::CWVCencSingleSampleDecrypter(
    std::string_view licenseUrl,
    std::vector<uint8_t>& pssh,
    const std::vector<uint8_t>& defaultKeyId,
    CryptoMode cryptoMode,
    const std::vector<uint8_t>& serverCertificate,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys,
    widevine_cdm_t* cdm,
    CWVDecrypter* host)
  : m_pssh(pssh),
    m_defaultKeyId(defaultKeyId),
    m_hasKey(false),
    m_hdcpLimit(0),
    m_resolutionLimit(0),
    m_host(host)
{
  SetParentIsOwner(false);

  static unsigned int SESSION_COUNTER = 0;
  AP4_UI32 ap4CryptoMode = 0;

  switch (cryptoMode)
  {
    case CryptoMode::AES_CTR:
      ap4CryptoMode = AP4_CENC_CIPHER_AES_128_CTR;
      break;
    case CryptoMode::AES_CBC:
      ap4CryptoMode = AP4_CENC_CIPHER_AES_128_CBC;
      break;
    case CryptoMode::NONE:
    default:
      LOG::LogF(LOGERROR, "Unsupported crypto mode");
      return;
  }

  if (CSrvBroker::GetSettings().IsDebugLicense())
  {
    std::string debugFilePath =
        FILESYS::PathCombine(m_host->GetLibraryPath(), "EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED.init");

    std::string data{reinterpret_cast<const char*>(m_pssh.data()), m_pssh.size()};
    UTILS::FILESYS::SaveFile(debugFilePath, data, true);
  }

  auto it = cdmKeys.find(defaultKeyId);

  if (it == std::end(cdmKeys))
  {
    GetKeysFromCdm(licenseUrl, pssh, defaultKeyId, serverCertificate, cdmKeys, cdm);
    it = cdmKeys.find(defaultKeyId);

    if (it == std::end(cdmKeys))
    {
      LOG::LogF(LOGERROR, "Failed to find key for default kid");
      return;
    }
  }

  const std::vector<uint8_t>& key = it->second;

  if (AP4_FAILED(AP4_CencSingleSampleDecrypter::Create(ap4CryptoMode, key.data(),
                                                       static_cast<AP4_Size>(key.size()), 0, 0,
                                                       nullptr, false, m_singleSampleDecrypter)))
  {
    LOG::LogF(LOGERROR, "Failed to create AP4_CencSingleSampleDecrypter");
  }

  m_hasKey = true;
  m_strSession = "widevine-rs-" + std::to_string(SESSION_COUNTER++);
}

CWVCencSingleSampleDecrypter::~CWVCencSingleSampleDecrypter()
{
  if (m_singleSampleDecrypter)
  {
    delete m_singleSampleDecrypter;
    m_singleSampleDecrypter = nullptr;
  }
}

AP4_DataBuffer CWVCencSingleSampleDecrypter::GetChallengeData()
{
  return m_challenge;
}

std::string CWVCencSingleSampleDecrypter::SendSessionMessage(std::string_view licenseUrl)
{
  std::vector<std::string> blocks{STRING::SplitToVec(licenseUrl, '|')};
  std::string licenseMessage;

  if (blocks.size() != 4)
  {
    LOG::LogF(LOGERROR, "Wrong \"|\" blocks in license URL. Four blocks (req | header | body | "
                        "response) are expected in license URL");
    return {};
  }

  if (CSrvBroker::GetSettings().IsDebugLicense())
  {
    std::string debugFilePath = FILESYS::PathCombine(
        m_host->GetLibraryPath(), "EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED.challenge");
    std::string data{reinterpret_cast<const char*>(m_challenge.GetData()),
                     m_challenge.GetDataSize()};
    UTILS::FILESYS::SaveFile(debugFilePath, data, true);
  }

  //Process placeholder in GET String
  std::string::size_type insPos(blocks[0].find("{SSM}"));
  if (insPos != std::string::npos)
  {
    if (insPos > 0 && blocks[0][insPos - 1] == 'B')
    {
      std::string msgEncoded{BASE64::Encode(m_challenge.GetData(), m_challenge.GetDataSize())};
      msgEncoded = STRING::URLEncode(msgEncoded);
      blocks[0].replace(insPos - 1, 6, msgEncoded);
    }
    else
    {
      LOG::Log(LOGERROR, "Unsupported License request template (command)");
      return {};
    }
  }

  insPos = blocks[0].find("{HASH}");
  if (insPos != std::string::npos)
  {
    DIGEST::MD5 md5;
    md5.Update(m_challenge.GetData(), m_challenge.GetDataSize());
    md5.Finalize();
    blocks[0].replace(insPos, 6, md5.HexDigest());
  }

  CURL::CUrl file{blocks[0].c_str()};
  file.AddHeader("Expect", "");

  std::string response;
  std::string resLimit;
  std::string contentType;
  char buf[2048];
  bool serverCertRequest;

  //Process headers
  std::vector<std::string> headers{StringUtils::Split(blocks[1], '&')};
  for (std::string& headerStr : headers)
  {
    std::vector<std::string> header{StringUtils::Split(headerStr, '=')};
    if (!header.empty())
    {
      StringUtils::Trim(header[0]);
      std::string value;
      if (header.size() > 1)
      {
        StringUtils::Trim(header[1]);
        value = STRING::URLDecode(header[1]);
      }
      file.AddHeader(header[0].c_str(), value.c_str());
    }
  }

  //Process body
  if (!blocks[2].empty())
  {
    if (blocks[2][0] == '%')
      blocks[2] = STRING::URLDecode(blocks[2]);

    insPos = blocks[2].find("{SSM}");
    if (insPos != std::string::npos)
    {
      std::string::size_type sidPos(blocks[2].find("{SID}"));
      std::string::size_type kidPos(blocks[2].find("{KID}"));

      char fullDecode = 0;
      if (insPos > 1 && sidPos > 1 && kidPos > 1 && (blocks[2][0] == 'b' || blocks[2][0] == 'B') &&
          blocks[2][1] == '{')
      {
        fullDecode = blocks[2][0];
        blocks[2] = blocks[2].substr(2, blocks[2].size() - 3);
        insPos -= 2;
        if (kidPos != std::string::npos)
          kidPos -= 2;
        if (sidPos != std::string::npos)
          sidPos -= 2;
      }

      size_t size_written(0);

      if (insPos > 0)
      {
        if (blocks[2][insPos - 1] == 'B' || blocks[2][insPos - 1] == 'b')
        {
          std::string msgEncoded{BASE64::Encode(m_challenge.GetData(), m_challenge.GetDataSize())};
          if (blocks[2][insPos - 1] == 'B')
          {
            msgEncoded = STRING::URLEncode(msgEncoded);
          }
          blocks[2].replace(insPos - 1, 6, msgEncoded);
          size_written = msgEncoded.size();
        }
        else if (blocks[2][insPos - 1] == 'D')
        {
          std::string msgEncoded{
              STRING::ToDecimal(m_challenge.GetData(), m_challenge.GetDataSize())};
          blocks[2].replace(insPos - 1, 6, msgEncoded);
          size_written = msgEncoded.size();
        }
        else
        {
          blocks[2].replace(insPos - 1, 6, reinterpret_cast<const char*>(m_challenge.GetData()),
                            m_challenge.GetDataSize());
          size_written = m_challenge.GetDataSize();
        }
      }
      else
      {
        LOG::Log(LOGERROR, "Unsupported License request template (body / ?{SSM})");
        return {};
      }

      if (sidPos != std::string::npos && insPos < sidPos)
        sidPos += size_written, sidPos -= 6;

      if (kidPos != std::string::npos && insPos < kidPos)
        kidPos += size_written, kidPos -= 6;

      size_written = 0;

      if (sidPos != std::string::npos)
      {
        if (sidPos > 0)
        {
          if (blocks[2][sidPos - 1] == 'B' || blocks[2][sidPos - 1] == 'b')
          {
            std::string msgEncoded{BASE64::Encode(m_strSession)};

            if (blocks[2][sidPos - 1] == 'B')
            {
              msgEncoded = STRING::URLEncode(msgEncoded);
            }

            blocks[2].replace(sidPos - 1, 6, msgEncoded);
            size_written = msgEncoded.size();
          }
          else
          {
            blocks[2].replace(sidPos - 1, 6, m_strSession.data(), m_strSession.size());
            size_written = m_strSession.size();
          }
        }
        else
        {
          LOG::LogF(LOGERROR, "Unsupported License request template (body / ?{SID})");
          return {};
        }
      }

      if (kidPos != std::string::npos)
      {
        if (sidPos < kidPos)
          kidPos += size_written, kidPos -= 6;

        if (blocks[2][kidPos - 1] == 'H')
        {
          std::string keyIdUUID{STRING::ToHexadecimal(m_defaultKeyId)};
          blocks[2].replace(kidPos - 1, 6, keyIdUUID.c_str(), 32);
        }
        else
        {
          std::string kidUUID{DRM::ConvertKidBytesToUUID(m_defaultKeyId)};
          blocks[2].replace(kidPos, 5, kidUUID.c_str(), 36);
        }
      }

      if (fullDecode)
      {
        std::string msgEncoded{BASE64::Encode(blocks[2])};
        if (fullDecode == 'B')
        {
          msgEncoded = STRING::URLEncode(msgEncoded);
        }
        blocks[2] = msgEncoded;
      }
    }

    std::string encData{BASE64::Encode(blocks[2])};
    //! @todo: inappropriate use of "postdata" header, use CURL::CUrl for post request
    file.AddHeader("postdata", encData.c_str());
  }

  serverCertRequest = m_challenge.GetDataSize() == 2;
  m_challenge.SetDataSize(0);

  int statusCode = file.Open();
  if (statusCode == -1 || statusCode >= 400)
  {
    LOG::Log(LOGERROR, "License server returned failure (HTTP error %i)", statusCode);
    return {};
  }

  CURL::ReadStatus downloadStatus = CURL::ReadStatus::CHUNK_READ;
  while (downloadStatus == CURL::ReadStatus::CHUNK_READ)
  {
    downloadStatus = file.Read(response);
  }

  resLimit = file.GetResponseHeader("X-Limit-Video");
  contentType = file.GetResponseHeader("Content-Type");

  if (!resLimit.empty())
  {
    std::string::size_type posMax = resLimit.find("max="); // log/check this
    if (posMax != std::string::npos)
      m_resolutionLimit = std::atoi(resLimit.data() + (posMax + 4));
  }

  if (downloadStatus == CURL::ReadStatus::ERROR)
  {
    LOG::LogF(LOGERROR, "Could not read full SessionMessage response");
    return {};
  }

  if (CSrvBroker::GetSettings().IsDebugLicense())
  {
    std::string debugFilePath = FILESYS::PathCombine(
        m_host->GetLibraryPath(), "EDEF8BA9-79D6-4ACE-A3C8-27DCD51D21ED.response");
    FILESYS::SaveFile(debugFilePath, response, true);
  }

  if (serverCertRequest && contentType.find("application/octet-stream") == std::string::npos)
    serverCertRequest = false;

  if (!blocks[3].empty() && blocks[3][0] != 'R' && !serverCertRequest)
  {
    if (blocks[3][0] == 'J' || (blocks[3].size() > 1 && blocks[3][0] == 'B' && blocks[3][1] == 'J'))
    {
      int dataPos = 2;

      if (response.size() >= 3 && blocks[3][0] == 'B')
      {
        response = BASE64::DecodeToStr(response);
        dataPos = 3;
      }

      jsmn_parser jsn;
      jsmntok_t tokens[256];

      jsmn_init(&jsn);
      int i(0), numTokens = jsmn_parse(&jsn, response.c_str(), response.size(), tokens, 256);

      std::vector<std::string> jsonVals{StringUtils::Split(blocks[3].substr(dataPos), ';')};

      // Find HDCP limit
      if (jsonVals.size() > 1)
      {
        for (; i < numTokens; ++i)
          if (tokens[i].type == JSMN_STRING && tokens[i].size == 1 &&
              jsonVals[1].size() == static_cast<unsigned int>(tokens[i].end - tokens[i].start) &&
              strncmp(response.c_str() + tokens[i].start, jsonVals[1].c_str(),
                      tokens[i].end - tokens[i].start) == 0)
            break;
        if (i < numTokens)
          m_hdcpLimit = std::atoi((response.c_str() + tokens[i + 1].start));
      }
      // Find license key
      if (jsonVals.size() > 0)
      {
        for (i = 0; i < numTokens; ++i)
          if (tokens[i].type == JSMN_STRING && tokens[i].size == 1 &&
              jsonVals[0].size() == static_cast<unsigned int>(tokens[i].end - tokens[i].start) &&
              strncmp(response.c_str() + tokens[i].start, jsonVals[0].c_str(),
                      tokens[i].end - tokens[i].start) == 0)
          {
            if (i + 1 < numTokens && tokens[i + 1].type == JSMN_ARRAY && tokens[i + 1].size == 1)
              ++i;
            break;
          }
      }
      else
        i = numTokens;

      if (i < numTokens)
      {
        std::string respData{
            response.substr(tokens[i + 1].start, tokens[i + 1].end - tokens[i + 1].start)};

        if (blocks[3][dataPos - 1] == 'B')
        {
          respData = BASE64::DecodeToStr(respData);
        }

        licenseMessage = std::string(respData.c_str(), respData.size());
      }
      else
      {
        LOG::LogF(LOGERROR, "Unable to find %s in JSON string", blocks[3].c_str() + 2);
        return {};
      }
    }
    else if (blocks[3][0] == 'H' && blocks[3].size() >= 2)
    {
      //Find the payload
      std::string::size_type payloadPos = response.find("\r\n\r\n");
      if (payloadPos != std::string::npos)
      {
        payloadPos += 4;
        if (blocks[3][1] == 'B')
        {
          licenseMessage = std::string(response.c_str() + payloadPos, response.size() - payloadPos);
        }
        else
        {
          LOG::LogF(LOGERROR, "Unsupported HTTP payload data type definition");
          return {};
        }
      }
      else
      {
        LOG::LogF(LOGERROR, "Unable to find HTTP payload in response");
        return {};
      }
    }
    else if (blocks[3][0] == 'B' && blocks[3].size() == 1)
    {
      std::string decRespData{BASE64::DecodeToStr(response)};

      licenseMessage = std::string(decRespData.c_str(), decRespData.size());
    }
    else
    {
      LOG::LogF(LOGERROR, "Unsupported License request template (response)");
      return {};
    }
  }
  else // its binary - simply push the returned data as update
  {
    licenseMessage = std::string(response.data(), response.size());
  }

  LOG::Log(LOGDEBUG, "License update successful");

  return licenseMessage;
}

AP4_Result CWVCencSingleSampleDecrypter::DecryptSampleData(AP4_UI32 poolId,
                                                           AP4_DataBuffer& dataIn,
                                                           AP4_DataBuffer& dataOut,
                                                           const AP4_UI08* iv,
                                                           unsigned int subsampleCount,
                                                           const AP4_UI16* bytesOfCleartextData,
                                                           const AP4_UI32* bytesOfEncryptedData)
{
  if (!m_singleSampleDecrypter)
  {
    return AP4_FAILURE;
  }

  return m_singleSampleDecrypter->DecryptSampleData(dataIn, dataOut, iv, subsampleCount,
                                                    bytesOfCleartextData, bytesOfEncryptedData);
}

bool CWVCencSingleSampleDecrypter::HasKeyId(const std::vector<uint8_t>& keyId)
{
  return m_hasKey && m_defaultKeyId == keyId;
}

void CWVCencSingleSampleDecrypter::GetKeysFromCdm(
    std::string_view licenseUrl,
    std::vector<uint8_t>& initData,
    const std::vector<uint8_t>& keyId,
    const std::vector<uint8_t>& serverCertificate,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys,
    widevine_cdm_t* cdm)
{
  char* error = nullptr;

  widevine_cdm_session_t* session = widevine_cdm_open(cdm);

  if (!serverCertificate.empty())
  {
    session = widevine_cdm_session_set_service_certificate(
        session, slice_ref_uint8_t{serverCertificate.data(), serverCertificate.size()}, &error);

    if (session == nullptr)
    {
      LOG::LogF(LOGERROR, "Failed to set server certificate: %s", error);
      return;
    }
  }

  widevine_pssh_t* pssh =
      widevine_pssh_from_bytes(slice_ref_uint8{m_pssh.data(), m_pssh.size()}, &error);

  if (pssh == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to create pssh: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return;
  }

  std::unique_ptr<widevine_cdm_license_request_t, decltype(&widevine_cdm_license_request_free)>
      request(widevine_cdm_session_get_license_request(
                  session, pssh, WIDEVINE_LICENSE_TYPE_S_T_R_E_A_M_I_N_G, &error),
              &widevine_cdm_license_request_free);
  session = nullptr;
  pssh = nullptr;

  if (request == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to create request: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return;
  }

  Vec_uint8_t challenge = widevine_cdm_license_request_challenge(request.get(), &error);

  if (error != nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to obtain challenge: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return;
  }

  m_challenge.SetData(challenge.ptr, challenge.len);
  widevine_challenge_free(challenge);

  const std::string licenseMessage = SendSessionMessage(licenseUrl);

  if (licenseMessage.empty())
  {
    LOG::LogF(LOGERROR, "Failed to get license response");
    return;
  }

  slice_ref_uint8_t slice{reinterpret_cast<const uint8_t*>(licenseMessage.data()),
                          licenseMessage.size()};
  Vec_widevine_content_key_t keys =
      widevine_license_request_get_content_keys(request.get(), slice, &error);

  if (error != nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to get content keys: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return;
  }

  for (size_t i = 0; i < keys.len; i++)
  {
    std::vector<uint8_t> kid{std::begin(keys.ptr[i].kid.idx), std::end(keys.ptr[i].kid.idx)};
    std::vector<uint8_t> key{keys.ptr[i].key.ptr, keys.ptr[i].key.ptr + keys.ptr[i].key.len};

    cdmKeys[kid] = key;
  }

  widevine_content_keys_free(keys);
}