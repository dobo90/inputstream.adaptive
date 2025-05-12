#include "PlayReadyCdm.h"

#include "decrypters/Helpers.h"
#include "playready.h"
#include "rapidjson/document.h"
#include "rapidjson/error/error.h"
#include "rapidjson/pointer.h"
#include "utils/CurlUtils.h"
#include "utils/FileUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include <kodi/Filesystem.h>

using namespace UTILS;

PlayreadyCdm::~PlayreadyCdm()
{
  playready_cdm_free(m_cdm);
  m_cdm = nullptr;
}

std::vector<std::string_view> PlayreadyCdm::SelectKeySystems(std::string_view keySystem)
{
  std::vector<std::string_view> keySystems;
  if (keySystem == KS_PLAYREADY)
  {
    keySystems.emplace_back(URN_PLAYREADY);
  }
  return keySystems;
}

bool PlayreadyCdm::OpenDRMSystem(std::string_view licenseURL,
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
  playready_cdm_free(m_cdm);
  m_cdm = playready_cdm_create_from_prd(prdPath.c_str(), &error);

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to instantiate cdm: %s", error);
    playready_error_message_free(error);
    error = nullptr;
  }

  return m_cdm != nullptr;
}

bool PlayreadyCdm::GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
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
  std::unique_ptr<playready_pssh_t, decltype(&playready_pssh_free)> prPssh(
      playready_pssh_from_bytes(slice_ref_uint8_t{pssh.data(), pssh.size()}, &error),
      &playready_pssh_free);

  if (prPssh == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to create pssh object: %s", error);
    playready_error_message_free(error);
    error = nullptr;
    return false;
  }

  playready_wrm_header_t* wrmHeader = playready_pssh_get_first_wrm_header(prPssh.get());

  if (wrmHeader == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to get wrm header");
    return false;
  }

  std::unique_ptr<playready_session_t, decltype(&playready_session_free)> session(
      playready_cdm_open_session(m_cdm), &playready_session_free);

  std::unique_ptr<char, decltype(&playready_license_challenge_free)> challenge(
      playready_session_get_license_challenge(session.get(), wrmHeader, &error),
      &playready_license_challenge_free);
  wrmHeader = nullptr;

  if (challenge == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to get challenge: %s", error);
    playready_error_message_free(error);
    error = nullptr;
    return false;
  }

  std::vector<std::string> blocks{STRING::SplitToVec(m_licenseUrl, '|')};
  std::string_view requestUrl;
  std::string_view licenseJsonPointer;
  std::string_view headers;
  std::string requestBody;

  if (blocks.size() == 4)
  {
    requestUrl = blocks[0];
    headers = blocks[1];
    licenseJsonPointer = blocks[3];

    if (!blocks[2].empty())
    {
      constexpr static char MARKER[] = "{XML}";
      constexpr size_t MARKER_LEN = std::size(MARKER) - 1;

      requestBody = STRING::URLDecode(blocks[2]);
      const size_t pos = requestBody.find(MARKER);

      if (pos == std::string::npos)
      {
        LOG::LogF(LOGERROR, "Failed to find %s marker", MARKER);
        return false;
      }

      if (licenseJsonPointer.empty())
      {
        requestBody.replace(pos, MARKER_LEN, challenge.get());
      }
      else
      {
        std::string escapedChallenge{challenge.get()};
        STRING::ReplaceAll(escapedChallenge, "\"", "\\\"");
        requestBody.replace(pos, MARKER_LEN, escapedChallenge);
      }
    }
  }
  else if (blocks.size() == 1)
  {
    requestUrl = m_licenseUrl;
  }
  else
  {
    LOG::LogF(LOGERROR, "Unsupported license URL format");
    return false;
  }

  CURL::CUrl curl{requestUrl, !requestBody.empty() ? requestBody : challenge.get()};
  curl.AddHeader("Content-Type", !licenseJsonPointer.empty() ? "application/json; charset=utf-8"
                                                             : "text/xml; charset=utf-8");

  if (!headers.empty())
  {
    // based on CWVCencSingleSampleDecrypter::SendSessionMessage()
    for (const std::string& header : STRING::SplitToVec(headers, '&'))
    {
      std::vector<std::string> pair{STRING::SplitToVec(header, '=')};

      if (pair.size() == 2)
      {
        std::string header = STRING::URLDecode(pair[0]);
        std::string value = STRING::URLDecode(pair[1]);

        curl.AddHeader(header, value);
      }
    }
  }

  std::string response;
  int statusCode = curl.Open();
  if (statusCode == -1 || statusCode >= 400)
  {
    LOG::Log(LOGERROR, "License server returned failure (HTTP error %i)", statusCode);
    return false;
  }

  if (curl.Read(response) != CURL::ReadStatus::IS_EOF)
  {
    LOG::LogF(LOGERROR, "Could not read the license server response");
    return false;
  }

  if (!licenseJsonPointer.empty())
  {
    rapidjson::Document doc;
    rapidjson::ParseResult result = doc.Parse(response.c_str());

    if (!result)
    {
      LOG::LogF(LOGERROR, "Failed to parse license response");
      return false;
    }

    const rapidjson::Value* value = rapidjson::Pointer(licenseJsonPointer.data()).Get(doc);

    if (value == nullptr)
    {
      LOG::LogF(LOGERROR, "Failed to get value using JSON pointer in license response");
      return false;
    }

    response = value->GetString();
  }

  Vec_playready_kid_ck_t keys =
      playready_session_get_keys_from_challenge_response(session.get(), response.c_str(), &error);

  if (error != nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to get keys for challenge response: %s", error);
    playready_error_message_free(error);
    error = nullptr;
    return false;
  }

  bool foundKey = false;

  for (size_t i = 0; i < keys.len; i++)
  {
    std::vector<uint8_t> newKid{std::begin(keys.ptr[i].kid.idx), std::end(keys.ptr[i].kid.idx)};
    std::vector<uint8_t> newKey{keys.ptr[i].ck.ptr, keys.ptr[i].ck.ptr + keys.ptr[i].ck.len};

    if (kid == newKid)
    {
      foundKey = true;
    }

    AddKey(std::move(newKid), std::move(newKey));
  }

  playready_keys_free(keys);
  return foundKey;
}