#include "ClearKeyCdm.h"

#include "CompKodiProps.h"
#include "SrvBroker.h"
#include "decrypters/Helpers.h"
#include "utils/StringUtils.h"

using namespace UTILS;

std::vector<std::string_view> ClearKeyCdm::SelectKeySystems(std::string_view keySystem)
{
  std::vector<std::string_view> keySystems;
  if (keySystem == KS_CLEARKEY)
    keySystems.emplace_back(URN_CLEARKEY);

  return keySystems;
}

bool ClearKeyCdm::OpenDRMSystem(std::string_view licenseURL,
                                const std::vector<uint8_t>& serverCertificate,
                                const uint8_t config)
{
  ClearKeyCdm::GetKeysFromLicenseServer({}, {});
  m_isInitialised = true;
  return true;
}

bool ClearKeyCdm::GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                           const std::vector<uint8_t>& kid)
{
  if (GetKey(kid).has_value())
  {
    return true;
  }

  const auto& keys =
      CSrvBroker::GetKodiProps().GetDrmConfig(std::string(DRM::KS_CLEARKEY)).license.keys;

  bool foundKey = false;

  for (const auto& [hexKid, hexKey] : keys)
  {
    std::vector<uint8_t> newKid, newKey;

    STRING::ToHexBytes(hexKid, newKid);
    STRING::ToHexBytes(hexKey, newKey);

    if (kid == newKid)
    {
      foundKey = true;
    }

    AddKey(std::move(newKid), std::move(newKey));
  }

  return foundKey;
}