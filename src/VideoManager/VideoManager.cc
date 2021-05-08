/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/


#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QUrl>
#include <QDir>

#ifndef QGC_DISABLE_UVC
#include <QCameraInfo>
#endif

#include "ScreenToolsController.h"
#include "VideoManager.h"
#include "QGCToolbox.h"
#include "QGCCorePlugin.h"
#include "QGCOptions.h"
#include "MultiVehicleManager.h"
#include "Settings/SettingsManager.h"
#include "Vehicle.h"
#include "QGCCameraManager.h"

#if defined(QGC_GST_STREAMING)
#include "GStreamer.h"
#else
#include "GLVideoItemStub.h"
#endif

#ifdef QGC_GST_TAISYNC_ENABLED
#include "TaisyncHandler.h"
#endif

QGC_LOGGING_CATEGORY(VideoManagerLog, "VideoManagerLog")

#if defined(QGC_GST_STREAMING)
static const char* kFileExtension[VideoReceiver::FILE_FORMAT_MAX - VideoReceiver::FILE_FORMAT_MIN] = {
    "mkv",
    "mov",
    "mp4"
};
#endif

//-----------------------------------------------------------------------------
VideoManager::VideoManager(QGCApplication* app, QGCToolbox* toolbox)
    : QGCTool(app, toolbox)
{
#if !defined(QGC_GST_STREAMING)
    static bool once = false;
    if (!once) {
        qmlRegisterType<GLVideoItemStub>("org.freedesktop.gstreamer.GLVideoItem", 1, 0, "GstGLVideoItem");
        once = true;
    }
#endif
}

//-----------------------------------------------------------------------------
VideoManager::~VideoManager()
{
    for (int i = 0; i < 5; i++) {
        if (_videoReceiver[i] != nullptr) {
            delete _videoReceiver[i];
            _videoReceiver[i] = nullptr;
        }
#if defined(QGC_GST_STREAMING)
        if (_videoSink[i] != nullptr) {
            // FIXME: AV: we need some interaface for video sink with .release() call
            // Currently VideoManager is destroyed after corePlugin() and we are crashing on app exit
            // calling qgcApp()->toolbox()->corePlugin()->releaseVideoSink(_videoSink[i]);
            // As for now let's call GStreamer::releaseVideoSink() directly
            GStreamer::releaseVideoSink(_videoSink[i]);
            _videoSink[i] = nullptr;
        }
#endif
    }
}

