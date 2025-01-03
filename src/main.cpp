/*
 *  Copyright (C) 2016 peak3d (http://www.peak3d.de)
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "main.h"

#include "CompResources.h"
#include "CompKodiProps.h"
#include "SrvBroker.h"
#include "Stream.h"
#include "utils/GUIUtils.h"
#include "utils/ThreadPool.h"
#include "utils/log.h"

extern "C" void kodi_addon_logger_rs_initialize(
    KODI_ADDON_BACKEND_HDL kodiBase,
    void (*addon_log_msg)(const KODI_ADDON_BACKEND_HDL hdl, const int loglevel, const char* msg));

using namespace PLAYLIST;
using namespace SESSION;

CInputStreamAdaptive::CInputStreamAdaptive(const kodi::addon::IInstanceInfo& instance)
  : CInstanceInputStream(instance)
{
  CSrvBroker::GetInstance()->Initialize();
}

ADDON_STATUS CInputStreamAdaptive::CreateInstance(const kodi::addon::IInstanceInfo& instance,
                                                  KODI_ADDON_INSTANCE_HDL& hdl)
{
  return ADDON_STATUS_NOT_IMPLEMENTED;
}

bool CInputStreamAdaptive::Open(const kodi::addon::InputstreamProperty& props)
{
  LOG::Log(LOGDEBUG, "Open()");

  CSrvBroker::GetInstance()->InitStage1(props.GetProperties());

  m_session = std::make_shared<CSession>();

  SResult ret = m_session->Initialize(props.GetURL());
  if (ret.IsFailed())
  {
    LOG::Log(LOGERROR, ret.Message().c_str());
    UTILS::GUI::ErrorDialog(ret.Message());
    m_session = nullptr;
    return false;
  }
  return true;
}

void CInputStreamAdaptive::Close(void)
{
  LOG::Log(LOGDEBUG, "Close()");
  m_session = nullptr;
  UTILS::THREAD::GlobalThreadPool.Reset();
  CSrvBroker::GetInstance()->Deinitialize();
}

bool CInputStreamAdaptive::GetStreamIds(std::vector<unsigned int>& ids)
{
  LOG::Log(LOGDEBUG, "GetStreamIds()");

  if (!m_session)
    return false;

  const unsigned int streamCount = m_session->GetStreamCount();
  if (streamCount > INPUTSTREAM_MAX_STREAM_COUNT)
  {
    LOG::LogF(
        LOGWARNING,
        "Exceeded the maximum limit of %i streams. %u streams have been excluded from playback",
        INPUTSTREAM_MAX_STREAM_COUNT, streamCount - INPUTSTREAM_MAX_STREAM_COUNT);
  }

  for (unsigned int i(0); i < INPUTSTREAM_MAX_STREAM_COUNT && i < streamCount; ++i)
  {
    CStream* stream = m_session->GetStream(i);
    if (!stream)
    {
      LOG::LogF(LOGERROR, "Cannot get the stream from sid %u", i);
      continue;
    }

    if (stream->m_isValid &&
        (m_session->GetMediaTypeMask() &
         static_cast<uint8_t>(1) << static_cast<int>(stream->m_adStream.GetStreamType())))
    {
      if (m_session->GetMediaTypeMask() != 0xFF)
      {
        const CRepresentation* rep = stream->m_adStream.getRepresentation();
        if (rep->IsIncludedStream())
          continue;
      }

      ids.emplace_back(m_session->GetStreamIdFromIndex(i));
    }
  }

  return !ids.empty();
}

void CInputStreamAdaptive::GetCapabilities(kodi::addon::InputstreamCapabilities& caps)
{
  LOG::Log(LOGDEBUG, "GetCapabilities()");
  uint32_t mask = INPUTSTREAM_SUPPORTS_IDEMUX | INPUTSTREAM_SUPPORTS_IDISPLAYTIME |
                  INPUTSTREAM_SUPPORTS_IPOSTIME | INPUTSTREAM_SUPPORTS_SEEK |
                  INPUTSTREAM_SUPPORTS_PAUSE;
#if INPUTSTREAM_VERSION_LEVEL > 1
  mask |= INPUTSTREAM_SUPPORTS_ICHAPTER;
#endif
  caps.SetMask(mask);
}

bool CInputStreamAdaptive::GetStream(int streamid, kodi::addon::InputstreamInfo& info)
{
  // GetStream is called by Kodi twice times, before and after OpenStream.
  LOG::Log(LOGDEBUG, "GetStream(%d)", streamid);

  // Return false prevents this stream from loading, but does not stop playback,
  // Kodi core will continue to request another stream of same type (a/v)
  // as long as one is successful
  return m_session->OnGetStream(streamid, info);

  //! @todo: Kodi VideoPlayer stream fallback bug / problem:
  //! if a video stream for some reason does not start (OpenStream/GetStream fails) VideoPlayer has no logic to open the next video stream based of stream ID
  //! this is a big problem (can be reproduced using "Stream selection type" to "Manual OSD" and hack a bit the code)
  //! because VP seem to always take the first stream ID (1001) in index order, instead to continue with the next stream ID.
  //! For example if you are playing stream ID 1022 and fails, VP try to play always 1001 instead of 1023.
  //! This causes the following problems:
  //! - impossibility to keep a consistent video codec to be used
  //! - impossibility to have a stable video quality fallback in a decreasing manner. We could sort the list of streams decreasingly by resolution and bandwidth
  //!   but dont solve in full the problem in case of multicodec's because VP will mix all.
  //!   I'm talking about "decreasing manner" quality also because with DRM if 4k / FULLHD doesn't work you need a fallback to lower quality.
  //! this needs to be fixed in the Kodi core VP
}

void CInputStreamAdaptive::UnlinkIncludedStreams(CStream* stream)
{
  if (stream->m_mainStreamIndex.has_value())
  {
    CStream* mainStream(m_session->GetStream(*stream->m_mainStreamIndex));
    if (mainStream->GetReader())
      mainStream->GetReader()->RemoveStreamType(stream->m_info.GetStreamType());
  }

  const CRepresentation* rep = stream->m_adStream.getRepresentation();

  if (rep->IsIncludedStream())
    m_IncludedStreams.erase(stream->m_info.GetStreamType());
}

void CInputStreamAdaptive::EnableStream(int streamid, bool enable)
{
  LOG::Log(LOGDEBUG, "EnableStream(%d: %s)", streamid, enable ? "true" : "false");

  if (!m_session)
    return;

  CStream* stream{m_session->GetStream(m_session->GetStreamIndexFromId(streamid))};

  if (!enable && stream && stream->IsEnabled())
  {
    UnlinkIncludedStreams(stream);
    m_session->EnableStream(stream, false);
  }
}

// OpenStream method notes:
// - This method is called:
//    - At playback start
//    - At chapter/period change (DEMUX_SPECIALID_STREAMCHANGE)
//    - At stream quality change (DEMUX_SPECIALID_STREAMCHANGE) by "adaptive" streaming or from Kodi OSD
//    - Due to CDVDDemuxClient::ParsePacket (Kodi core) while in playback, see "Kodi core is attempting to reopen the stream" below
// - The "streamid" requested can be influenced from preferences set in Kodi settings (e.g. language).
// - If the requested "streamid" fails to open on the Kodi core side (after OpenStream callback, e.g. for missing extradata)
//   Kodi core will try to (fallback) open another video "streamid", this will happen recursively until success.
// - The OpenStream method not only opens the stream, but also implicitly enables it
//     so don't exists a EnableStream callback to explicitly enable the stream after the opening,
//     EnableStream method is used by VP only to disable the stream
//     which can happen immediately after opening (e.g. subtitles disabled on playback startup).
// - If a stream info property has been changed, you need to return "true" on OpenStream
//   to allow Kodi core to update its internal properties with our changes.
bool CInputStreamAdaptive::OpenStream(int streamid)
{
  LOG::Log(LOGDEBUG, "OpenStream(%d)", streamid);

  if (!m_session)
    return false;

  CStream* stream(m_session->GetStream(m_session->GetStreamIndexFromId(streamid)));

  if (!stream)
    return false;

  if (stream->IsEnabled())
  {
    // Stream quality changed
    if (stream->m_adStream.StreamChanged())
    {
      UnlinkIncludedStreams(stream);
      stream->Reset();
      stream->m_adStream.Reset();
    }
    else // Kodi core is attempting to reopen the stream
    {
      // If a stream was already opened means that Kodi core is attempting to reopen
      // ALL streams just for his internal purposes and this callback must be avoided.

      //! @todo: This issue appears to have been introduced with PR https://github.com/xbmc/xbmc/pull/10097
      //! when "changes" var differs from the stored value, it force to reopen all streams,
      //! where in the past was reopening only the associated stream.
      //!
      //! The request to reopen a stream is unclear,
      //! there is no reason to open a stream already opened, because the EnableStream(false) callback was never sent
      //! so the state of the addon is still unchanged.
      //!
      //! Looks like that the "changes" var is modified by CDVDDemuxClient::ParsePacket while in playback
      //! https://github.com/xbmc/xbmc/blob/d1a1d48c3cb3722d39264ffdd8132f755ffecd27/xbmc/cores/VideoPlayer/DVDDemuxers/DVDDemuxClient.cpp#L119
      //! maybe this is required for some other CDVDInputStream interface(?) but not for CInputStreamAddon, this should be at least optional
      //! since can easily leads to playback problems such as stuttering because reopening all streams can be an heavy task to do while playback
      //! moreover for subtitles case this is even more messed up, since they can be disabled... (EnableStream callback)

      // NOTE: you cannot rely on stream->IsEnabled() for all stream types because a playback can start with subtitles disabled
      // so this leads to a mess between OpenStream and EnableStream callbacks causing abnormal behaviors in the addon components.
      // As workaround we detect the first stream opened twice times (OpenStream for subtitles type is always last),
      // then we set m_checkCoreReopen to true in order to skip all the following callbacks to OpenStream,
      // and when we receive the first DemuxRead callback means that Kodi core has finished all openings,
      // then we reset m_checkCoreReopen for the next round of OpenStream callbacks
      m_checkCoreReopen = true;
    }
  }

  if (m_checkCoreReopen)
  {
    LOG::Log(LOGDEBUG, "OpenStream(%d): The stream has already been opened", streamid);
    return false;
  }

  stream->SetIsEnabled(true);

  CRepresentation* rep = stream->m_adStream.getRepresentation();

  // If we select a dummy (=inside video) stream, open the video part
  // Dummy streams will be never enabled, they will only enable / activate audio track.
  if (rep->IsIncludedStream())
  {
    CStream* mainStream;
    unsigned int mainStreamIndex{0};

    while ((mainStream = m_session->GetStream(mainStreamIndex++)))
    {
      if (mainStream->m_info.GetStreamType() == INPUTSTREAM_TYPE_VIDEO && mainStream->IsEnabled())
        break;
    }

    if (mainStream)
    {
      stream->m_mainStreamIndex = mainStreamIndex;
      ISampleReader* mainReader = mainStream->GetReader();
      if (!mainReader)
      {
        LOG::LogF(LOGERROR, "Cannot get the stream sample reader");
      }
      else
      {
        mainReader->AddStreamType(stream->m_info.GetStreamType(), streamid);
        mainReader->GetInformation(stream->m_info);
      }
    }
    else
    {
      stream->m_mainStreamIndex.reset();
    }

    m_IncludedStreams[stream->m_info.GetStreamType()] = streamid;
    return false;
  }

  if (!m_session->PrepareStream(stream, m_lastPts))
  {
    m_session->EnableStream(stream, false);
    return false;
  }

  stream->GetReader()->SetStreamId(stream->m_info.GetStreamType(), streamid);

  if (stream->m_info.GetStreamType() == INPUTSTREAM_TYPE_VIDEO)
  {
    for (auto& [streamType, id] : m_IncludedStreams)
    {
      stream->GetReader()->AddStreamType(streamType, id);

      const unsigned int streamIndex = m_session->GetStreamIndexFromId(id);

      CStream* incStream = m_session->GetStream(streamIndex);
      if (!incStream)
      {
        LOG::LogF(LOGERROR, "Cannot get the stream from stream index %u", streamIndex);
      }
      else
      {
        stream->GetReader()->GetInformation(incStream->m_info);
      }
    }
  }

  m_session->EnableStream(stream, true);

  // If stream use DRM always update stream info
  const bool isInfoChanged = stream->GetReader()->GetInformation(stream->m_info) ||
                             !stream->m_info.GetCryptoSession().GetSessionId().empty();
  return isInfoChanged;
}

DEMUX_PACKET* CInputStreamAdaptive::DemuxRead(void)
{
  if (!m_session)
    return NULL;

  m_session->OnDemuxRead();

  // On Kodi core CDVDDemuxClient::ParsePacket occurs after the DemuxRead callback
  // since it can cause to reopen all streams, reset the check just before
  m_checkCoreReopen = false;

  if (~m_failedSeekTime)
  {
    LOG::Log(LOGDEBUG, "Seeking to last failed seek position (%d)", m_failedSeekTime);
    m_session->SeekTime(static_cast<double>(m_failedSeekTime) * 0.001f, 0, false);
    m_failedSeekTime = ~0;
  }

  ISampleReader* sr{nullptr};

  if (m_session->GetNextSample(sr))
  {
    DEMUX_PACKET* p{nullptr};

    if (m_session->CheckChange())
    {
      // Adaptive stream has switched stream (representation) quality
      m_lastPts = PLAYLIST::NO_PTS_VALUE;
      p = AllocateDemuxPacket(0);
      p->iStreamId = DEMUX_SPECIALID_STREAMCHANGE;
      LOG::Log(LOGDEBUG, "DEMUX_SPECIALID_STREAMCHANGE (stream quality changed)");
      return p;
    }

    if (sr)
    {
      AP4_Size iSize(sr->GetSampleDataSize());
      const AP4_UI08* pData(sr->GetSampleData());
      bool srHaveData{iSize > 0 && pData};

      if (srHaveData)
      {
        m_lastPts = sr->DTSorPTSManifest();
        p = AllocateDemuxPacket(iSize);
        p->dts = static_cast<double>(sr->DTS());
        p->pts = static_cast<double>(sr->PTS());
        p->duration = static_cast<double>(sr->GetDuration());
        p->iStreamId = sr->GetStreamId();
        p->iGroupId = 0;
        p->iSize = iSize;
        std::memcpy(p->pData, pData, iSize);
      }

      //LOG::Log(LOGDEBUG, "DTS: %0.4f, PTS:%0.4f, ID: %u SZ: %d", p->dts, p->pts, p->iStreamId, p->iSize);

      // Start reading the next sample
      sr->ReadSampleAsync();
    }
    else // We are waiting for the data, so return an empty packet
    {
      p = AllocateDemuxPacket(0);
    }

    return p;
  }

  // Ends here when GetNextSample fails due to sample reader in EOS state or stream disabled
  // that could means, in case of multi-periods streams, that segments are ended
  // in the current period and its needed to switch to the next period
  if (m_session->SeekChapter(m_session->GetChapter() + 1))
  {
    // Switched to new period / chapter
    m_lastPts = PLAYLIST::NO_PTS_VALUE;
    // Disable streams from the old period (kodi core never close/disable streams...)
    for (unsigned int i(0); i < INPUTSTREAM_MAX_STREAM_COUNT && i < m_session->GetStreamCount();
         ++i)
    {
      EnableStream(m_session->GetStreamIdFromIndex(i), false);
    }
    // Initialize the new period
    m_session->InitializePeriod();
    DEMUX_PACKET* p = AllocateDemuxPacket(0);
    p->iStreamId = DEMUX_SPECIALID_STREAMCHANGE;
    LOG::Log(LOGDEBUG, "DEMUX_SPECIALID_STREAMCHANGE (chapter changed)");
    return p;
  }
  return NULL;
}

// Accurate search (PTS based)
bool CInputStreamAdaptive::DemuxSeekTime(double time, bool backwards, double& startpts)
{
  return true;
}

void CInputStreamAdaptive::SetVideoResolution(unsigned int width,
                                              unsigned int height,
                                              unsigned int maxWidth,
                                              unsigned int maxHeight)
{
  CSrvBroker::GetResources().SetScreenInfo({static_cast<int>(width), static_cast<int>(height),
                                            static_cast<int>(maxWidth),
                                            static_cast<int>(maxHeight)});

  // SetVideoResolution method is initially called before CInputStreamAdaptive::Open so there is no session yet
  // After that, other callbacks may be made during playback (e.g. for window resize)
  if (m_session)
    m_session->OnScreenResChange();
}

bool CInputStreamAdaptive::PosTime(int ms)
{
  if (!m_session)
    return false;

  LOG::Log(LOGINFO, "PosTime (%d)", ms);

  bool ret = m_session->SeekTime(static_cast<double>(ms) * 0.001f, 0, false);
  m_failedSeekTime = ret ? ~0 : ms;

  return ret;
}

int CInputStreamAdaptive::GetTotalTime()
{
  if (!m_session)
    return 0;

  return static_cast<int>(m_session->GetTotalTimeMs());
}

int CInputStreamAdaptive::GetTime()
{
  if (!m_session)
    return 0;

  int timeMs = static_cast<int>(m_session->GetElapsedTimeMs());
  return timeMs;
}

bool CInputStreamAdaptive::IsRealTimeStream()
{
  return m_session && m_session->IsLive();
}

#if INPUTSTREAM_VERSION_LEVEL > 1
int CInputStreamAdaptive::GetChapter()
{
  // Provide the current chapter number
  // chapter numbering starts from 1, specify 0 for no chapter
  return m_session ? m_session->GetChapter() : 0;
}

int CInputStreamAdaptive::GetChapterCount()
{
  if (m_session)
  {
    const int count = m_session->GetChapterCount();
    if (count > 1)
      return count;
  }
  // Return 0 to prevent Kodi core from handling chapters
  return 0;
}

const char* CInputStreamAdaptive::GetChapterName(int ch)
{
  if (!m_session)
    return nullptr;

  return m_session->GetChapterName(ch);
}

int64_t CInputStreamAdaptive::GetChapterPos(int ch)
{
  return m_session ? m_session->GetChapterPos(ch) : 0;
}

bool CInputStreamAdaptive::SeekChapter(int ch)
{
  return m_session ? m_session->SeekChapter(ch) : false;
}
#endif

/*****************************************************************************************************/

class ATTR_DLL_LOCAL CMyAddon : public kodi::addon::CAddonBase
{
public:
  CMyAddon();
  ADDON_STATUS CreateInstance(const kodi::addon::IInstanceInfo& instance,
                              KODI_ADDON_INSTANCE_HDL& hdl) override;
};

CMyAddon::CMyAddon()
{
}

ADDON_STATUS CMyAddon::CreateInstance(const kodi::addon::IInstanceInfo& instance,
                                      KODI_ADDON_INSTANCE_HDL& hdl)
{
  if (instance.IsType(ADDON_INSTANCE_INPUTSTREAM))
  {
    kodi_addon_logger_rs_initialize(kodi::addon::CPrivateBase::m_interface->toKodi->kodiBase,
                                    kodi::addon::CPrivateBase::m_interface->toKodi->addon_log_msg);
    hdl = new CInputStreamAdaptive(instance);
    return ADDON_STATUS_OK;
  }
  return ADDON_STATUS_NOT_IMPLEMENTED;
}

ADDONCREATOR(CMyAddon);
