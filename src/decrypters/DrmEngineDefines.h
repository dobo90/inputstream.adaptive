/*
 *  Copyright (C) 2025 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#pragma once

#include "utils/CryptoUtils.h"

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

// forwards
namespace DRM
{
class Cdm;
}

namespace DRM
{

struct Config
{
  // The Key System used to initialize DRM
  std::string keySystem;
  // To enable persistent state CDM behaviour
  bool isPersistentStorage{false};
  // Optional parameters to make the CDM key request (CDM specific parameters)
  std::map<std::string, std::string> optKeyReqParams;

  struct License
  {
    // The license server certificate
    std::vector<uint8_t> serverCert;
    // The license server uri
    std::string serverUri;
    // To force an HTTP GET request, instead that POST request
    bool isHttpGetRequest{false};
    // HTTP request headers
    std::map<std::string, std::string> reqHeaders;
    // HTTP parameters to append to the url
    std::string reqParams;
    // Custom license data encoded as base64 to make the HTTP license request
    std::string reqData;
    // License data wrappers
    // Multiple wrappers supported e.g. "base64,json", the name order defines the order
    // in which data will be wrapped, (1) base64 --> (2) url
    std::string wrapper;
    // License data unwrappers
    // Multiple un-wrappers supported e.g. "base64,json", the name order defines the order
    // in which data will be unwrapped, (1) base64 --> (2) json
    std::string unwrapper;
    // License data unwrappers parameters
    std::map<std::string, std::string> unwrapperParams;
    // Clear key's for ClearKey DRM (KID / KEY pair)
    std::map<std::string, std::string> keys;
  };

  // The license configuration
  License license;
  // Specifies if has been parsed the new DRM config ("drm" or "drm_legacy" kodi property)
  //! @todo: to remove when deprecated DRM properties will be removed
  bool isNewConfig{true};
};

constexpr std::string_view ROBUSTNESS_HW_SECDEC = "HW_SECURE_DECODE";

enum class EngineStatus
{
  NONE,
  DRM_ERROR, // Unsupported DRM, or a problem in the initialization of DRM
  DECRYPTER_ERROR, // A DRM decrypter error
};

enum class DRMMediaType
{
  UNKNOWN,
  VIDEO,
  AUDIO
};

struct DRMInfo
{
  std::string keySystem; // Key system, empty if CENC
  std::string robustness;
  std::vector<uint8_t> initData;
  std::string defaultKid;
  std::string licenseServerUri;
  std::vector<uint8_t> serverCert; // Server certificate
};

} // namespace DRM
