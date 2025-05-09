/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "DrmEngineDefines.h"
#include "IDecrypter.h"
#include "utils/CryptoUtils.h"

#ifdef INPUTSTREAM_TEST_BUILD
#include "test/KodiStubs.h"
#else
#include <kodi/addon-instance/Inputstream.h>
#endif

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DRM
{

class ATTR_DLL_LOCAL CDRMEngine
{
public:
  CDRMEngine() = default;
  virtual ~CDRMEngine() = default;

  /*!
   * \brief Initialize a DRM (if needed), then create or reuse a session.
   * \param drmInfos The DRM info provided by a manifest
   * \param streamInfo The InputstreamInfo where set the DRM configuration
   * \return The DRM session if has success, otherwise nullptr
   */
  std::optional<std::pair<DRMInfo, std::shared_ptr<DRM::IDecrypter>>> InitializeSession(
      std::vector<DRM::DRMInfo> manifestDrmInfos,
      std::vector<DRM::DRMInfo> mediaDrmInfos,
      kodi::addon::InputstreamInfo& streamInfo);

  /*!
   * \brief Get the current engine status. The state can change after each call to InitializeSession.
   * \return The current status
   */
  EngineStatus GetStatus() const { return m_status; }

  /*!
   * \brief Unload DRM engine resources.
   */
  void Dispose();

private:
  /*!
   * \brief Initialize the DRM engine.
   */
  bool Initialize();

  /*!
   * \brief Configure DRM ClearKey, by replacing manifest DRM info when needed.
   */
  bool ConfigureClearKey(std::vector<DRM::DRMInfo>& drmInfos);

  /*!
   * \brief Select and set a DRM compatible with a DRM info.
   * \param drmInfos The manifest DRM info
   * \return True if a match was found, otherwise false
   */
  bool SelectDRM(std::vector<DRM::DRMInfo>& drmInfos);

  // \brief Check if a Key System is supported
  bool HasKeySystemSupport(std::string_view keySystem) const;

  // \brief Get a DRM instance for the specified key system, it will return nullptr if not found
  std::shared_ptr<DRM::IDecrypter> GetDrmInstance(std::string_view ks) const;

  std::string m_keySystem; // Choosen key system
  std::vector<DRMInstance> m_drms;

  EngineStatus m_status{EngineStatus::NONE};
};

} // namespace DRM
