/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DrmEngine.h"

#include "CompKodiProps.h"
#include "CompResources.h"
#include "CompSettings.h"
#include "DrmFactory.h"
#include "Helpers.h"
#include "SrvBroker.h"
#include "utils/Base64Utils.h"
#include "utils/GUIUtils.h"
#include "utils/StringUtils.h"
#include "utils/UrlUtils.h"
#include "utils/log.h"

#include <nlohmann/json.hpp>

using njson = nlohmann::json;
using namespace DRM;
using namespace UTILS;

namespace
{
STREAM_CRYPTO_KEY_SYSTEM KSToCryptoKeySystem(std::string_view keySystem)
{
  if (keySystem == DRM::KS_WIDEVINE)
    return STREAM_CRYPTO_KEY_SYSTEM_WIDEVINE;
  else if (keySystem == DRM::KS_WISEPLAY)
    return STREAM_CRYPTO_KEY_SYSTEM_WISEPLAY;
  else if (keySystem == DRM::KS_PLAYREADY)
    return STREAM_CRYPTO_KEY_SYSTEM_PLAYREADY;
  else if (keySystem == DRM::KS_CLEARKEY)
    return STREAM_CRYPTO_KEY_SYSTEM_CLEARKEY;
  else
    return STREAM_CRYPTO_KEY_SYSTEM_NONE;
}

/*!
 * \brief Get a DRMInfo by Key System
 * \param drmInfos The manifest DRM info
 * \param keySystem The Key System to search
 * \param isStrict If true only match the provided key system, otherwise also match empty key system (CENC)
 * \return The DRMInfo if found, otherwise nullptr
 */
DRM::DRMInfo* GetDRMInfoByKS(std::vector<DRM::DRMInfo>& drmInfos, std::string_view keySystem, bool isStrict = false)
{
  // Give priority to entries that explicitly match the requested key system, then try find entries with an empty key system (CENC)
  auto itDrmInfo = std::find_if(drmInfos.begin(), drmInfos.end(), [&](const DRMInfo& info)
                                { return !info.keySystem.empty() && info.keySystem == keySystem; });

  if (itDrmInfo != drmInfos.end())
    return &(*itDrmInfo);

  if (!isStrict)
  {
    itDrmInfo = std::find_if(drmInfos.begin(), drmInfos.end(), [&](const DRMInfo& info)
                             { return info.keySystem.empty(); });

    if (itDrmInfo != drmInfos.end())
      return &(*itDrmInfo);
  }

  return nullptr;
}

std::vector<DRM::DRMInfo> GetDRMInfosByKS(std::vector<DRM::DRMInfo>& drmInfos, std::string_view keySystem)
{
  std::vector<DRM::DRMInfo> ret;
  // Give priority to entries that explicitly match the requested key system, then append entries with an empty key system (CENC)
  for (const auto& info : drmInfos)
  {
    if (!info.keySystem.empty() && info.keySystem == keySystem)
      ret.emplace_back(info);
  }
  for (const auto& info : drmInfos)
  {
    if (info.keySystem.empty())
      ret.emplace_back(info);
  }

  return ret;
}

// \brief Common CENC DRMInfo need to be converted to a specific key system with appropriate PSSH init
void ConvertDRMInfoCENC(DRM::DRMInfo& drmInfo, const std::string& keySystem)
{
  if (!drmInfo.keySystem.empty())
    return; // Not a CENC DRMInfo, no need to convert

  LOG::Log(LOGDEBUG, "Converting Common CENC DRMInfo to %s", keySystem.c_str());
  std::vector<std::vector<uint8_t>> keyIds;

  if (DRM::IsValidPsshHeader(drmInfo.initData))
  {
    DRM::PSSH parser;
    if (parser.Parse(drmInfo.initData))
      keyIds = parser.GetKeyIds();
  }

  if (keyIds.empty())
  {
    if (drmInfo.defaultKid.empty())
    {
      LOG::Log(LOGERROR, "Common CENC DRMInfo does not have a default KID, cannot convert to %s",
               keySystem.c_str());
      return;
    }
    keyIds.emplace_back(DRM::ConvertKidStrToBytes(drmInfo.defaultKid));
  }

  drmInfo.initData = DRM::PSSH::Make(KeySystemToUUID(keySystem), keyIds);
}

/*
 * \brief Get the union of DRM infos from initialization media segment and manifest
 *        by prioritizing media ones since it should be more accurate than manifest
 * \param mediaDrmInfos The DRM infos from the media
 * \param manifestDrmInfos The DRM infos from the manifest
 * \return The union of DRM infos
 */
std::vector<DRM::DRMInfo> DrmInfosUnion(std::vector<DRM::DRMInfo> mediaDrmInfos,
                                        std::vector<DRM::DRMInfo> manifestDrmInfos)
{
  std::vector<DRM::DRMInfo> drmInfos = mediaDrmInfos;

  for (const auto& item : manifestDrmInfos)
  {
    // Match existing entries by keySystem, defaultKid and initData only.
    // Ignore licenseServerUri so that we can copy it from the manifest
    // when the media-provided entry lacks it.
    auto it = std::find_if(drmInfos.begin(), drmInfos.end(),
                           [&](const DRM::DRMInfo& r)
                           {
                             return r.keySystem == item.keySystem &&
                                    r.defaultKid == item.defaultKid &&
                                    r.initData == item.initData;
                           });

    if (it == drmInfos.end())
    {
      drmInfos.emplace_back(item);
    }
    else
    {
      // If the media entry does not provide a licenseServerUri but the
      // manifest does, copy it. Do not overwrite an existing media URI.
      if (it->licenseServerUri.empty() && !item.licenseServerUri.empty())
        it->licenseServerUri = item.licenseServerUri;
    }
  }

  return drmInfos;
}
} // unnamed namespace

