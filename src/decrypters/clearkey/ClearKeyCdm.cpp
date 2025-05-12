#include "ClearKeyCdm.h"

#include "utils/StringUtils.h"

using namespace UTILS;

bool ClearKeyCdm::Initialize(const DRM::Config& drmConfig, std::string_view decrypterPath)
{
  m_config = drmConfig;
  return true;
}

bool ClearKeyCdm::GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                           const std::vector<uint8_t>& kid)
{
  if (GetKey(kid).has_value())
  {
    return true;
  }

  const auto& keys = m_config.license.keys;
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
