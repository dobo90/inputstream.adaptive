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
#include "Stream.h"
#include "common/AdaptationSet.h"
#include "common/Representation.h"
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
 * \return The DRMInfo if found, otherwise nullptr
 */
DRM::DRMInfo* GetDRMInfoByKS(std::vector<DRM::DRMInfo>& drmInfos, std::string_view keySystem)
{
  // If no key system is provided its assumend CENC content compatible with any DRM
  auto itDrmInfo = std::find_if(drmInfos.begin(), drmInfos.end(), [&](const DRMInfo& info)
                                { return info.keySystem == keySystem || info.keySystem.empty(); });

  if (itDrmInfo != drmInfos.end())
    return &(*itDrmInfo);

  return nullptr;
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

bool DRM::CDRMEngine::InitializeSession(std::vector<DRM::DRMInfo> drmInfos,
                                        kodi::addon::InputstreamInfo& streamInfo,
                                        PLAYLIST::CRepresentation* repr,
                                        PLAYLIST::CAdaptationSet* adp,
                                        DRM::DRMInfo& initDrmInfo)
{
  if (drmInfos.empty())
    return false;

  if (!Initialize())
    return false;

  LOG::Log(LOGDEBUG, "Initialize crypto session");

  ConfigureClearKey(drmInfos);

  auto& kodiProps = CSrvBroker::GetKodiProps();

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
    return false;
  }

  // Find a compatible DRM info
  auto pDrmInfo = GetDRMInfoByKS(drmInfos, m_keySystem);

  if (!pDrmInfo)
  {
    LOG::LogF(LOGERROR, "The Key System \"%s\" does not match any DRMInfo", m_keySystem.c_str());
    m_status = EngineStatus::DRM_ERROR;
    return false;
  }

  DRM::DRMInfo& drmInfo = *pDrmInfo;
  const auto drmPropCfg = kodiProps.GetDrmConfig(m_keySystem);

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
      drmInfo.initData =
          DRM::PSSH::MakeWidevine({DRM::ConvertKidStrToBytes(drmInfo.defaultKid)}, customInitData);
    }

    if (drmInfo.initData.empty())
      LOG::LogF(LOGERROR, "The custom init PSSH contains no data");
  }

  // If no KID, but init data, extract the KID from init data
  if (!drmInfo.initData.empty() && drmInfo.defaultKid.empty() &&
      DRM::IsValidPsshHeader(drmInfo.initData))
  {
    DRM::PSSH parser;
    if (parser.Parse(drmInfo.initData) && !parser.GetKeyIds().empty())
    {
      LOG::Log(LOGDEBUG, "Default KID parsed from init data");
      drmInfo.defaultKid = STRING::ToLower(STRING::ToHexadecimal(parser.GetKeyIds()[0]));
    }
  }

  if ((drmInfo.initData.empty() && m_keySystem != DRM::KS_CLEARKEY) || drmInfo.defaultKid.empty())
  {
    // Try extract the PSSH/KID from the stream, as last resort because its expensive
    ExtractStreamProtectionData(repr, adp, drmInfo);
  }

  std::shared_ptr<DRM::IDecrypter> drm = GetDrmInstance(m_keySystem);
  if (!drm)
  {
    m_status = EngineStatus::DRM_ERROR;
    LOG::LogF(LOGERROR, "Cannot get the DRM instance for keysystem %s", m_keySystem.c_str());
    GUI::ErrorDialog(GUI::GetLocalizedString(30303));
    return false;
  }

  // Create crypto session
  kodi::addon::StreamCryptoSession cryptoSession;

  streamInfo.SetFeatures(INPUTSTREAM_FEATURE_NONE);
  cryptoSession.SetFlags(STREAM_CRYPTO_FLAG_NONE);

  streamInfo.SetCryptoSession(cryptoSession);

  initDrmInfo = drmInfo;
  return true;
}