bool DRM::CDRMEngine::Initialize()
{
  if (!m_drms.empty())
    return true; // assume as already initialized

  if (m_status == EngineStatus::DRM_ERROR)
    return false; // something wrong with a previous initialization

  //! @todo: to test a way to initialize DRM when manifest is downloaded/parsed
  //! in the hoping to have a more smoother playback transition from unencrypted->to->encrypted periods

  // This is the list of keysystems supported by at least one DRM
  // are ordered by priority where the lower index has higher priority
  // by default ClearKey has the lowest priority since real DRMs should be preferred
  std::vector<std::string_view> keySystemsPrio = {KS_WIDEVINE, KS_PLAYREADY, KS_WISEPLAY, KS_CLEARKEY};

  const auto& kodiProps = CSrvBroker::GetKodiProps();
  // Reorder the keysystems list by using the custom DRM configuration, if any
  for (auto& [ks, cfg] : kodiProps.GetDrmConfigs())
  {
    if (cfg.priority.has_value() && *cfg.priority != 0)
    {
      auto it = std::find(keySystemsPrio.begin(), keySystemsPrio.end(), ks);
      if (it != keySystemsPrio.end())
      {
        keySystemsPrio.erase(it);

        size_t index = *cfg.priority - 1;
        if (index >= keySystemsPrio.size())
          index = keySystemsPrio.size() - 1;

        keySystemsPrio.insert(keySystemsPrio.begin() + index, ks);
      }
    }
  }

  // Get all DRM supported by the platform in use to determine which keysystems are supported
  std::vector<std::shared_ptr<DRM::IDecrypter>> drms = FACTORY::GetDecrypters();

  std::string decrypterPath = CSrvBroker::GetSettings().GetDecrypterPath();
  if (decrypterPath.empty())
  {
    LOG::LogF(LOGERROR,
              "Cannot initialize DrmEngine, no decrypter path set in the add-on settings");
    m_status = EngineStatus::DRM_ERROR;
    return false;
  }

  // Initialize DRMs
  for (auto it = drms.begin(); it != drms.end();)
  {
    if (!(*it)->Initialize()) // Failed to initialize DRM, delete it and go on
    {
      LOG::LogF(LOGERROR, "Unable to initialize %s DRM", (*it)->GetName().c_str());
      it = drms.erase(it);
    }
    else
      ++it;
  }

  // Check what keysystems are supported by DRMs by priority order
  // and so add the supported one to the DRM list
  for (std::string_view ks : keySystemsPrio)
  {
    for (auto& drm : drms)
    {
      if (drm->IsKeySystemSupported(ks))
        m_drms.emplace_back(ks, drm);
    }
  }

  if (m_drms.empty())
  {
    LOG::LogF(LOGWARNING, "No DRM available");
    return false;
  }

  return true;
}

