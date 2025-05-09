/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DrmEngineDefines.h"

#include <shared_mutex>
#include <string_view>

#include <kodi/c-api/addon-instance/inputstream/stream_crypto.h>

namespace DRM
{
class Cdm
{
public:
  virtual ~Cdm() {}

  /*
   * \brief Return a short name for the decrypter implementation
   */
  virtual const std::string GetName() const { return "Unknown"; }

  virtual bool Initialize(const DRM::Config& drmConfig, std::string_view decrypterPath) = 0;

  virtual std::string_view GetKeySystem() const = 0;

  virtual bool GetKeysFromLicenseServer(const std::vector<uint8_t>& pssh,
                                        const std::vector<uint8_t>& kid) = 0;

  std::optional<std::vector<uint8_t>> GetKey(const std::vector<uint8_t>& kid);

protected:
  void AddKey(std::vector<uint8_t> kid, std::vector<uint8_t> key);
  std::string SendRequestToLicenseServer(const std::vector<uint8_t>& kid,
                                         const std::vector<uint8_t>& pssh,
                                         uint8_t* challengePtr,
                                         size_t challengeLen);

  DRM::Config m_config;
  std::string m_strSession;

private:
  std::map<std::vector<uint8_t>, std::vector<uint8_t>> m_keys;
  std::shared_mutex m_keysMutex;
};
}; // namespace DRM
