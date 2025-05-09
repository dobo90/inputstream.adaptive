/*
 *  Copyright (C) 2023 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "CompSettings.h"
#include "utils/StringUtils.h"
#include "utils/log.h"

#ifndef INPUTSTREAM_TEST_BUILD
#include <kodi/addon-instance/Inputstream.h>
#include <kodi/Filesystem.h>
#endif

using namespace ADP::SETTINGS;
using namespace UTILS;

bool ADP::SETTINGS::CCompSettings::GetCacheBool(const std::string& key) const
{
  std::lock_guard lock(m_settingsMtx);
  auto it = m_cacheBool.find(key);
  if (it != m_cacheBool.end())
    return it->second;

  bool v = kodi::addon::GetSettingBoolean(key);
  m_cacheBool.emplace(key, v);
  return v;
}

StreamSelMode ADP::SETTINGS::CCompSettings::GetStreamSelMode() const
{
  const std::string mode = kodi::addon::GetSettingString("adaptivestream.streamselection.mode");
  if (mode == "manual-v")
    return StreamSelMode::MANUAL_VIDEO;
  if (mode == "manual-av")
    return StreamSelMode::MANUAL;

  LOG::Log(LOGERROR, "Unknown value for \"adaptivestream.streamselection.mode\" setting");
  return StreamSelMode::AUTO;
}

std::string ADP::SETTINGS::CCompSettings::GetChooserType() const
{
  return kodi::addon::GetSettingString("adaptivestream.type");
}

std::pair<int, int> ADP::SETTINGS::CCompSettings::GetResMax() const
{
  std::pair<int, int> val;
  if (!STRING::GetMapValue(RES_CONV_LIST, kodi::addon::GetSettingString("adaptivestream.res.max"),
                           val))
    LOG::Log(LOGERROR, "Unknown value for \"adaptivestream.res.max\" setting");

  return val;
}

std::pair<int, int> ADP::SETTINGS::CCompSettings::GetResRangeLimit() const
{
  std::pair<int, int> val;
  if (!STRING::GetMapValue(RES_CONV_LIST,
                           kodi::addon::GetSettingString("adaptivestream.res.rangelimit"), val))
    LOG::Log(LOGERROR, "Unknown value for \"adaptivestream.res.rangelimit\" setting");

  return val;
}

bool ADP::SETTINGS::CCompSettings::IsBandwidthInitAuto() const
{
  return kodi::addon::GetSettingBoolean("adaptivestream.bandwidth.init.auto");
}

uint32_t ADP::SETTINGS::CCompSettings::GetBandwidthInit() const
{
  return static_cast<uint32_t>(kodi::addon::GetSettingInt("adaptivestream.bandwidth.init") * 1000);
}

uint32_t ADP::SETTINGS::CCompSettings::GetBandwidthMin() const
{
  return static_cast<uint32_t>(kodi::addon::GetSettingInt("adaptivestream.bandwidth.min") * 1000);
}

uint32_t ADP::SETTINGS::CCompSettings::GetBandwidthMax() const
{
  return static_cast<uint32_t>(kodi::addon::GetSettingInt("adaptivestream.bandwidth.max") * 1000);
}

bool ADP::SETTINGS::CCompSettings::IsIgnoreScreenRes() const
{
  return kodi::addon::GetSettingBoolean("overrides.ignore.screen.res");
}

bool ADP::SETTINGS::CCompSettings::IsIgnoreScreenResChange() const
{
  return kodi::addon::GetSettingBoolean("overrides.ignore.screen.res.change");
}

std::string ADP::SETTINGS::CCompSettings::GetChooserTestMode() const
{
  return kodi::addon::GetSettingString("adaptivestream.test.mode");
}

int ADP::SETTINGS::CCompSettings::GetChooserTestSegsVideo() const
{
  return kodi::addon::GetSettingInt("adaptivestream.test.segments.video");
}

int ADP::SETTINGS::CCompSettings::GetChooserTestSegsAudio() const
{
  return kodi::addon::GetSettingInt("adaptivestream.test.segments.audio");
}

int ADP::SETTINGS::CCompSettings::GetMediaType() const
{
  return kodi::addon::GetSettingInt("MEDIATYPE");
}

std::string ADP::SETTINGS::CCompSettings::GetDecrypterPath() const
{
  return kodi::vfs::TranslateSpecialProtocol(kodi::addon::GetSettingString("DECRYPTERPATH"));
}

bool ADP::SETTINGS::CCompSettings::IsDebugLicense() const
{
  return GetCacheBool("debug.save.license");
}

bool ADP::SETTINGS::CCompSettings::IsDebugManifest() const
{
  return GetCacheBool("debug.save.manifest");
}

bool ADP::SETTINGS::CCompSettings::IsDebugVerbose() const
{
  return GetCacheBool("debug.verbose");
}
