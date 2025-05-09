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

SResult CreateDRM(std::string_view keySystem, std::shared_ptr<DRM::Cdm>& drm)
{
  std::string decrypterPath = CSrvBroker::GetSettings().GetDecrypterPath();
  if (decrypterPath.empty())
  {
    LOG::LogF(LOGERROR, "No decrypter path set in the add-on settings");
    return SResult::Error(GUI::GetLocalizedString(30302));
  }

  drm = DRM::FACTORY::GetCdm(KSToCryptoKeySystem(keySystem));

  if (!drm)
  {
    LOG::LogF(LOGERROR, "Unable to create the DRM decrypter");
    return SResult::Error(GUI::GetLocalizedString(30303));
  }

  drm->SetLibraryPath(decrypterPath);

  if (!drm->Initialize())
  {
    drm = nullptr;
    LOG::LogF(LOGERROR, "Unable to initialize the DRM decrypter");
    return SResult::Error(GUI::GetLocalizedString(30303));
  }

  return SResultCode::OK;
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

void DRM::CDRMEngine::Initialize()
{
  // Build the list of the supported DRM's by KeySystem,
  // the list depends on the operative system used,
  // the order of addition also defines the DRM priority (always prefer real DRM's over ClearKey)
  // in future could be improved to take in account also the DRM capability on the target system

  // Widevine currently always preferred as first because on android can reach 4k on L1 devices
  m_supportedKs.emplace_back(KS_WIDEVINE);
  // TODO: m_supportedKs.emplace_back(KS_PLAYREADY);
  m_supportedKs.emplace_back(KS_CLEARKEY);

  // Sort key systems based on priorities
  const auto& kodiProps = CSrvBroker::GetKodiProps();

  for (auto& [ks, cfg] : kodiProps.GetDrmConfigs())
  {
    if (cfg.priority.has_value() && *cfg.priority != 0)
    {
      auto it = std::find(m_supportedKs.begin(), m_supportedKs.end(), ks);
      if (it != m_supportedKs.end())
      {
        m_supportedKs.erase(it);

        size_t index = *cfg.priority - 1;
        if (index >= m_supportedKs.size())
          index = m_supportedKs.size() - 1;

        m_supportedKs.insert(m_supportedKs.begin() + index, ks);
      }
    }
  }
}

bool DRM::CDRMEngine::InitializeSession(std::vector<DRM::DRMInfo> drmInfos,
                                        kodi::addon::InputstreamInfo& streamInfo,
                                        PLAYLIST::CRepresentation* repr,
                                        PLAYLIST::CAdaptationSet* adp,
                                        bool canCleanupSessions,
                                        DRM::DRMInfo& initDrmInfo)
{
  if (drmInfos.empty())
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

  // Create the DRM decrypter
  if (!STRING::KeyExists(m_drms, m_keySystem))
  {
    //! @todo: to test a way to preinitialize DRM when manifest is downloaded/parsed
    //! in the hoping to have a more smoother playback transition
    //! this can be tested with multiperiods video where first period is unencrypted and second one DRM crypted
    std::shared_ptr<DRM::Cdm> drm;
    SResult ret = CreateDRM(m_keySystem, drm);
    if (ret.IsFailed())
    {
      m_status = EngineStatus::DRM_ERROR;
      LOG::LogF(LOGERROR, "%s", ret.Message().c_str());
      GUI::ErrorDialog(ret.Message());
      return false;
    }
    m_drms.emplace(m_keySystem, drm);
  }

  auto cdm = GetCdm();
  if (!cdm->IsInitialised())
  {
    DRM::Config drmCfg = DRM::CreateDRMConfig(m_keySystem, drmPropCfg);
    const SResult ret = cdm->OpenDRMSystem(drmCfg);

    if (ret.IsFailed())
    {
      LOG::LogF(LOGERROR, "Failed to open the DRM");
      m_status = EngineStatus::DRM_ERROR;
      GUI::ErrorDialog(ret.Message());
      return false;
    }
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
  for (const std::string& ks : m_supportedKs)
  {
    const DRM::DRMInfo* drmInfo = GetDRMInfoByKS(drmInfos, ks);

    if (drmInfo)
    {
      m_keySystem = ks;
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
  return std::find(m_supportedKs.cbegin(), m_supportedKs.cend(), keySystem) != m_supportedKs.cend();
}

void DRM::CDRMEngine::Dispose()
{
  LOG::Log(LOGDEBUG, "Dispose DRM Engine");
  m_drms.clear();
}

std::shared_ptr<DRM::Cdm> DRM::CDRMEngine::GetCdm() const
{
  auto it = m_drms.find(m_keySystem);

  return it != std::end(m_drms) ? it->second : nullptr;
}