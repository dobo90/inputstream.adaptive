#include "WidevineCdm.h"

#include "decrypters/HelperWv.h"
#include "utils/Base64Utils.h"
#include "utils/CurlUtils.h"
#include "utils/FileUtils.h"
#include "utils/ResultType.h"
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

SResult WidevineCdm::OpenDRMSystem(const DRM::Config& config)
{
  char* error = nullptr;
  const std::string wvdPath = FILESYS::PathCombine(m_libraryPath, "device.wvd");
  widevine_cdm_free(m_cdm);
  m_cdm = widevine_cdm_create_from_wvd(wvdPath.c_str(), &error);

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to instantiate cdm: %s", error);
    auto ret = SResult::Error(error);
    widevine_error_message_free(error);
    error = nullptr;
    return ret;
  }

  m_config = config;

  return SResult::Ok();
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

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "m_cdm is not initialized");
    return false;
  }

  widevine_cdm_session_t* session = widevine_cdm_open(m_cdm);

  if (!m_config.license.serverCert.empty())
  {
    session = widevine_cdm_session_set_service_certificate(
        session,
        slice_ref_uint8_t{m_config.license.serverCert.data(), m_config.license.serverCert.size()},
        &error);

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

  const std::string licenseMessage = SendSessionMessage(kid, pssh, challenge.ptr, challenge.len);
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
                                            const std::vector<uint8_t>& pssh,
                                            uint8_t* challengePtr,
                                            size_t challengeLen)
{
  const DRM::Config::License& licConfig = m_config.license;
  //! @todo: cleanup this var
  std::vector<uint8_t> challenge(reinterpret_cast<const uint8_t*>(challengePtr),
                                 reinterpret_cast<const uint8_t*>(challengePtr + challengeLen));
  std::string reqData;

  if (!licConfig.isHttpGetRequest) // Make HTTP POST request
  {
    if (licConfig.reqData.empty()) // By default add raw challenge
    {
      reqData.assign(challenge.cbegin(), challenge.cend());
    }
    else
    {
      if (BASE64::IsValidBase64(licConfig.reqData))
        reqData = BASE64::DecodeToStr(licConfig.reqData);
      else //! @todo: this fallback as plain text must be removed when the deprecated DRM properties are removed, and so replace it to return error
        reqData = licConfig.reqData;

      // Some services have a customized license server that require data to be wrapped with their formats (e.g. JSON).
      // Here we provide a built-in way to customize the license data to be sent, this avoid force add-ons to integrate
      // an HTTP server proxy to manage the license data request/response, and so use Kodi properties to set wrappers.
      if (!DRM::WvWrapLicense(reqData, challenge, m_strSession, kid, pssh, licConfig.wrapper,
                              m_config.isNewConfig))
      {
        return {};
      }
    }
  }

  std::string url = licConfig.serverUri;
  DRM::TranslateLicenseUrlPh(url, challenge, m_config.isNewConfig);

  CURL::CUrl cUrl{url, reqData};
  cUrl.AddHeaders(licConfig.reqHeaders);

  const int statusCode = cUrl.Open();

  if (statusCode == -1 || statusCode >= 400)
  {
    LOG::Log(LOGERROR, "License server returned failure (HTTP error %i)", statusCode);
    return {};
  }

  std::string respData;
  if (cUrl.Read(respData) == CURL::ReadStatus::ERROR)
  {
    LOG::LogF(LOGERROR, "Cannot read license server response");
    return {};
  }

  const std::string respContentType = cUrl.GetResponseHeader("Content-Type");

  // Unwrap license response
  if (!licConfig.unwrapper.empty())
  {
    std::string unwrappedData;
    int hdcpResLimit;
    uint16_t hdcpVerLimit;
    // Some services have a customized license server that require data to be wrapped with their formats (e.g. JSON).
    // Here we provide a built-in way to unwrap the license data received, this avoid force add-ons to integrate
    // a HTTP server proxy to manage the license data request/response, and so use Kodi properties to set wrappers.
    if (!DRM::WvUnwrapLicense(licConfig.unwrapper, licConfig.unwrapperParams, respContentType,
                              respData, unwrappedData, hdcpResLimit, hdcpVerLimit))
    {
      return {};
    }
    respData = unwrappedData;
  }

  return respData;
}