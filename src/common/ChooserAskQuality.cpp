/*
 *  Copyright (C) 2022 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "ChooserAskQuality.h"

#include "AdaptationSet.h"
#include "CompSettings.h"
#include "CompKodiProps.h"
#include "ReprSelector.h"
#include "Representation.h"
#include "SrvBroker.h"
#include "Period.h"
#include "utils/GUIUtils.h"
#include "utils/StringUtils.h"
#include "utils/Utils.h"
#include "utils/log.h"

#include <vector>

using namespace CHOOSER;
using namespace PLAYLIST;
using namespace UTILS;

namespace
{
std::string CovertFpsToString(float value)
{
  std::string str{STRING::Format("%.3f", value)};
  std::size_t found = str.find_last_not_of("0");
  if (found != std::string::npos)
    str.erase(found + 1);

  if (str.back() == '.')
    str.pop_back();

  return str;
}

std::string CreateStreamName(const CRepresentation* repr)
{
  std::string hdrType;
  if (CODEC::Contains(repr->GetCodecs(), CODEC::FOURCC_DVH1) ||
      CODEC::Contains(repr->GetCodecs(), CODEC::FOURCC_DVHE))
  {
    hdrType = "DV";
  }
  else
  {
    const ColorTRC colorTrc = repr->GetColorTRC();
    if (colorTrc == ColorTRC::SMPTE2084)
      hdrType = "HDR10";
    else if (colorTrc == ColorTRC::ARIB_STD_B67)
      hdrType = "HLG";
  }

  float fps{static_cast<float>(repr->GetFrameRate())};
  if (fps > 0 && repr->GetFrameRateScale() > 0)
    fps /= repr->GetFrameRateScale();

  std::string quality = "(";
  if (repr->GetWidth() > 0 && repr->GetHeight() > 0)
    quality += STRING::Format("%ix%i, ", repr->GetWidth(), repr->GetHeight());
  if (fps > 0)
    quality += STRING::Format("%s fps, ", CovertFpsToString(fps).c_str());

  quality += STRING::Format("%u Kbps)", repr->GetBandwidth() / 1000);

  const std::unordered_map<std::string, std::string> phValues = {
      {"codec", CODEC::GetVideoDesc(repr->GetCodecs())},
      {"hdr-type", hdrType},
      {"quality", quality}};

  return STRING::ReplacePlaceholders(GUI::GetLocalizedString(30232), phValues, '{', '}');
}
} // unnamed namespace

CRepresentationChooserAskQuality::CRepresentationChooserAskQuality()
{
  LOG::Log(LOGDEBUG, "[Repr. chooser] Type: Ask quality");
}

void CRepresentationChooserAskQuality::Initialize(const ADP::KODI_PROPS::ChooserProps& props)
{
  auto& settings = CSrvBroker::GetSettings();
  m_resRangeLimit = settings.GetResRangeLimit();

  LOG::Log(LOGDEBUG,
           "[Repr. chooser] Configuration\n"
           "Resolution range limit: %ix%i",
           m_resRangeLimit.first, m_resRangeLimit.second);
}

void CRepresentationChooserAskQuality::PostInit()
{
}

PLAYLIST::CAdaptationSet* CHOOSER::CRepresentationChooserAskQuality::GetPreferredVideoAdpSet(
    PLAYLIST::CPeriod* period, PLAYLIST::CAdaptationSet* adpSetPreferred)
{
  // If the dialog box has already been displayed, then the period has changed
  if (m_isDialogShown)
  {
    // Try find the adaptation set with same codec or fallback to the preferred
    PLAYLIST::CAdaptationSet* selAdpSet = adpSetPreferred;
    for (auto& adpSet : period->GetAdaptationSets())
    {
      if (adpSet->GetStreamType() != StreamType::VIDEO || adpSet->GetRepresentations().size() == 0)
        continue;

      if (CODEC::GetVideoDesc(adpSet->GetCodecs()) != m_selectedVideoCodecDesc)
        continue;
    
      selAdpSet = adpSet.get();
      break;
    }

    return selAdpSet;
  }
  else
  {
    CRepresentationSelector selector{m_screenCurrentWidth, m_screenCurrentHeight};
    std::vector<std::string> entries;
    std::vector<std::pair<CAdaptationSet*, CRepresentation*>> entriesOjb;
    int preselIndex{GUI::DIALOG_NO_VALUE}; // Preselected list item
    int selIndex{0};

    for (auto& adpSet : period->GetAdaptationSets())
    {
      if (adpSet->GetStreamType() != StreamType::VIDEO || adpSet->GetRepresentations().size() == 0)
        continue;

      if (m_resRangeLimit.first != 0) // Show only the nearest resolution to the preference
      {
        CRepresentationSelector selector{m_resRangeLimit.first, m_resRangeLimit.second};
        CRepresentation* nearestRep = selector.Nearest(adpSet.get());

        if (nearestRep)
        {
          entries.emplace_back(CreateStreamName(nearestRep));
          entriesOjb.emplace_back(adpSet.get(), nearestRep);

          if (adpSet.get() == adpSetPreferred)
            preselIndex = static_cast<int>(entries.size()) - 1;
        }
      }
      else // Show all resolutions
      {
        CRepresentation* bestRep{nullptr};

        // preferred adaptation set, in order to have a preferred codec for multi-codec manifests
        if (adpSetPreferred == adpSet.get())
        {
          bestRep = selector.Highest(adpSetPreferred);
        }

        for (auto& repr : adpSet->GetRepresentations())
        {
          if (!repr->isPlayable)
            continue;

        entries.emplace_back(CreateStreamName(repr.get()));
        entriesOjb.emplace_back(adpSet.get(), repr.get());

        if (repr.get() == bestRep)
          preselIndex = static_cast<int>(entries.size()) - 1;
        }
      }
    }

    if (entries.size() > 1)
    {
      selIndex = GUI::SelectDialog(GUI::GetLocalizedString(30231), entries, preselIndex, 10000);
    }

    if (!entries.empty())
    {
      if (selIndex == GUI::DIALOG_NO_VALUE) // has been cancelled by the user
        selIndex = preselIndex == GUI::DIALOG_NO_VALUE ? 0 : preselIndex;

      CAdaptationSet* selAdpSet{entriesOjb[selIndex].first};
      CRepresentation* selRep{entriesOjb[selIndex].second};

      m_selectedResWidth = selRep->GetWidth();
      m_selectedResHeight = selRep->GetHeight();
      m_selectedBandwidth = selRep->GetBandwidth();
      // Convert codec to description, as the codec string may be slightly different
      // when using ISO BMFF format, and the comparison may fail
      m_selectedVideoCodecDesc = CODEC::GetVideoDesc(selAdpSet->GetCodecs());

      m_isDialogShown = true;

      LogDetails(nullptr, selRep);
      return selAdpSet;
    }
  }
  return adpSetPreferred;
}

PLAYLIST::CRepresentation* CRepresentationChooserAskQuality::GetNextRepresentation(
    PLAYLIST::CAdaptationSet* adp, PLAYLIST::CRepresentation* currentRep)
{
  if (currentRep && currentRep->isPlayable)
    return currentRep;

  CRepresentationSelector selector{m_selectedResWidth, m_selectedResHeight};

  if (adp->GetStreamType() != StreamType::VIDEO)
  {
    return selector.HighestBw(adp);
  }

  return selector.NearestBw(adp, m_selectedBandwidth);
}