std::optional<std::pair<DRMInfo, std::shared_ptr<DRM::IDecrypter>>> DRM::CDRMEngine::
    InitializeSession(std::vector<DRM::DRMInfo> manifestDrmInfos,
                      std::vector<DRM::DRMInfo> mediaDrmInfos,
                      kodi::addon::InputstreamInfo& streamInfo)
{
  const auto& kodiProps = CSrvBroker::GetKodiProps();
  
  if (kodiProps.GetManifestConfig().ignoreMediaDefaultKid)
    mediaDrmInfos.clear();

  std::vector<DRM::DRMInfo> drmInfos = DrmInfosUnion(mediaDrmInfos, manifestDrmInfos);

  if (drmInfos.empty())
    return std::nullopt;

  if (!Initialize())
    return std::nullopt;

  // Reset status before to start a new initialization
  m_status = EngineStatus::NONE;

  LOG::Log(LOGDEBUG, "Initialize crypto session");

  ConfigureClearKey(drmInfos);

  // This is a kind of hack,
  // some services use manifests (usually SmoothStreaming) with PlayReady DRM only,
  // but they have also a Widevine license server that allow to play same stream with Widevine,
  // this will allow to force change the manifest DRMInfo KeySytem to Widevine and replace the init data
  bool isPRtoWVKeySystem{false};
  if (HasKeySystemSupport(KS_WIDEVINE) && drmInfos.size() == 1 &&
      drmInfos[0].keySystem == KS_PLAYREADY && kodiProps.GetDrmConfigs().size() == 1 &&
      kodiProps.HasDrmConfig(KS_WIDEVINE))
  {
    drmInfos[0].keySystem = KS_WIDEVINE;
    isPRtoWVKeySystem = true;
  }

  if (!SelectDRM(drmInfos))
  {
    LOG::LogF(LOGERROR, "The stream requires an unsupported DRM.");
    GUI::ErrorDialog("The stream requires an unsupported DRM.");
    m_status = EngineStatus::DRM_ERROR;
    return std::nullopt;
  }

  // Get DRMInfo compatible with key system
  std::vector<DRM::DRMInfo> selDrmInfos = GetDRMInfosByKS(drmInfos, m_keySystem);

  if (selDrmInfos.empty())
  {
    LOG::LogF(LOGERROR, "The Key System \"%s\" does not match any DRMInfo", m_keySystem.c_str());
    m_status = EngineStatus::DRM_ERROR;
    return std::nullopt;
  }

  const auto drmPropCfg = kodiProps.GetDrmConfig(m_keySystem);
  std::optional<std::pair<DRMInfo, std::shared_ptr<DRM::IDecrypter>>> session;

  for (size_t drmInfoIdx = 0; drmInfoIdx < selDrmInfos.size(); ++drmInfoIdx)
  {
    DRM::DRMInfo drmInfo = selDrmInfos[drmInfoIdx];

    ConvertDRMInfoCENC(drmInfo, m_keySystem);

    // Set custom init data PSSH provided from property,
    // can allow to initialize a DRM that could be also not specified
    // as supported in the manifest (e.g. missing DASH ContentProtection tags)
    if (!drmPropCfg.initData.empty() || isPRtoWVKeySystem)
    {
      drmInfo.initData.clear();

      std::vector<uint8_t> customInitData = BASE64::Decode(drmPropCfg.initData);

      if (DRM::IsValidPsshHeader(customInitData))
      {
        LOG::Log(LOGDEBUG, "Use custom init PSSH provided by the \"license\" property");
        drmInfo.initData = customInitData;
      }
      else if (m_keySystem == DRM::KS_WIDEVINE) // Try to create a PSSH box, KID should be provided by manifest
      {
        LOG::Log(LOGDEBUG, "Make a Widevine init PSSH to replace PlayReady init data");
        drmInfo.initData = DRM::PSSH::MakeWidevine({DRM::ConvertKidStrToBytes(drmInfo.defaultKid)},
                                                   customInitData);
      }

      if (drmInfo.initData.empty())
        LOG::LogF(LOGERROR, "The custom init PSSH contains no data");
    }

    // If no KID, but init data, extract the KID from init data
    if (!drmInfo.initData.empty() && drmInfo.defaultKid.empty() &&
        DRM::IsValidPsshHeader(drmInfo.initData))
    {
      LOG::Log(LOGDEBUG, "No default KID provided from DRM info, try extracting from init data");
      DRM::PSSH parser;
      if (parser.Parse(drmInfo.initData))
      {
        const auto& keyIds = parser.GetKeyIds();
        if (keyIds.empty())
          LOG::Log(LOGWARNING, "No KID found in PSSH");
        else if (keyIds.size() > 1)
          LOG::Log(LOGWARNING, "Multiple KIDs found in PSSH, cannot be determined the default");
        else
        {
          LOG::Log(LOGDEBUG, "Default KID parsed from init data");
          drmInfo.defaultKid = STRING::ToLower(STRING::ToHexadecimal(keyIds[0]));
        }
      }
    }

    if (drmInfo.defaultKid.empty())
      LOG::Log(LOGWARNING, "Cannot get default KID from DRM info, decryption can fail");

    std::shared_ptr<DRM::IDecrypter> drm = GetDrmInstance(m_keySystem);
    if (!drm)
    {
      m_status = EngineStatus::DRM_ERROR;
      LOG::LogF(LOGERROR, "Cannot get the DRM instance for keysystem %s", m_keySystem.c_str());
      GUI::ErrorDialog(GUI::GetLocalizedString(30303));
      return std::nullopt;
    }

    session = std::make_pair(drmInfo, drm);
    break;
  }

  if (!session)
  {
    LOG::LogF(LOGERROR, "Failed to initialize a DRM session for the stream");
    m_status = EngineStatus::DRM_ERROR;
    return std::nullopt;
  }

  // Create crypto session
  kodi::addon::StreamCryptoSession cryptoSession;

  streamInfo.SetFeatures(INPUTSTREAM_FEATURE_NONE);
  cryptoSession.SetFlags(STREAM_CRYPTO_FLAG_NONE);

  streamInfo.SetCryptoSession(cryptoSession);

  return session;
}

