/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "VideoSettings.h"
#include "QGCApplication.h"
#include "VideoManager.h"

#include <QQmlEngine>
#include <QtQml>
#include <QVariantList>

#ifndef QGC_DISABLE_UVC
#include <QCameraInfo>
#endif

const char* VideoSettings::videoSourceNoVideo   = "No Video Available";
const char* VideoSettings::videoDisabled        = "Video Stream Disabled";
const char* VideoSettings::videoSourceRTSP      = "RTSP Video Stream";
const char* VideoSettings::videoSourceRTSP2      = "RTSP Video Stream 2";
const char* VideoSettings::videoSourceUDPH264   = "UDP h.264 Video Stream";
const char* VideoSettings::videoSourceUDPH265   = "UDP h.265 Video Stream";
const char* VideoSettings::videoSourceTCP       = "TCP-MPEG2 Video Stream";
const char* VideoSettings::videoSourceMPEGTS    = "MPEG-TS (h.264) Video Stream";

DECLARE_SETTINGGROUP(Video, "Video")
{
    qmlRegisterUncreatableType<VideoSettings>("QGroundControl.SettingsManager", 1, 0, "VideoSettings", "Reference only");

    // Setup enum values for videoSource settings into meta data
    QStringList videoSourceList;
#ifdef QGC_GST_STREAMING
    videoSourceList.append(videoSourceRTSP);
#ifndef NO_UDP_VIDEO
    videoSourceList.append(videoSourceUDPH264);
    videoSourceList.append(videoSourceUDPH265);
#endif
    videoSourceList.append(videoSourceTCP);
    videoSourceList.append(videoSourceMPEGTS);
#endif
#ifndef QGC_DISABLE_UVC
    QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo &cameraInfo: cameras) {
        videoSourceList.append(cameraInfo.description());
    }
#endif
    if (videoSourceList.count() == 0) {
        _noVideo = true;
        videoSourceList.append(videoSourceNoVideo);
    } else {
        videoSourceList.insert(0, videoDisabled);
    }
    QVariantList videoSourceVarList;
    for (const QString& videoSource: videoSourceList) {
        videoSourceVarList.append(QVariant::fromValue(videoSource));
    }
    _nameToMetaDataMap[videoSourceName]->setEnumInfo(videoSourceList, videoSourceVarList);

    QStringList videoSource2List;
#ifdef QGC_GST_STREAMING
    videoSource2List.append(videoSourceRTSP2);
#endif
    if (videoSource2List.count() == 0) {
        _noVideo2 = true;
        videoSource2List.append(videoSourceNoVideo);
    } else {
        videoSource2List.insert(0, videoDisabled);
    }

    QVariantList videoSource2VarList;
    for (const QString& videoSource2: videoSource2List) {
        videoSource2VarList.append(QVariant::fromValue(videoSource2));
    }
    _nameToMetaDataMap[videoSource2Name]->setEnumInfo(videoSource2List, videoSource2VarList);
    // Set default value for videoSource
    _setDefaults();
}

void VideoSettings::_setDefaults()
{
    if (_noVideo) {
        _nameToMetaDataMap[videoSourceName]->setRawDefaultValue(videoSourceNoVideo);
    } else {
        _nameToMetaDataMap[videoSourceName]->setRawDefaultValue(videoDisabled);
    }
    if (_noVideo2) {
        _nameToMetaDataMap[videoSource2Name]->setRawDefaultValue(videoSourceNoVideo);
    } else {
        _nameToMetaDataMap[videoSource2Name]->setRawDefaultValue(videoDisabled);
    }
}

DECLARE_SETTINGSFACT(VideoSettings, aspectRatio)
DECLARE_SETTINGSFACT(VideoSettings, videoFit)
DECLARE_SETTINGSFACT(VideoSettings, gridLines)
DECLARE_SETTINGSFACT(VideoSettings, showRecControl)
DECLARE_SETTINGSFACT(VideoSettings, recordingFormat)
DECLARE_SETTINGSFACT(VideoSettings, maxVideoSize)
DECLARE_SETTINGSFACT(VideoSettings, enableStorageLimit)
DECLARE_SETTINGSFACT(VideoSettings, rtspTimeout)
DECLARE_SETTINGSFACT(VideoSettings, streamEnabled)
DECLARE_SETTINGSFACT(VideoSettings, disableWhenDisarmed)
DECLARE_SETTINGSFACT(VideoSettings, lowLatencyMode)

