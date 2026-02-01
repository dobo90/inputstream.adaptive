/*
 *  Copyright (C) 2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ChooserFixedRes.h"

#include "AdaptationSet.h"
#include "CompKodiProps.h"
#include "CompSettings.h"
#include "ReprSelector.h"
#include "Representation.h"
#include "SrvBroker.h"
#include "utils/log.h"

using namespace CHOOSER;
using namespace PLAYLIST;

CRepresentationChooserFixedRes::CRepresentationChooserFixedRes()
{
  LOG::Log(LOGDEBUG, "[Repr. chooser] Type: Fixed resolution");
}

void CRepresentationChooserFixedRes::Initialize(const ADP::KODI_PROPS::ChooserProps& props)
{
  auto& settings = CSrvBroker::GetSettings();

  m_screenResMax = settings.GetResMax();;

  // Override settings with Kodi/video add-on properties

  if (m_screenResMax.first == 0 ||
      (props.m_resolutionMax.first > 0 && m_screenResMax > props.m_resolutionMax))
  {
    m_screenResMax = props.m_resolutionMax;
  }

  LOG::Log(LOGDEBUG,
           "[Repr. chooser] Configuration\n"
           "Resolution max: %ix%i\n",
           m_screenResMax.first, m_screenResMax.second);
}

void CRepresentationChooserFixedRes::PostInit()
{
  LOG::Log(LOGDEBUG,
           "[Repr. chooser] Stream selection conditions\n"
           "Screen resolution: %ix%i",
           m_screenCurrentWidth, m_screenCurrentHeight);
}

PLAYLIST::CRepresentation* CRepresentationChooserFixedRes::GetNextRepresentation(
    PLAYLIST::CAdaptationSet* adp, PLAYLIST::CRepresentation* currentRep)
{
  if (currentRep && currentRep->isPlayable)
    return currentRep;

  std::pair<int, int> resolution{m_screenResMax};

  if (resolution.first == 0) // Max limit set to "Auto"
    resolution = {m_screenCurrentWidth, m_screenCurrentHeight};

  CRepresentationSelector selector{resolution.first, resolution.second};

  if (adp->GetStreamType() == StreamType::VIDEO)
  {
    return selector.Highest(adp);
  }
  else
  {
    return selector.HighestBw(adp);
  }
}