bool DRM::CDRMEngine::ConfigureClearKey(std::vector<DRM::DRMInfo>& drmInfos)
{
  const auto& kodiProps = CSrvBroker::GetKodiProps();

  if (!kodiProps.HasDrmConfig(KS_CLEARKEY) || drmInfos.empty())
    return false;

  const ADP::KODI_PROPS::DrmCfg& drmCfg = kodiProps.GetDrmConfig(KS_CLEARKEY);

  // The ClearKey configuration can add (or replace) CK DRMInfo when
  // it finds a custom license uri or keys
  const bool isCustomLicense = !drmCfg.license.serverUri.empty() || !drmCfg.license.keys.empty();

  if (!isCustomLicense)
  {
    // If exists DRMInfo with CENC keysystem, copy the Kid to the CK DRMInfo
    DRMInfo* cencDrmInfo = GetDRMInfoByKS(drmInfos, "", true);
    DRMInfo* ckDrmInfo = GetDRMInfoByKS(drmInfos, KS_CLEARKEY, true);

    if (cencDrmInfo && ckDrmInfo)
    {
      ckDrmInfo->defaultKid = cencDrmInfo->defaultKid;
      // Delete CENC to prevent using it
      drmInfos.erase(std::remove_if(drmInfos.begin(), drmInfos.end(), [](const DRM::DRMInfo& info)
                                    { return info.keySystem.empty(); }),
                     drmInfos.end());
    }

    return false;
  }

  // Copy common info from the first DRMInfo with a default KID
  auto it = std::find_if(drmInfos.begin(), drmInfos.end(),
                         [](const DRM::DRMInfo& s) { return !s.defaultKid.empty(); });
  DRMInfo& drmInfoBase = (it != drmInfos.end()) ? *it : drmInfos.front();

  const std::string defaultKid = drmInfoBase.defaultKid;

  if (kodiProps.GetDrmConfigs().size() == 1) // Single config (CK)
  {
    // Delete all the DRMInfo, so you can force CK even if the manifest uses a different DRM
    drmInfos.clear();
  }
  else // More configs, behavior based on "priority" its needed to preserve DRMInfos
  {
    drmInfos.erase(std::remove_if(drmInfos.begin(), drmInfos.end(), [](const DRM::DRMInfo& info)
                                  { return info.keySystem == KS_CLEARKEY; }),
                   drmInfos.end());
  }

  std::string licenseUri;

  if (drmCfg.license.keys.empty())
  {
    licenseUri = drmCfg.license.serverUri;
  }
  else // Create license uri with jwkSets
  {
    njson jData;
    njson jwkSets = njson::array();

    for (auto& [kid, key] : drmCfg.license.keys)
    {
      const std::string kVal =
          BASE64::UrlSafeEncode(BASE64::Encode(DRM::ConvertKidStrToBytes(key), false));
      const std::string kidVal =
          BASE64::UrlSafeEncode(BASE64::Encode(DRM::ConvertKidStrToBytes(kid), false));

      njson jwkSet;
      jwkSet["k"] = kVal;
      jwkSet["kid"] = kidVal;
      jwkSet["kty"] = "oct";
      jwkSets.push_back(jwkSet);
    }

    jData["keys"] = jwkSets;
    jData["type"] = "temporary";

    const std::string dumps = jData.dump(-1, ' ', false, njson::error_handler_t::ignore);

    licenseUri = "data:application/json;base64," + BASE64::Encode(dumps);
  }

  DRM::DRMInfo drmInfo;
  drmInfo.keySystem = KS_CLEARKEY;
  drmInfo.defaultKid = defaultKid;
  drmInfo.licenseServerUri = licenseUri;
  drmInfos.emplace_back(drmInfo);

  return true;
}