//-----------------------------------------------------------------------------
void
VideoManager::setToolbox(QGCToolbox *toolbox)
{
   QGCTool::setToolbox(toolbox);
   QQmlEngine::setObjectOwnership(this, QQmlEngine::CppOwnership);
   qmlRegisterUncreatableType<VideoManager> ("QGroundControl.VideoManager", 1, 0, "VideoManager", "Reference only");
   qmlRegisterUncreatableType<VideoReceiver>("QGroundControl",              1, 0, "VideoReceiver","Reference only");

   // TODO: Those connections should be Per Video, not per VideoManager.
   _videoSettings = toolbox->settingsManager()->videoSettings();
   QString videoSource = _videoSettings->videoSource()->rawValue().toString();
   connect(_videoSettings->videoSource(),   &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged);
   connect(_videoSettings->udpPort(),       &Fact::rawValueChanged, this, &VideoManager::_udpPortChanged);
   connect(_videoSettings->rtspUrl(),       &Fact::rawValueChanged, this, &VideoManager::_rtspUrlChanged);
   connect(_videoSettings->tcpUrl(),        &Fact::rawValueChanged, this, &VideoManager::_tcpUrlChanged);
   connect(_videoSettings->aspectRatio(),   &Fact::rawValueChanged, this, &VideoManager::_aspectRatioChanged);
   connect(_videoSettings->lowLatencyMode(),&Fact::rawValueChanged, this, &VideoManager::_lowLatencyModeChanged);

   QString videoSource2 = _videoSettings->videoSource2()->rawValue().toString();
   connect(_videoSettings->videoSource2(),   &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged2);
   connect(_videoSettings->rtspUrl2(),       &Fact::rawValueChanged, this, &VideoManager::_rtspUrlChanged2);
   QString videoSource3 = _videoSettings->videoSource3()->rawValue().toString();
   connect(_videoSettings->videoSource3(),   &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged3);
   connect(_videoSettings->rtspUrl3(),       &Fact::rawValueChanged, this, &VideoManager::_rtspUrlChanged3);
   QString videoSource4 = _videoSettings->videoSource4()->rawValue().toString();
   connect(_videoSettings->videoSource4(),   &Fact::rawValueChanged, this, &VideoManager::_videoSourceChanged4);
   connect(_videoSettings->rtspUrl4(),       &Fact::rawValueChanged, this, &VideoManager::_rtspUrlChanged4);

   MultiVehicleManager *pVehicleMgr = qgcApp()->toolbox()->multiVehicleManager();
   connect(pVehicleMgr, &MultiVehicleManager::activeVehicleChanged, this, &VideoManager::_setActiveVehicle);



#if defined(QGC_GST_STREAMING)
   GStreamer::blacklist(static_cast<VideoSettings::VideoDecoderOptions>(_videoSettings->forceVideoDecoder()->rawValue().toInt()));
#ifndef QGC_DISABLE_UVC
   // If we are using a UVC camera setup the device name
   _updateUVC();
#endif

    emit isGStreamerChanged();
    emit isGStreamerChanged2();
    emit isGStreamerChanged3();
    emit isGStreamerChanged4();

    qCDebug(VideoManagerLog) << "New Video Source:" << videoSource;
    qCDebug(VideoManagerLog) << "New Video Source 2:" << videoSource2;
    qCDebug(VideoManagerLog) << "New Video Source 3:" << videoSource3;
    qCDebug(VideoManagerLog) << "New Video Source 4:" << videoSource4;

#if defined(QGC_GST_STREAMING)
    _videoReceiver[0] = toolbox->corePlugin()->createVideoReceiver(this);
    _videoReceiver[1] = toolbox->corePlugin()->createVideoReceiver(this);
    _videoReceiver[2] = toolbox->corePlugin()->createVideoReceiver(this);
    _videoReceiver[3] = toolbox->corePlugin()->createVideoReceiver(this);
    _videoReceiver[4] = toolbox->corePlugin()->createVideoReceiver(this);

//0
    connect(_videoReceiver[0], &VideoReceiver::streamingChanged, this, [this](bool active){
        _streaming = active;
        emit streamingChanged();
    });

    connect(_videoReceiver[0], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
        if (status == VideoReceiver::STATUS_OK) {
            _videoStarted[0] = true;
            if (_videoSink[0] != nullptr) {
                // It is absolytely ok to have video receiver active (streaming) and decoding not active
                // It should be handy for cases when you have many streams and want to show only some of them
                // NOTE that even if decoder did not start it is still possible to record video
                _videoReceiver[0]->startDecoding(_videoSink[0]);
            }
        } else if (status == VideoReceiver::STATUS_INVALID_URL) {
            // Invalid URL - don't restart
        } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
            // Already running
        } else {
            _restartVideo(0);
        }
    });

    connect(_videoReceiver[0], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
        _videoStarted[0] = false;
        _startReceiver(0);
    });

    connect(_videoReceiver[0], &VideoReceiver::decodingChanged, this, [this](bool active){
        _decoding = active;
        emit decodingChanged();
    });

    connect(_videoReceiver[0], &VideoReceiver::recordingChanged, this, [this](bool active){
        _recording = active;
        if (!active) {
            _subtitleWriter.stopCapturingTelemetry();
        }
        emit recordingChanged();
    });

    connect(_videoReceiver[0], &VideoReceiver::recordingStarted, this, [this](){
        _subtitleWriter.startCapturingTelemetry(_videoFile);
    });

    connect(_videoReceiver[0], &VideoReceiver::videoSizeChanged, this, [this](QSize size){
        _videoSize = ((quint32)size.width() << 16) | (quint32)size.height();
        emit videoSizeChanged();
    });


//1
    connect(_videoReceiver[1], &VideoReceiver::streamingChanged, this, [this](bool active){
        _streaming = active;
        emit streamingChanged();
    });

    connect(_videoReceiver[1], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
        if (status == VideoReceiver::STATUS_OK) {
            _videoStarted[1] = true;
            if (_videoSink[1] != nullptr) {
                // It is absolytely ok to have video receiver active (streaming) and decoding2 not active
                // It should be handy for cases when you have many streams and want to show only some of them
                // NOTE that even if decoder did not start it is still possible to record video
                _videoReceiver[1]->startDecoding(_videoSink[1]);
            }
        } else if (status == VideoReceiver::STATUS_INVALID_URL) {
            // Invalid URL - don't restart
        } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
            // Already running
        } else {
            _restartVideo(1);
        }
    });

    connect(_videoReceiver[1], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
        _videoStarted[1] = false;
        _startReceiver(1);
    });

    connect(_videoReceiver[1], &VideoReceiver::decodingChanged, this, [this](bool active){
        _decoding2 = active;
        emit decodingChanged();
    });

    connect(_videoReceiver[1], &VideoReceiver::recordingChanged, this, [this](bool active){
        _recording = active;
        if (!active) {
            _subtitleWriter.stopCapturingTelemetry();
        }
        emit recordingChanged();
    });

    connect(_videoReceiver[1], &VideoReceiver::recordingStarted, this, [this](){
        _subtitleWriter.startCapturingTelemetry(_videoFile);
    });

    connect(_videoReceiver[1], &VideoReceiver::videoSizeChanged, this, [this](QSize size){
        _videoSize = ((quint32)size.width() << 16) | (quint32)size.height();
        emit videoSizeChanged();
    });

