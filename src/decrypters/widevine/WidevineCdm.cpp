#include "WidevineCdm.h"

#include "utils/FileUtils.h"
#include "utils/ResultType.h"
#include "utils/log.h"
#include "widevine.h"

using namespace UTILS;

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

  return SResultCode::OK;
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

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "cdm is not initialized");
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

  if (!m_config.license.serverCert.empty())
  {
    session = widevine_cdm_session_set_service_certificate(
        session,
        slice_ref_uint8_t{m_config.license.serverCert.data(), m_config.license.serverCert.size()},
        &error);

    if (session == nullptr)
    {
      LOG::LogF(LOGERROR, "Failed to set server certificate: %s", error);
      widevine_error_message_free(error);
      error = nullptr;
      widevine_pssh_free(wvPssh);
      wvPssh = nullptr;
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

  const std::string licenseMessage =
      SendRequestToLicenseServer(kid, pssh, challenge.ptr, challenge.len);
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