DECLARE_SETTINGSFACT_NO_FUNC(VideoSettings, videoSource)
{
    if (!_videoSourceFact) {
        _videoSourceFact = _createSettingsFact(videoSourceName);
        //-- Check for sources no longer available
        if(!_videoSourceFact->enumStrings().contains(_videoSourceFact->rawValue().toString())) {
            if (_noVideo) {
                _videoSourceFact->setRawValue(videoSourceNoVideo);
            } else {
                _videoSourceFact->setRawValue(videoDisabled);
            }
        }
        connect(_videoSourceFact, &Fact::valueChanged, this, &VideoSettings::_configChanged);
    }
    return _videoSourceFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(VideoSettings, videoSource2)
{
    if (!_videoSource2Fact) {
        _videoSource2Fact = _createSettingsFact(videoSource2Name);
        //-- Check for sources no longer available
        if(!_videoSource2Fact->enumStrings().contains(_videoSource2Fact->rawValue().toString())) {
            if (_noVideo2) {
                _videoSource2Fact->setRawValue(videoSourceNoVideo);
            } else {
                _videoSource2Fact->setRawValue(videoDisabled);
            }
        }
        connect(_videoSource2Fact, &Fact::valueChanged, this, &VideoSettings::_configChanged);
    }
    return _videoSource2Fact;
}

DECLARE_SETTINGSFACT_NO_FUNC(VideoSettings, udpPort)
{
    if (!_udpPortFact) {
        _udpPortFact = _createSettingsFact(udpPortName);
        connect(_udpPortFact, &Fact::valueChanged, this, &VideoSettings::_configChanged);
    }
    return _udpPortFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(VideoSettings, rtspUrl)
{
    if (!_rtspUrlFact) {
        _rtspUrlFact = _createSettingsFact(rtspUrlName);
        connect(_rtspUrlFact, &Fact::valueChanged, this, &VideoSettings::_configChanged);
    }
    return _rtspUrlFact;
}

DECLARE_SETTINGSFACT_NO_FUNC(VideoSettings, rtspUrl2)
{
    if (!_rtspUrl2Fact) {
        _rtspUrl2Fact = _createSettingsFact(rtspUrl2Name);
        connect(_rtspUrl2Fact, &Fact::valueChanged, this, &VideoSettings::_configChanged);
    }
    return _rtspUrl2Fact;
}

DECLARE_SETTINGSFACT_NO_FUNC(VideoSettings, tcpUrl)
{
    if (!_tcpUrlFact) {
        _tcpUrlFact = _createSettingsFact(tcpUrlName);
        connect(_tcpUrlFact, &Fact::valueChanged, this, &VideoSettings::_configChanged);
    }
    return _tcpUrlFact;
}

bool VideoSettings::streamConfigured(void)
{
#if !defined(QGC_GST_STREAMING)
    return false;
#endif
    //-- First, check if it's autoconfigured
    if(qgcApp()->toolbox()->videoManager()->autoStreamConfigured()) {
        qCDebug(VideoManagerLog) << "Stream auto configured";
        return true;
    }
    //-- Check if it's disabled
    QString vSource = videoSource()->rawValue().toString();
    QString vSource2 = videoSource2()->rawValue().toString();
    if((vSource == videoSourceNoVideo || vSource == videoDisabled) && (vSource2 == videoSourceNoVideo || vSource2 == videoDisabled)) {
        return false;
    }
    //-- If UDP, check if port is set
    if(vSource == videoSourceUDPH264 || vSource == videoSourceUDPH265) {
        qCDebug(VideoManagerLog) << "Testing configuration for UDP Stream:" << udpPort()->rawValue().toInt();
        return udpPort()->rawValue().toInt() != 0;
    }
    //-- If RTSP, check for URL
    if(vSource == videoSourceRTSP) {
        qCDebug(VideoManagerLog) << "Testing configuration for RTSP Stream:" << rtspUrl()->rawValue().toString();
        return !rtspUrl()->rawValue().toString().isEmpty();
    }
    //-- If RTSP, check for URL
    if(vSource2 == videoSourceRTSP2) {
        qCDebug(VideoManagerLog) << "Testing configuration for RTSP Stream 2:" << rtspUrl2()->rawValue().toString();
        return !rtspUrl2()->rawValue().toString().isEmpty();
    }
    //-- If TCP, check for URL
    if(vSource == videoSourceTCP) {
        qCDebug(VideoManagerLog) << "Testing configuration for TCP Stream:" << tcpUrl()->rawValue().toString();
        return !tcpUrl()->rawValue().toString().isEmpty();
    }
    //-- If MPEG-TS, check if port is set
    if(vSource == videoSourceMPEGTS) {
        qCDebug(VideoManagerLog) << "Testing configuration for MPEG-TS Stream:" << udpPort()->rawValue().toInt();
        return udpPort()->rawValue().toInt() != 0;
    }
    return false;
}

void VideoSettings::_configChanged(QVariant)
{
    emit streamConfiguredChanged();
}