//2
        connect(_videoReceiver[2], &VideoReceiver::streamingChanged, this, [this](bool active){
            _streaming = active;
            emit streamingChanged();
        });

        connect(_videoReceiver[2], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
            if (status == VideoReceiver::STATUS_OK) {
                _videoStarted[2] = true;
                if (_videoSink[2] != nullptr) {
                    // It is absolytely ok to have video receiver active (streaming) and decoding2 not active
                    // It should be handy for cases when you have many streams and want to show only some of them
                    // NOTE that even if decoder did not start it is still possible to record video
                    _videoReceiver[2]->startDecoding(_videoSink[2]);
                }
            } else if (status == VideoReceiver::STATUS_INVALID_URL) {
                // Invalid URL - don't restart
            } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
                // Already running
            } else {
                _restartVideo(2);
            }
        });

        connect(_videoReceiver[2], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
            _videoStarted[2] = false;
            _startReceiver(2);
        });

        connect(_videoReceiver[2], &VideoReceiver::decodingChanged, this, [this](bool active){
            _decoding3 = active;
            emit decodingChanged();
        });

        connect(_videoReceiver[2], &VideoReceiver::recordingChanged, this, [this](bool active){
            _recording = active;
            if (!active) {
                _subtitleWriter.stopCapturingTelemetry();
            }
            emit recordingChanged();
        });

        connect(_videoReceiver[2], &VideoReceiver::recordingStarted, this, [this](){
            _subtitleWriter.startCapturingTelemetry(_videoFile);
        });

        connect(_videoReceiver[2], &VideoReceiver::videoSizeChanged, this, [this](QSize size){
            _videoSize = ((quint32)size.width() << 16) | (quint32)size.height();
            emit videoSizeChanged();
        });

        //3
            connect(_videoReceiver[3], &VideoReceiver::streamingChanged, this, [this](bool active){
                _streaming = active;
                emit streamingChanged();
            });

            connect(_videoReceiver[3], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
                if (status == VideoReceiver::STATUS_OK) {
                    _videoStarted[3] = true;
                    if (_videoSink[3] != nullptr) {
                        // It is absolytely ok to have video receiver active (streaming) and decoding2 not active
                        // It should be handy for cases when you have many streams and want to show only some of them
                        // NOTE that even if decoder did not start it is still possible to record video
                        _videoReceiver[3]->startDecoding(_videoSink[3]);
                    }
                } else if (status == VideoReceiver::STATUS_INVALID_URL) {
                    // Invalid URL - don't restart
                } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
                    // Already running
                } else {
                    _restartVideo(3);
                }
            });

            connect(_videoReceiver[3], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
                _videoStarted[3] = false;
                _startReceiver(3);
            });

            connect(_videoReceiver[3], &VideoReceiver::decodingChanged, this, [this](bool active){
                _decoding4 = active;
                emit decodingChanged();
            });

            connect(_videoReceiver[3], &VideoReceiver::recordingChanged, this, [this](bool active){
                _recording = active;
                if (!active) {
                    _subtitleWriter.stopCapturingTelemetry();
                }
                emit recordingChanged();
            });

            connect(_videoReceiver[3], &VideoReceiver::recordingStarted, this, [this](){
                _subtitleWriter.startCapturingTelemetry(_videoFile);
            });

            connect(_videoReceiver[3], &VideoReceiver::videoSizeChanged, this, [this](QSize size){
                _videoSize = ((quint32)size.width() << 16) | (quint32)size.height();
                emit videoSizeChanged();
            });

    //connect(_videoReceiver, &VideoReceiver::onTakeScreenshotComplete, this, [this](VideoReceiver::STATUS status){
    //    if (status == VideoReceiver::STATUS_OK) {
    //    }
    //});

    // FIXME: AV: I believe _thermalVideoReceiver should be handled just like _videoReceiver in terms of event
    // and I expect that it will be changed during multiple video stream activity
    if (_videoReceiver[4] != nullptr) {
        connect(_videoReceiver[4], &VideoReceiver::onStartComplete, this, [this](VideoReceiver::STATUS status) {
            if (status == VideoReceiver::STATUS_OK) {
                _videoStarted[4] = true;
                if (_videoSink[4] != nullptr) {
                    _videoReceiver[4]->startDecoding(_videoSink[4]);
                }
            } else if (status == VideoReceiver::STATUS_INVALID_URL) {
                // Invalid URL - don't restart
            } else if (status == VideoReceiver::STATUS_INVALID_STATE) {
                // Already running
            } else {
                _restartVideo(4);
            }
        });

        connect(_videoReceiver[4], &VideoReceiver::onStopComplete, this, [this](VideoReceiver::STATUS) {
            _videoStarted[4] = false;
            _startReceiver(4);
        });
    }
#endif
    _updateSettings(0);
    _updateSettings(1);
    _updateSettings(2);
    _updateSettings(3);
    _updateSettings(4);

    if(isGStreamer()) {
        startVideo(0);
    } else {
        stopVideo(0);
    }
    if(isGStreamer2()) {
        startVideo(1);
    } else {
        stopVideo(1);
    }
    if(isGStreamer3()) {
        startVideo(2);
    } else {
        stopVideo(2);
    }
    if(isGStreamer4()) {
        startVideo(3);
    } else {
        stopVideo(3);
    }

#endif
}

