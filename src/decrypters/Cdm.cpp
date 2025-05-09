#include "Cdm.h"

#include "decrypters/HelperWv.h"
#include "utils/Base64Utils.h"
#include "utils/CurlUtils.h"
#include "utils/log.h"

#include <mutex>

using namespace DRM;
using namespace UTILS;

std::optional<std::vector<uint8_t>> Cdm::GetKey(const std::vector<uint8_t>& kid)
{
  std::shared_lock<std::shared_mutex> lock(m_keysMutex);

  auto it = m_keys.find(kid);
  return it != std::end(m_keys) ? std::make_optional<>(it->second) : std::nullopt;
}

void Cdm::AddKey(std::vector<uint8_t> kid, std::vector<uint8_t> key)
{
  std::unique_lock<std::shared_mutex> lock(m_keysMutex);
  m_keys[kid] = key;
}

// Kept as close as possible to CWVCencSingleSampleDecrypter::SendSessionMessage()
std::string Cdm::SendRequestToLicenseServer(const std::vector<uint8_t>& kid,
                                            const std::vector<uint8_t>& pssh,
                                            uint8_t* challengePtr,
                                            size_t challengeLen)
{
  const DRM::Config::License& licConfig = m_config.license;
  //! @todo: cleanup this var
  const std::vector<uint8_t> challenge(
      reinterpret_cast<const uint8_t*>(challengePtr),
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