bool DRM::CDRMEngine::ConfigureClearKey(std::vector<DRM::DRMInfo>& drmInfos)
{
  const auto& kodiProps = CSrvBroker::GetKodiProps();

  if (!kodiProps.HasDrmConfig(KS_CLEARKEY) || drmInfos.empty())
    return false;

  const ADP::KODI_PROPS::DrmCfg& drmCfg = kodiProps.GetDrmConfig(KS_CLEARKEY);

  // The ClearKey configuration can add (or replace) CK DRMInfo when
  // it finds a custom license uri or keys
  if (drmCfg.license.serverUri.empty() && drmCfg.license.keys.empty())
    return false;

  // Get info from any drm info item, since should be the same
  const std::string defaultKid = drmInfos[0].defaultKid;

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

//! @todo: to remove requirements for CRepresentation CAdaptationSet vars
//! see also todo comment below
void DRM::CDRMEngine::ExtractStreamProtectionData(PLAYLIST::CRepresentation* repr,
                                                  PLAYLIST::CAdaptationSet* adp,
                                                  DRM::DRMInfo& drmInfo)
{
  if (repr->GetContainerType() != PLAYLIST::ContainerType::MP4)
    return;

  LOG::LogF(LOGDEBUG, "Parse MP4 protection data from stream");

  //! @todo: AdaptiveTree* const_cast its not good thing to do, it should be removed
  //! with a code rework, for example by reusing the CStream created by OpenStream
  //! and/or maybe create a way to avoid involve DRMEngine with CStream directly
  SESSION::CStream stream{
      const_cast<adaptive::AdaptiveTree*>(&CSrvBroker::GetResources().GetTree()), adp, repr};

  stream.SetIsEnabled(true);
  stream.m_adStream.start_stream();
  stream.SetAdByteStream(std::make_unique<CAdaptiveByteStream>(&stream.m_adStream));
  stream.SetStreamFile(std::make_unique<AP4_File>(*stream.GetAdByteStream(),
                                                  AP4_DefaultAtomFactory::Instance_, true));
  AP4_Movie* movie{stream.GetStreamFile()->GetMovie()};
  if (!movie)
  {
    LOG::LogF(LOGERROR, "No MOOV atom in stream");
    stream.Disable();
    return;
  }

  AP4_Track* track =
      movie->GetTrack(static_cast<AP4_Track::Type>(stream.m_adStream.GetTrackType()));

  if (track) // Try extract the default KID from tenc / piff mp4 box
  {
    AP4_ProtectedSampleDescription* protSampleDesc =
        static_cast<AP4_ProtectedSampleDescription*>(track->GetSampleDescription(0));

    if (protSampleDesc)
    {
      AP4_ProtectionSchemeInfo* psi = protSampleDesc->GetSchemeInfo();
      if (psi)
      {
        AP4_ContainerAtom* schi = protSampleDesc->GetSchemeInfo()->GetSchiAtom();
        if (schi)
        {
          AP4_TencAtom* tenc =
              AP4_DYNAMIC_CAST(AP4_TencAtom, schi->GetChild(AP4_ATOM_TYPE_TENC, 0));
          if (tenc)
          {
            drmInfo.defaultKid = STRING::ToLower(STRING::ToHexadecimal(tenc->GetDefaultKid(), 16));
          }
          else
          {
            AP4_PiffTrackEncryptionAtom* piff =
                AP4_DYNAMIC_CAST(AP4_PiffTrackEncryptionAtom,
                                 schi->GetChild(AP4_UUID_PIFF_TRACK_ENCRYPTION_ATOM, 0));
            if (piff)
            {
              drmInfo.defaultKid = STRING::ToLower(STRING::ToHexadecimal(piff->GetDefaultKid(), 16));
            }
          }
        }
      }
    }
  }

  if (drmInfo.initData.empty() || drmInfo.defaultKid.empty())
  {
    AP4_Array<AP4_PsshAtom>& pssh{movie->GetPsshAtoms()};
    const uint8_t* currSystemId = DRM::KeySystemToUUID(m_keySystem);

    for (unsigned int i = 0; i < pssh.ItemCount(); ++i)
    {
      AP4_PsshAtom& psshAtom = pssh[i];

      // Try find the system id
      if (std::memcmp(psshAtom.GetSystemId(), currSystemId, 16) == 0)
      {
        const AP4_DataBuffer& dataBuf = psshAtom.GetData();
        const std::vector<uint8_t> psshData{dataBuf.GetData(),
                                            dataBuf.GetData() + dataBuf.GetDataSize()};

        drmInfo.initData = DRM::PSSH::Make(psshAtom.GetSystemId(), {}, psshData);

        if (psshAtom.GetKid(0))
        {
          drmInfo.defaultKid = STRING::ToLower(STRING::ToHexadecimal(pssh[i].GetKid(0), 16));
        }

        break;
      }
    }
  }

  stream.Disable();
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