void VideoManager::_cleanupOldVideos()
{
#if defined(QGC_GST_STREAMING)
    //-- Only perform cleanup if storage limit is enabled
    if(!_videoSettings->enableStorageLimit()->rawValue().toBool()) {
        return;
    }
    QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();
    QDir videoDir = QDir(savePath);
    videoDir.setFilter(QDir::Files | QDir::Readable | QDir::NoSymLinks | QDir::Writable);
    videoDir.setSorting(QDir::Time);

    QStringList nameFilters;

    for(size_t i = 0; i < sizeof(kFileExtension) / sizeof(kFileExtension[0]); i += 1) {
        nameFilters << QString("*.") + kFileExtension[i];
    }

    videoDir.setNameFilters(nameFilters);
    //-- get the list of videos stored
    QFileInfoList vidList = videoDir.entryInfoList();
    if(!vidList.isEmpty()) {
        uint64_t total   = 0;
        //-- Settings are stored using MB
        uint64_t maxSize = _videoSettings->maxVideoSize()->rawValue().toUInt() * 1024 * 1024;
        //-- Compute total used storage
        for(int i = 0; i < vidList.size(); i++) {
            total += vidList[i].size();
        }
        //-- Remove old movies until max size is satisfied.
        while(total >= maxSize && !vidList.isEmpty()) {
            total -= vidList.last().size();
            qCDebug(VideoManagerLog) << "Removing old video file:" << vidList.last().filePath();
            QFile file (vidList.last().filePath());
            file.remove();
            vidList.removeLast();
        }
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoManager::startVideo(unsigned id)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }

    if(!_videoSettings->streamEnabled()->rawValue().toBool() || !_videoSettings->streamConfigured()) {
        qCDebug(VideoManagerLog) << "Stream not enabled/configured";
        return;
    }

    _startReceiver(id);
}

//-----------------------------------------------------------------------------
void
VideoManager::stopVideo(unsigned id)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }
    _stopReceiver(id);
}

void
VideoManager::startRecording(const QString& videoFile)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }
#if defined(QGC_GST_STREAMING)
    if (!_videoReceiver[0]) {
        qgcApp()->showMessage(tr("Video receiver is not ready."));
    }

    if (!_videoReceiver[1]) {
        qgcApp()->showMessage(tr("Video receiver 2 is not ready."));
    }
    if (!_videoReceiver[2]) {
        qgcApp()->showMessage(tr("Video receiver 3 is not ready."));
    }
    if (!_videoReceiver[3]) {
        qgcApp()->showMessage(tr("Video receiver 4 is not ready."));
    }


    if(!_videoReceiver[0] ) {
        return;
    }

    const VideoReceiver::FILE_FORMAT fileFormat = static_cast<VideoReceiver::FILE_FORMAT>(_videoSettings->recordingFormat()->rawValue().toInt());

    if(fileFormat < VideoReceiver::FILE_FORMAT_MIN || fileFormat >= VideoReceiver::FILE_FORMAT_MAX) {
        qgcApp()->showMessage(tr("Invalid video format defined."));
        return;
    }
    QString ext = kFileExtension[fileFormat - VideoReceiver::FILE_FORMAT_MIN];

    //-- Disk usage maintenance
    _cleanupOldVideos();

    QString savePath = qgcApp()->toolbox()->settingsManager()->appSettings()->videoSavePath();

    if (savePath.isEmpty()) {
        qgcApp()->showMessage(tr("Unabled to record video. Video save path must be specified in Settings."));
        return;
    }

    _videoFile = savePath + "/"
            + (videoFile.isEmpty() ? QDateTime::currentDateTime().toString("yyyy-MM-dd_hh.mm.ss") : videoFile)
            + ".";
    QString videoFile2 = _videoFile + "2." + ext;
    QString videoFile3 = _videoFile + "3." + ext;
    QString videoFile4 = _videoFile + "4." + ext;
    QString videoFile5 = _videoFile + "5." + ext;

    _videoFile += ext;

    if (_videoReceiver[0] && _videoStarted[0]) {
        _videoReceiver[0]->startRecording(_videoFile, fileFormat);
    }
    if (_videoReceiver[1] && _videoStarted[1]) {
        _videoReceiver[1]->startRecording(videoFile2, fileFormat);
    }
    if (_videoReceiver[2] && _videoStarted[2]) {
        _videoReceiver[2]->startRecording(videoFile3, fileFormat);
    }
    if (_videoReceiver[3] && _videoStarted[3]) {
        _videoReceiver[3]->startRecording(videoFile4, fileFormat);
    }
    if (_videoReceiver[4] && _videoStarted[4]) {
        _videoReceiver[4]->startRecording(videoFile5, fileFormat);
    }

#else
    Q_UNUSED(videoFile)
#endif
}

void
VideoManager::stopRecording()
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }
#if defined(QGC_GST_STREAMING)

    for (int i = 0; i < 5; i++) {
        if (_videoReceiver[i]) {
            _videoReceiver[i]->stopRecording();
        }
    }
#endif
}

void
VideoManager::grabImage(const QString& imageFile)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }
#if defined(QGC_GST_STREAMING)
    if (_videoReceiver[0]) {
        _imageFile = imageFile;

        emit imageFileChanged();

        _videoReceiver[0]->takeScreenshot(_imageFile);
    }
    else {
        if (_videoReceiver[1]) {
            _imageFile = imageFile;

            emit imageFileChanged();

            _videoReceiver[1]->takeScreenshot(_imageFile);
        }
    }

#else
    Q_UNUSED(imageFile)
#endif
}

