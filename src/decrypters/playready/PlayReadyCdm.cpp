#include "PlayReadyCdm.h"

#include "playready.h"
#include "utils/FileUtils.h"
#include "utils/ResultType.h"
#include "utils/log.h"

using namespace UTILS;

PlayreadyCdm::~PlayreadyCdm()
{
  playready_cdm_free(m_cdm);
  m_cdm = nullptr;
}

SResult PlayreadyCdm::OpenDRMSystem(const DRM::Config& config)
{
  char* error = nullptr;
  const std::string prdPath = FILESYS::PathCombine(m_libraryPath, "device.prd");
  playready_cdm_free(m_cdm);
  m_cdm = playready_cdm_create_from_prd(prdPath.c_str(), &error);

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "Failed to instantiate cdm: %s", error);
    auto ret = SResult::Error(error);
    playready_error_message_free(error);
    error = nullptr;
    return ret;
  }

  m_config = config;

  return SResultCode::OK;
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

  if (m_cdm == nullptr)
  {
    LOG::LogF(LOGERROR, "cdm is not initialized");
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

  std::string response = SendRequestToLicenseServer(
      kid, pssh, reinterpret_cast<uint8_t*>(challenge.get()), strlen(challenge.get()));

  if (response.empty())
  {
    LOG::LogF(LOGERROR, "Failed to get license response");
    return false;
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
