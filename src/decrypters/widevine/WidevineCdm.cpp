#include "WidevineCdm.h"

#include "decrypters/Helpers.h"
#include "jsmn.h"
#include "utils/Base64Utils.h"
#include "utils/CurlUtils.h"
#include "utils/DigestMD5Utils.h"
#include "utils/FileUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"
#include "widevine.h"

#include <cstdint>

#include <kodi/Filesystem.h>

using namespace UTILS;
using namespace kodi::tools;

WidevineCdm::~WidevineCdm()
{
  widevine_cdm_free(m_cdm);
  m_cdm = nullptr;
}

std::vector<std::string_view> WidevineCdm::SelectKeySystems(std::string_view keySystem)
{
  std::vector<std::string_view> keySystems;
  if (keySystem == KS_WIDEVINE)
    keySystems.emplace_back(URN_WIDEVINE);

  return keySystems;
}

bool WidevineCdm::OpenDRMSystem(std::string_view licenseURL,
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
  widevine_cdm_free(m_cdm);
  m_cdm = widevine_cdm_create_from_wvd(wvdPath.c_str(), &error);

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to instantiate cdm: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
  }

  return m_cdm != nullptr;
}

bool WidevineCdm::GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                           const std::vector<uint8_t>& kid)
{
  if (GetKey(kid).has_value())
  {
    return true;
  }

  if (pssh.empty())
  {
    LOG::LogF(LOGERROR, "PSSH is empty");
    return false;
  }

  char* error = nullptr;
  widevine_pssh_t* wvPssh =
      widevine_pssh_from_bytes(slice_ref_uint8{pssh.data(), pssh.size()}, &error);

  if (wvPssh == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to create pssh: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return false;
  }

  widevine_cdm_session_t* session = widevine_cdm_open(m_cdm);

  if (!m_serverCertificate.empty())
  {
    session = widevine_cdm_session_set_service_certificate(
        session, slice_ref_uint8_t{m_serverCertificate.data(), m_serverCertificate.size()}, &error);

    if (session == nullptr)
    {
      LOG::LogF(LOGERROR, "Failed to set server certificate: %s", error);
      return false;
    }
  }

  std::unique_ptr<widevine_cdm_license_request_t, decltype(&widevine_cdm_license_request_free)>
      request(widevine_cdm_session_get_license_request(
                  session, wvPssh, WIDEVINE_LICENSE_TYPE_S_T_R_E_A_M_I_N_G, &error),
              &widevine_cdm_license_request_free);
  session = nullptr;
  wvPssh = nullptr;

  if (request == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to create request: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return false;
  }

  Vec_uint8_t challenge = widevine_cdm_license_request_challenge(request.get(), &error);

  if (error != nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to obtain challenge: %s", error);
    widevine_error_message_free(error);
    error = nullptr;
    return false;
  }

  const std::string licenseMessage = SendSessionMessage(kid, challenge.ptr, challenge.len);
  widevine_challenge_free(challenge);

  if (licenseMessage.empty())
  {
    LOG::LogF(LOGERROR, "Failed to get license response");
    return false;
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
    return false;
  }

  bool foundKey = false;

  for (size_t i = 0; i < keys.len; i++)
  {
    std::vector<uint8_t> newKid{std::begin(keys.ptr[i].kid.idx), std::end(keys.ptr[i].kid.idx)};
    std::vector<uint8_t> newKey{keys.ptr[i].key.ptr, keys.ptr[i].key.ptr + keys.ptr[i].key.len};

    if (kid == newKid)
    {
      foundKey = true;
    }

    AddKey(std::move(newKid), std::move(newKey));
  }

  widevine_content_keys_free(keys);
  return foundKey;
}

// Kept as close as possible to CWVCencSingleSampleDecrypter::SendSessionMessage()
std::string WidevineCdm::SendSessionMessage(const std::vector<uint8_t>& kid,
                                            uint8_t* challengePtr,
                                            size_t challengeLen)
{
  std::vector<std::string> blocks{STRING::SplitToVec(m_licenseUrl, '|')};
  std::string licenseMessage;

  if (blocks.size() != 4)
  {
    LOG::LogF(LOGERROR, "Wrong \"|\" blocks in license URL. Four blocks (req | header | body | "
                        "response) are expected in license URL");
    return {};
  }

  //Process placeholder in GET String
  std::string::size_type insPos(blocks[0].find("{SSM}"));
  if (insPos != std::string::npos)
  {
    if (insPos > 0 && blocks[0][insPos - 1] == 'B')
    {
      std::string msgEncoded{BASE64::Encode(challengePtr, challengeLen)};
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
    md5.Update(challengePtr, challengeLen);
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
          std::string msgEncoded{BASE64::Encode(challengePtr, challengeLen)};
          if (blocks[2][insPos - 1] == 'B')
          {
            msgEncoded = STRING::URLEncode(msgEncoded);
          }
          blocks[2].replace(insPos - 1, 6, msgEncoded);
          size_written = msgEncoded.size();
        }
        else if (blocks[2][insPos - 1] == 'D')
        {
          std::string msgEncoded{STRING::ToDecimal(challengePtr, challengeLen)};
          blocks[2].replace(insPos - 1, 6, msgEncoded);
          size_written = msgEncoded.size();
        }
        else
        {
          blocks[2].replace(insPos - 1, 6, reinterpret_cast<const char*>(challengePtr),
                            challengeLen);
          size_written = challengeLen;
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
          std::string keyIdUUID{STRING::ToHexadecimal(kid)};
          blocks[2].replace(kidPos - 1, 6, keyIdUUID.c_str(), 32);
        }
        else
        {
          std::string kidUUID{DRM::ConvertKidBytesToUUID(kid)};
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

  contentType = file.GetResponseHeader("Content-Type");

  if (downloadStatus == CURL::ReadStatus::ERROR)
  {
    LOG::LogF(LOGERROR, "Could not read full SessionMessage response");
    return {};
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