//-----------------------------------------------------------------------------
double VideoManager::aspectRatio()
{
    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->currentStreamInstance();
        if(pInfo) {
            qCDebug(VideoManagerLog) << "Primary AR: " << pInfo->aspectRatio();
            return pInfo->aspectRatio();
        }
    }
    // FIXME: AV: use _videoReceiver->videoSize() to calculate AR (if AR is not specified in the settings?)
    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

//-----------------------------------------------------------------------------
double VideoManager::thermalAspectRatio()
{
    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->thermalStreamInstance();
        if(pInfo) {
            qCDebug(VideoManagerLog) << "Thermal AR: " << pInfo->aspectRatio();
            return pInfo->aspectRatio();
        }
    }
    return 1.0;
}

//-----------------------------------------------------------------------------
double VideoManager::hfov()
{
    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->currentStreamInstance();
        if(pInfo) {
            return pInfo->hfov();
        }
    }
    return 1.0;
}

//-----------------------------------------------------------------------------
double VideoManager::thermalHfov()
{
    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->thermalStreamInstance();
        if(pInfo) {
            return pInfo->aspectRatio();
        }
    }
    return _videoSettings->aspectRatio()->rawValue().toDouble();
}

//-----------------------------------------------------------------------------
bool
VideoManager::hasThermal()
{
    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->thermalStreamInstance();
        if(pInfo) {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
QString
VideoManager::imageFile()
{
    return _imageFile;
}

//-----------------------------------------------------------------------------
bool
VideoManager::autoStreamConfigured()
{
#if defined(QGC_GST_STREAMING)
    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->currentStreamInstance();
        if(pInfo) {
            return !pInfo->uri().isEmpty();
        }
    }
#endif
    return false;
}

//-----------------------------------------------------------------------------
void
VideoManager::_updateUVC()
{
#ifndef QGC_DISABLE_UVC
    QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    QList<QCameraInfo> cameras = QCameraInfo::availableCameras();
    for (const QCameraInfo &cameraInfo: cameras) {
        if(cameraInfo.description() == videoSource) {
            _videoSourceID = cameraInfo.deviceName();
            emit videoSourceIDChanged();
            qCDebug(VideoManagerLog) << "Found USB source:" << _videoSourceID << " Name:" << videoSource;
            break;
        }
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoManager::_videoSourceChanged()
{
    _updateUVC();
    emit hasVideoChanged();
    emit isGStreamerChanged();
    emit isAutoStreamChanged();
    _restartVideo(0);
}

void
VideoManager::_videoSourceChanged2()
{
    emit hasVideoChanged2();
    emit isGStreamerChanged2();
    emit isAutoStreamChanged2();
    _restartVideo(1);
}
void VideoManager::_videoSourceChanged3()
{
    emit hasVideoChanged3();
    emit isGStreamerChanged3();
    emit isAutoStreamChanged3();
    _restartVideo(2);
}
void VideoManager::_videoSourceChanged4()
{
    emit hasVideoChanged4();
    emit isGStreamerChanged4();
    emit isAutoStreamChanged4();
    _restartVideo(3);
}
//-----------------------------------------------------------------------------
void
VideoManager::_udpPortChanged()
{
    _restartVideo(0);
//    _restartVideo(1);
}

//-----------------------------------------------------------------------------
void
VideoManager::_rtspUrlChanged()
{
    _restartVideo(0);
}

void
VideoManager::_rtspUrlChanged2()
{
    _restartVideo(1);
}
void VideoManager::_rtspUrlChanged3()
{
    _restartVideo(2);
}
void VideoManager::_rtspUrlChanged4()
{
    _restartVideo(3);
}

//-----------------------------------------------------------------------------
void
VideoManager::_tcpUrlChanged()
{
    _restartVideo(0);
//    _restartVideo(1);
}

//-----------------------------------------------------------------------------
void
VideoManager::_lowLatencyModeChanged()
{
    _restartAllVideos();
}

//-----------------------------------------------------------------------------
bool
VideoManager::hasVideo()
{
    if(autoStreamConfigured()) {
        return true;
    }
    QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return !videoSource.isEmpty() && videoSource != VideoSettings::videoSourceNoVideo && videoSource != VideoSettings::videoDisabled;
}

bool
VideoManager::hasVideo2()
{
    if(autoStreamConfigured()) {
        return true;
    }
    QString videoSource2 = _videoSettings->videoSource2()->rawValue().toString();
    return !videoSource2.isEmpty() && videoSource2 != VideoSettings::videoSourceNoVideo && videoSource2 != VideoSettings::videoDisabled;
}
bool VideoManager::hasVideo3()
{
    if(autoStreamConfigured()) {
        return true;
    }
    QString videoSource3 = _videoSettings->videoSource3()->rawValue().toString();
    return !videoSource3.isEmpty() && videoSource3 != VideoSettings::videoSourceNoVideo && videoSource3 != VideoSettings::videoDisabled;
}
bool VideoManager::hasVideo4()
{
    if(autoStreamConfigured()) {
        return true;
    }
    QString videoSource4 = _videoSettings->videoSource4()->rawValue().toString();
    return !videoSource4.isEmpty() && videoSource4 != VideoSettings::videoSourceNoVideo && videoSource4 != VideoSettings::videoDisabled;
}

//-----------------------------------------------------------------------------
bool
VideoManager::isGStreamer()
{
#if defined(QGC_GST_STREAMING)
    QString videoSource = _videoSettings->videoSource()->rawValue().toString();
    return
        videoSource == VideoSettings::videoSourceUDPH264 ||
        videoSource == VideoSettings::videoSourceUDPH265 ||
        videoSource == VideoSettings::videoSourceRTSP ||
        videoSource == VideoSettings::videoSourceTCP ||
        videoSource == VideoSettings::videoSourceMPEGTS ||
        autoStreamConfigured();
#else
    return false;
#endif
}

bool
VideoManager::isGStreamer2()
{
#if defined(QGC_GST_STREAMING)
    QString videoSource2 = _videoSettings->videoSource2()->rawValue().toString();
    return
        videoSource2 == VideoSettings::videoSourceRTSP2 ||
        autoStreamConfigured();
#else
    return false;
#endif
}
bool VideoManager::isGStreamer3()
{
#if defined(QGC_GST_STREAMING)
    QString videoSource3 = _videoSettings->videoSource3()->rawValue().toString();
    return
        videoSource3 == VideoSettings::videoSourceRTSP3 ||
        autoStreamConfigured();
#else
    return false;
#endif
}
bool VideoManager::isGStreamer4()
{
#if defined(QGC_GST_STREAMING)
    QString videoSource4 = _videoSettings->videoSource4()->rawValue().toString();
    return
        videoSource4 == VideoSettings::videoSourceRTSP4 ||
        autoStreamConfigured();
#else
    return false;
#endif
}

//-----------------------------------------------------------------------------
#ifndef QGC_DISABLE_UVC
bool
VideoManager::uvcEnabled()
{
    return QCameraInfo::availableCameras().count() > 0;
}
#endif

//-----------------------------------------------------------------------------
void
VideoManager::setfullScreen(bool f)
{
    if(f) {
        //-- No can do if no vehicle or connection lost
        if(!_activeVehicle || _activeVehicle->connectionLost()) {
            f = false;
        }
    }
    _fullScreen = f;
    emit fullScreenChanged();
}

//-----------------------------------------------------------------------------
void
VideoManager::_initVideo()
{
#if defined(QGC_GST_STREAMING)
    QQuickItem* root = qgcApp()->mainRootWindow();

    if (root == nullptr) {
        qCDebug(VideoManagerLog) << "mainRootWindow() failed. No root window";
        return;
    }
//1
    QQuickItem* widget = root->findChild<QQuickItem*>("videoContent");

    if (widget != nullptr && _videoReceiver[0] != nullptr) {
        _videoSink[0] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
        if (_videoSink[0] != nullptr) {
            if (_videoStarted[0]) {
                _videoReceiver[0]->startDecoding(_videoSink[0]);
            }
        } else {
            qCDebug(VideoManagerLog) << "createVideoSink() failed";
        }
    } else {
        qCDebug(VideoManagerLog) << "video receiver disabled";
    }
//2
    widget = root->findChild<QQuickItem*>("video2Content");

    if (widget != nullptr && _videoReceiver[1] != nullptr) {
        _videoSink[1] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
        if (_videoSink[1] != nullptr) {
            if (_videoStarted[1]) {
                _videoReceiver[1]->startDecoding(_videoSink[1]);
            }
        } else {
            qCDebug(VideoManagerLog) << "createVideoSink2() failed";
        }
    } else {
        qCDebug(VideoManagerLog) << "video receiver 2 disabled";
    }
    //3
        widget = root->findChild<QQuickItem*>("video3Content");

        if (widget != nullptr && _videoReceiver[2] != nullptr) {
            _videoSink[2] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
            if (_videoSink[2] != nullptr) {
                if (_videoStarted[2]) {
                    _videoReceiver[2]->startDecoding(_videoSink[2]);
                }
            } else {
                qCDebug(VideoManagerLog) << "createVideoSink3() failed";
            }
        } else {
            qCDebug(VideoManagerLog) << "video receiver 3 disabled";
        }

        //4
            widget = root->findChild<QQuickItem*>("video4Content");

            if (widget != nullptr && _videoReceiver[3] != nullptr) {
                _videoSink[3] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
                if (_videoSink[3] != nullptr) {
                    if (_videoStarted[3]) {
                        _videoReceiver[3]->startDecoding(_videoSink[3]);
                    }
                } else {
                    qCDebug(VideoManagerLog) << "createVideoSink4() failed";
                }
            } else {
                qCDebug(VideoManagerLog) << "video receiver 4 disabled";
            }
            //5
    widget = root->findChild<QQuickItem*>("thermalVideo");

    if (widget != nullptr && _videoReceiver[4] != nullptr) {
        _videoSink[4] = qgcApp()->toolbox()->corePlugin()->createVideoSink(this, widget);
        if (_videoSink[4] != nullptr) {
            if (_videoStarted[4]) {
                _videoReceiver[4]->startDecoding(_videoSink[4]);
            }
        } else {
            qCDebug(VideoManagerLog) << "createVideoSink4() failed";
        }
    } else {
        qCDebug(VideoManagerLog) << "thermal video receiver disabled";
    }
#endif
}

//-----------------------------------------------------------------------------
bool
VideoManager::_updateSettings(unsigned id)
{
    if(!_videoSettings)
        return false;

    const bool lowLatencyStreaming  =_videoSettings->lowLatencyMode()->rawValue().toBool();

    bool settingsChanged = _lowLatencyStreaming[id] != lowLatencyStreaming;

    _lowLatencyStreaming[id] = lowLatencyStreaming;

    //-- Auto discovery

    if(_activeVehicle && _activeVehicle->dynamicCameras()) {
        QGCVideoStreamInfo* pInfo = _activeVehicle->dynamicCameras()->currentStreamInstance();
        if(pInfo) {
            if (id == 0) {
                qCDebug(VideoManagerLog) << "Configure primary stream:" << pInfo->uri();
                switch(pInfo->type()) {
                    case VIDEO_STREAM_TYPE_RTSP:
                        if ((settingsChanged |= _updateVideoUri(id, pInfo->uri()))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceRTSP);
                        }
                        break;
                    case VIDEO_STREAM_TYPE_TCP_MPEG:
                        if ((settingsChanged |= _updateVideoUri(id, pInfo->uri()))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceTCP);
                        }
                        break;
                    case VIDEO_STREAM_TYPE_RTPUDP:
                        if ((settingsChanged |= _updateVideoUri(id, QStringLiteral("udp://0.0.0.0:%1").arg(pInfo->uri())))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceUDPH264);
                        }
                        break;
                    case VIDEO_STREAM_TYPE_MPEG_TS_H264:
                        if ((settingsChanged |= _updateVideoUri(id, QStringLiteral("mpegts://0.0.0.0:%1").arg(pInfo->uri())))) {
                            _toolbox->settingsManager()->videoSettings()->videoSource()->setRawValue(VideoSettings::videoSourceMPEGTS);
                        }
                        break;
                    default:
                        settingsChanged |= _updateVideoUri(id, pInfo->uri());
                        break;
                }
            }
            else if (id == 4) { //-- Thermal stream (if any)
                QGCVideoStreamInfo* pTinfo = _activeVehicle->dynamicCameras()->thermalStreamInstance();
                if (pTinfo) {
                    qCDebug(VideoManagerLog) << "Configure secondary stream:" << pTinfo->uri();
                    switch(pTinfo->type()) {
                        case VIDEO_STREAM_TYPE_RTSP:
                        case VIDEO_STREAM_TYPE_TCP_MPEG:
                            settingsChanged |= _updateVideoUri(id, pTinfo->uri());
                            break;
                        case VIDEO_STREAM_TYPE_RTPUDP:
                            settingsChanged |= _updateVideoUri(id, QStringLiteral("udp://0.0.0.0:%1").arg(pTinfo->uri()));
                            break;
                        case VIDEO_STREAM_TYPE_MPEG_TS_H264:
                            settingsChanged |= _updateVideoUri(id, QStringLiteral("mpegts://0.0.0.0:%1").arg(pTinfo->uri()));
                            break;
                        default:
                            settingsChanged |= _updateVideoUri(id, pTinfo->uri());
                            break;
                    }
                }
            }
            return settingsChanged;
        }
    }
    if(id == 0) {
        QString source = _videoSettings->videoSource()->rawValue().toString();
        if (source == VideoSettings::videoSourceUDPH264)
            settingsChanged |= _updateVideoUri(0, QStringLiteral("udp://0.0.0.0:%1").arg(_videoSettings->udpPort()->rawValue().toInt()));
        else if (source == VideoSettings::videoSourceUDPH265)
            settingsChanged |= _updateVideoUri(0, QStringLiteral("udp265://0.0.0.0:%1").arg(_videoSettings->udpPort()->rawValue().toInt()));
        else if (source == VideoSettings::videoSourceMPEGTS)
            settingsChanged |= _updateVideoUri(0, QStringLiteral("mpegts://0.0.0.0:%1").arg(_videoSettings->udpPort()->rawValue().toInt()));
        else if (source == VideoSettings::videoSourceRTSP)
            settingsChanged |= _updateVideoUri(0, _videoSettings->rtspUrl()->rawValue().toString());
        else if (source == VideoSettings::videoSourceTCP)
            settingsChanged |= _updateVideoUri(0, QStringLiteral("tcp://%1").arg(_videoSettings->tcpUrl()->rawValue().toString()));
    }

    if(id == 1) {
        QString source2 = _videoSettings->videoSource2()->rawValue().toString();
        if (source2 == VideoSettings::videoSourceRTSP2)
            settingsChanged |= _updateVideoUri(1, _videoSettings->rtspUrl2()->rawValue().toString());
    }
    if(id == 2) {
        QString source3 = _videoSettings->videoSource3()->rawValue().toString();
        if (source3 == VideoSettings::videoSourceRTSP3)
            settingsChanged |= _updateVideoUri(2, _videoSettings->rtspUrl3()->rawValue().toString());
    }
    if(id == 3) {
        QString source4 = _videoSettings->videoSource4()->rawValue().toString();
        if (source4 == VideoSettings::videoSourceRTSP4)
            settingsChanged |= _updateVideoUri(3, _videoSettings->rtspUrl4()->rawValue().toString());
    }

    return settingsChanged;
}

