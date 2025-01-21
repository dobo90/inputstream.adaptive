#include "PlayReadyCencSingleSampleDecrypter.h"

#include "PlayReadyDecrypter.h"
#include "rapidjson/document.h"
#include "rapidjson/error/error.h"
#include "rapidjson/pointer.h"
#include "utils/CurlUtils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

using namespace UTILS;

CPlayReadyCencSingleSampleDecrypter::CPlayReadyCencSingleSampleDecrypter(
    std::string_view licenseUrl,
    std::vector<uint8_t>& initData,
    const std::vector<uint8_t>& defaultKeyId,
    CryptoMode cryptoMode,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys,
    playready_cdm_t* cdm)
{
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

  if (licenseUrl.empty())
  {
    LOG::LogF(LOGERROR, "License server URL not found");
    return;
  }

  auto it = cdmKeys.find(defaultKeyId);

  if (it == std::end(cdmKeys))
  {
    GetKeysFromCdm(licenseUrl, initData, defaultKeyId, cdm, cdmKeys);
    it = cdmKeys.find(defaultKeyId);

    if (it == std::end(cdmKeys))
    {
      LOG::LogF(LOGERROR, "Failed to find key for default kid");
      return;
    }
  }

  std::vector<uint8_t> key = it->second;

  if (AP4_FAILED(AP4_CencSingleSampleDecrypter::Create(ap4CryptoMode, key.data(),
                                                       static_cast<AP4_Size>(key.size()), 0, 0,
                                                       nullptr, false, m_singleSampleDecrypter)))
  {
    LOG::LogF(LOGERROR, "Failed to create AP4_CencSingleSampleDecrypter");
  }
  SetParentIsOwner(false);

  m_keyId = defaultKeyId;
  m_strSession = "playready-rs-" + std::to_string(SESSION_COUNTER++);
}

CPlayReadyCencSingleSampleDecrypter::~CPlayReadyCencSingleSampleDecrypter()
{
  if (m_singleSampleDecrypter)
  {
    delete m_singleSampleDecrypter;
    m_singleSampleDecrypter = nullptr;
  }
}

AP4_Result CPlayReadyCencSingleSampleDecrypter::DecryptSampleData(
    AP4_UI32 poolId,
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
  return (m_singleSampleDecrypter)
      ->DecryptSampleData(dataIn, dataOut, iv, subsampleCount, bytesOfCleartextData,
                          bytesOfEncryptedData);
}

bool CPlayReadyCencSingleSampleDecrypter::HasKeyId(const std::vector<uint8_t>& keyId)
{
  if (!m_keyId.has_value())
  {
    return false;
  }

  return m_keyId.value() == keyId;
}

void CPlayReadyCencSingleSampleDecrypter::GetKeysFromCdm(
    std::string_view licenseUrl,
    std::vector<uint8_t>& initData,
    const std::vector<uint8_t>& keyId,
    playready_cdm_t* cdm,
    std::map<std::vector<uint8_t>, std::vector<uint8_t>>& cdmKeys)
{
  char* error = nullptr;
  std::unique_ptr<playready_pssh_t, decltype(&playready_pssh_free)> pssh(
      playready_pssh_from_bytes(slice_ref_uint8_t{initData.data(), initData.size()}, &error),
      &playready_pssh_free);

  if (pssh == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to create pssh object: %s", error);
    playready_error_message_free(error);
    error = nullptr;
    return;
  }

  playready_wrm_header_t* wrmHeader = playready_pssh_get_first_wrm_header(pssh.get());

  if (wrmHeader == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to get wrm header: %s", error);
    return;
  }

  std::unique_ptr<playready_session_t, decltype(&playready_session_free)> session(
      playready_cdm_open_session(cdm), &playready_session_free);

  std::unique_ptr<char, decltype(&playready_license_challenge_free)> challenge(
      playready_session_get_license_challenge(session.get(), wrmHeader, &error),
      &playready_license_challenge_free);
  wrmHeader = nullptr;

  if (challenge == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to get challenge: %s", error);
    playready_error_message_free(error);
    error = nullptr;
    return;
  }

  std::vector<std::string> split = STRING::SplitToVec(licenseUrl, '|');
  std::string requestUrl;
  std::string headers;
  std::string requestBody;
  std::string contentType;

  if (split.size() == 4)
  {
    std::string escapedChallenge = std::string{challenge.get()};
    STRING::ReplaceAll(escapedChallenge, "\"", "\\\"");
    requestBody = STRING::URLDecode(split[2]);

    size_t pos = requestBody.find("{XML}");
    if (pos == std::string::npos)
    {
      LOG::LogF(LOGERROR, "Failed to find {XML} marker");
      return;
    }

    requestBody.replace(pos, 5, escapedChallenge);
    requestUrl = split[0];
    headers = STRING::URLDecode(split[1]);
    contentType = "application/json; charset=utf-8";
  }
  else if (split.size() == 1)
  {
    requestBody = challenge.get();
    requestUrl = licenseUrl;
    contentType = "text/xml; charset=utf-8";
  }
  else
  {
    LOG::LogF(LOGERROR, "Unsupported license URL format");
    return;
  }

  CURL::CUrl curl{requestUrl, requestBody};
  curl.AddHeader("Content-Type", contentType);

  if (!headers.empty())
  {
    rapidjson::Document doc;
    rapidjson::ParseResult result = doc.Parse(headers.c_str());

    if (!result)
    {
      LOG::LogF(LOGERROR, "Failed to parse headers");
      return;
    }

    for (const auto& m : doc.GetObject())
    {
      const char* name = m.name.GetString();
      const char* value = m.value.GetString();

      if (name != nullptr && value != nullptr)
      {
        curl.AddHeader(name, value);
      }
    }
  }

  std::string response;
  int statusCode = curl.Open();
  if (statusCode == -1 || statusCode >= 400)
  {
    LOG::Log(LOGERROR, "License server returned failure (HTTP error %i)", statusCode);
    return;
  }

  if (curl.Read(response) != CURL::ReadStatus::IS_EOF)
  {
    LOG::LogF(LOGERROR, "Could not read the license server response");
    return;
  }

  if (split.size() == 4)
  {
    rapidjson::Document doc;
    rapidjson::ParseResult result = doc.Parse(response.c_str());

    if (!result)
    {
      LOG::LogF(LOGERROR, "Failed to parse license response");
      return;
    }

    const rapidjson::Value* value = rapidjson::Pointer(split[3].c_str()).Get(doc);

    if (value == nullptr)
    {
      LOG::LogF(LOGERROR, "Failed to get value using JSON pointer in license response");
      return;
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
    return;
  }

  for (size_t i = 0; i < keys.len; i++)
  {
    std::vector<uint8_t> kid{std::begin(keys.ptr[i].kid.idx), std::end(keys.ptr[i].kid.idx)};
    std::vector<uint8_t> key{keys.ptr[i].ck.ptr, keys.ptr[i].ck.ptr + keys.ptr[i].ck.len};

    cdmKeys[kid] = key;
  }

  playready_keys_free(keys);
}