bool DRM::CDRMEngine::SelectDRM(std::vector<DRM::DRMInfo>& drmInfos)
{
  if (!m_keySystem.empty())
    return true;

  // Iterate supported DRM Key System's to find a match with the drmInfo's,
  // the supported DRM's are ordered by priority
  // the lower index have the higher priority
  for (auto& drm : m_drms)
  {
    const DRM::DRMInfo* drmInfo = GetDRMInfoByKS(drmInfos, drm.keySystem);

    if (drmInfo)
    {
      m_keySystem = drm.keySystem;
      LOG::LogF(LOGDEBUG, "Selected DRM key system: %s", m_keySystem.c_str());
      break;
    }
  }

  return !m_keySystem.empty();
}

bool DRM::CDRMEngine::HasKeySystemSupport(std::string_view keySystem) const
{
  return std::any_of(m_drms.cbegin(), m_drms.cend(),
                     [&keySystem](const DRMInstance& a) { return a.keySystem == keySystem; });
}

std::shared_ptr<DRM::IDecrypter> DRM::CDRMEngine::GetDrmInstance(std::string_view ks) const
{
  auto it = std::find_if(m_drms.cbegin(), m_drms.cend(),
                         [&ks](const DRMInstance& d) { return d.keySystem == ks; });
  return it != m_drms.cend() ? it->drm : nullptr;
}

void DRM::CDRMEngine::Dispose()
{
  LOG::Log(LOGDEBUG, "Dispose DRM Engine");
  m_drms.clear();
}