//-----------------------------------------------------------------------------
bool
VideoManager::_updateVideoUri(unsigned id, const QString& uri)
{
#if defined(QGC_GST_TAISYNC_ENABLED) && (defined(__android__) || defined(__ios__))
    //-- Taisync on iOS or Android sends a raw h.264 stream
    if (isTaisync()) {
        if (id == 0) {
            return _updateVideoUri(0, QString("tsusb://0.0.0.0:%1").arg(TAISYNC_VIDEO_UDP_PORT));
        } if (id == 1) {
            // FIXME: AV: TAISYNC_VIDEO_UDP_PORT is used by video stream, thermal stream should go via its own proxy
            if (!_videoUri[1].isEmpty()) {
                _videoUri[1].clear();
                return true;
            } else {
                return false;
            }
        }
    }
#endif
    if (uri == _videoUri[id]) {
        return false;
    }

    _videoUri[id] = uri;

    return true;
}

//-----------------------------------------------------------------------------
void
VideoManager::_restartVideo(unsigned id)
{
    if (qgcApp()->runningUnitTests()) {
        return;
    }

#if defined(QGC_GST_STREAMING)
    bool oldLowLatencyStreaming = _lowLatencyStreaming[id];
    QString oldUri = _videoUri[id];
    _updateSettings(id);
    bool newLowLatencyStreaming = _lowLatencyStreaming[id];
    QString newUri = _videoUri[id];

    // FIXME: AV: use _updateSettings() result to check if settings were changed
    if (oldUri == newUri && oldLowLatencyStreaming == newLowLatencyStreaming && _videoStarted[id]) {
        qCDebug(VideoManagerLog) << "No sense to restart video streaming, skipped"  << id;
        return;
    }

    qCDebug(VideoManagerLog) << "Restart video streaming"  << id;

    if (_videoStarted[id]) {
        _stopReceiver(id);
    } else {
        _startReceiver(id);
    }
#endif
}

