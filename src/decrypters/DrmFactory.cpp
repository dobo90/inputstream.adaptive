/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "DrmFactory.h"

#include "CompKodiProps.h"
#include "Helpers.h"
#include "utils/Base64Utils.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#include "clearkey/ClearKeyCdm.h"
#include "playready/PlayReadyCdm.h"
#include "widevine/WidevineCdm.h"

#include <kodi/addon-instance/inputstream/StreamCrypto.h>

using namespace UTILS;

namespace
{
// \brief Fill in missing drm configuration info with defaults
void FillDrmConfigDefaults(std::string_view keySystem, DRM::Config& cfg)
{
  auto& licCfg = cfg.license;

  if (keySystem == DRM::KS_WIDEVINE)
  {
    if (!licCfg.isHttpGetRequest)
    {
      if (!STRING::KeyExists(licCfg.reqHeaders, "Content-Type"))
        licCfg.reqHeaders["Content-Type"] = "application/octet-stream";
    }
  }
  else if (keySystem == DRM::KS_PLAYREADY)
  {
    if (!licCfg.isHttpGetRequest)
    {
      if (!STRING::KeyExists(licCfg.reqHeaders, "Content-Type"))
        licCfg.reqHeaders["Content-Type"] = "text/xml";
    }
  }
  else if (keySystem == DRM::KS_WISEPLAY)
  {
    if (!licCfg.isHttpGetRequest)
    {
      if (!STRING::KeyExists(licCfg.reqHeaders, "Content-Type"))
        licCfg.reqHeaders["Content-Type"] = "application/json";
    }
  }
}
} // unnamed namespace

DRM::Config DRM::CreateDRMConfig(std::string_view keySystem, const ADP::KODI_PROPS::DrmCfg& propCfg)
{
  DRM::Config cfg;

  cfg.keySystem = keySystem;
  cfg.isNewConfig = propCfg.isNewConfig;

  auto& propLicCfg = propCfg.license;
  auto& licCfg = cfg.license;

  licCfg.serverCert = BASE64::Decode(propLicCfg.serverCert);
  licCfg.serverUri = propLicCfg.serverUri;
  licCfg.isHttpGetRequest = propLicCfg.isHttpGetRequest;

  if (!propLicCfg.reqData.empty() && !BASE64::IsValidBase64(propLicCfg.reqData) &&
      propCfg.isNewConfig)
  {
    LOG::LogF(LOGERROR, "The license \"req_data\" parameter must have data encoded as base 64.");
  }
  else
  {
    licCfg.reqData = propLicCfg.reqData;
  }

  licCfg.reqHeaders = propLicCfg.reqHeaders;
  licCfg.reqParams = propLicCfg.reqParams;
  licCfg.wrapper = propLicCfg.wrapper;
  licCfg.unwrapper = propLicCfg.unwrapper;
  licCfg.unwrapperParams = propLicCfg.unwrapperParams;
  licCfg.keys = propLicCfg.keys;

  FillDrmConfigDefaults(keySystem, cfg);

  return cfg;
}

std::shared_ptr<DRM::Cdm> DRM::FACTORY::GetCdm(STREAM_CRYPTO_KEY_SYSTEM keySystem)
{
  if (keySystem == STREAM_CRYPTO_KEY_SYSTEM_WIDEVINE)
  {
    return std::make_shared<WidevineCdm>();
  }
  else if (keySystem == STREAM_CRYPTO_KEY_SYSTEM_PLAYREADY)
  {
    return std::make_shared<PlayreadyCdm>();
  }
  else if (keySystem == STREAM_CRYPTO_KEY_SYSTEM_CLEARKEY)
  {
    return std::make_shared<ClearKeyCdm>();
  }

  return nullptr;
}

std::vector<std::shared_ptr<DRM::Cdm>> DRM::FACTORY::GetCdms()
{
  std::vector<std::shared_ptr<DRM::Cdm>> cdms{std::make_shared<WidevineCdm>(),
                                              std::make_shared<PlayreadyCdm>(),
                                              std::make_shared<ClearKeyCdm>()};

  return cdms;
}