//-----------------------------------------------------------------------------
void
VideoManager::_restartAllVideos()
{
    _restartVideo(0);
    _restartVideo(1);
    _restartVideo(2);
    _restartVideo(3);
    _restartVideo(4);
}

//----------------------------------------------------------------------------------------
void
VideoManager::_startReceiver(unsigned id)
{
#if defined(QGC_GST_STREAMING)
    const unsigned timeout = _videoSettings->rtspTimeout()->rawValue().toUInt();

    if (id > 4) {
        qCDebug(VideoManagerLog) << "Unsupported receiver id" << id;
    } else if (_videoReceiver[id] != nullptr/* && _videoSink[id] != nullptr*/) {
        if (!_videoUri[id].isEmpty()) {
            qCDebug(VideoManagerLog) << "Start receiver id" << id;
            _videoReceiver[id]->start(_videoUri[id], timeout, _lowLatencyStreaming[id] ? -1 : 0);
        }
    }
#endif
}

//----------------------------------------------------------------------------------------
void
VideoManager::_stopReceiver(unsigned id)
{
#if defined(QGC_GST_STREAMING)
    if (id > 4) {
        qCDebug(VideoManagerLog) << "Unsupported receiver id" << id;
    } else if (_videoReceiver[id] != nullptr) {
        qCDebug(VideoManagerLog) << "Stop receiver id" << id;
        _videoReceiver[id]->stop();
    }
#endif
}

//----------------------------------------------------------------------------------------
void
VideoManager::_setActiveVehicle(Vehicle* vehicle)
{
    if(_activeVehicle) {
        disconnect(_activeVehicle, &Vehicle::connectionLostChanged, this, &VideoManager::_connectionLostChanged);
        if(_activeVehicle->dynamicCameras()) {
            QGCCameraControl* pCamera = _activeVehicle->dynamicCameras()->currentCameraInstance();
            if(pCamera) {
                pCamera->stopStream();
            }
            disconnect(_activeVehicle->dynamicCameras(), &QGCCameraManager::streamChanged, this, &VideoManager::_restartAllVideos);
        }
    }
    _activeVehicle = vehicle;
    if(_activeVehicle) {
        connect(_activeVehicle, &Vehicle::connectionLostChanged, this, &VideoManager::_connectionLostChanged);
        if(_activeVehicle->dynamicCameras()) {
            connect(_activeVehicle->dynamicCameras(), &QGCCameraManager::streamChanged, this, &VideoManager::_restartAllVideos);
            QGCCameraControl* pCamera = _activeVehicle->dynamicCameras()->currentCameraInstance();
            if(pCamera) {
                pCamera->resumeStream();
            }
        }
    } else {
        //-- Disable full screen video if vehicle is gone
        setfullScreen(false);
    }
    emit autoStreamConfiguredChanged();
    _restartAllVideos();
}

//----------------------------------------------------------------------------------------
void
VideoManager::_connectionLostChanged(bool connectionLost)
{
    if(connectionLost) {
        //-- Disable full screen video if connection is lost
        setfullScreen(false);
    }
}

//----------------------------------------------------------------------------------------
void
VideoManager::_aspectRatioChanged()
{
    emit aspectRatioChanged();
}
