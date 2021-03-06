/*****************************************************************************
 *
 * Copyright (C) 2008 Thomas Volkert <thomas@homer-conferencing.com>
 *
 * This software is free software.
 * Your are allowed to redistribute it and/or modify it under the terms of
 * the GNU General Public License version 2 as published by the Free Software
 * Foundation.
 *
 * This source is published in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License version 2 for more details.
 *
 * You should have received a copy of the GNU General Public License version 2
 * along with this program. Otherwise, you can write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 * Alternatively, you find an online version of the license text under
 * http://www.gnu.org/licenses/gpl-2.0.html.
 *
 *****************************************************************************/

/*
 * Purpose: Implementation of the widget for video display
 * Since:   2008-12-01
 */

/*
         Concept:
                1.) draw hour glass via the palette's backgroundRole
                2.) stop hour glass when first frame is received
                3.) deactivate background drawing and auto. update via Qt
                        -> setAutoFillBackground(false);
                        -> setAttribute(Qt::WA_NoSystemBackground, true);
                        -> setAttribute(Qt::WA_PaintOnScreen, true);
                        -> setAttribute(Qt::WA_OpaquePaintEvent, true);
                4.) draw manually the widget's content, use frame data and background color

 */

#include <Widgets/OverviewPlaylistWidget.h>
#include <Widgets/ParticipantWidget.h>
#include <Widgets/VideoWidget.h>
#include <ProcessStatisticService.h>
#include <MainWindow.h>

#include <MediaSource.h>
#include <MediaSourceFile.h>
#include <MediaSourceLogo.h>
#include <PacketStatistic.h>
#include <Logger.h>
#include <PacketStatistic.h>
#include <Dialogs/AddNetworkSinkDialog.h>
#include <Configuration.h>
#include <Meeting.h>
#include <Snippets.h>

#include <QInputDialog>
#include <QPalette>
#include <QImage>
#include <QMenu>
#include <QDockWidget>
#include <QContextMenuEvent>
#include <QFileDialog>
#include <QDir>
#include <QTime>
#include <QMutex>
#include <QPainter>
#include <QEvent>
#include <QApplication>
#include <QHostInfo>
#include <QStringList>
#include <QDesktopWidget>
#ifdef LINUX
#include <QDBusInterface>
#endif
#ifdef APPLE
#include <CoreServices/CoreServices.h>
#endif

#include <stdlib.h>
#include <vector>

namespace Homer { namespace Gui {

using namespace std;
using namespace Homer::Conference;
using namespace Homer::Monitor;

///////////////////////////////////////////////////////////////////////////////

// what is the minimum time between two consecutive resolution updates caused by changes in the media source?
#define VIDEO_WIDGET_MINIMUM_TIME_DIFF_BETWEEN_AUTO_RESOLUTION_UPDATES        1

// at what time period is timerEvent() called?
#define VIDEO_WIDGET_TIMER_PERIOD               	                        250 //ms

#define VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME       		                      3 // seconds

// how long should a OSD status message stay on the screen?
#define VIDEO_WIDGET_OSD_PERIOD                   		                      3 // seconds

// how many measurement steps do we use?
#define FPS_MEASUREMENT_STEPS                    		                 2 * 30

#define SEEK_SMALL_STEP                         		                     10 // seconds
#define SEEK_MEDIUM_STEP                        	                         60 // seconds
#define SEEK_BIG_STEP                          	                            300 // seconds
// additional seeking drift when seeking backwards
#define SEEK_BACKWARD_DRIFT                      		                      5 // seconds

// de/activate periodic widget updates even if no new input data is received
#define VIDEO_WIDGET_FORCE_PERIODIC_UPDATE_TIME	                            250 // ms, 0 means "deactivated"

//
#define VIDEO_WIDGET_MIN_TIME_PERIOD_BETWEEN_ACTIVITY_SIMULATION            500 // ms

///////////////////////////////////////////////////////////////////////////////

struct AspectRatioEntry{
    float ratio;
    float horz;
    float vert;
    string name;
};

#define VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS            10
#define ASPECT_RATIO_INDEX_ORIGINAL                     0
#define ASPECT_RATIO_INDEX_WINDOW                       (VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS -1)

AspectRatioEntry SupportedAspectRatios[VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS] = {
        {    0,    0,  0, "Original" },
        {    1,    1,  1, "   1 : 1" },
        { 1.33,    4,  3, "   4 : 3" },
        { 1.25,    5,  4, "   5 : 4" },
        { 1.77,   16,  9, "  16 : 9" },
        {  1.6,   16, 10, "  16 : 10"},
        { 2.21, 2.21,  1, "2.21 : 1" },
        { 2.35, 2.35,  1, "2.35 : 1" },
        { 2.39, 2.39,  1, "2.39 : 1" },
        {   -1,   -1, -1, QT_TRANSLATE_NOOP("Homer::Gui::VideoWidget", "Window")}
};

///////////////////////////////////////////////////////////////////////////////

#define VIDEO_EVENT_NEW_FRAME                   (QEvent::User + 1001)
#define VIDEO_EVENT_SOURCE_OPEN_ERROR           (QEvent::User + 1002)
#define VIDEO_EVENT_NEW_SOURCE                  (QEvent::User + 1003)
#define VIDEO_EVENT_NEW_SOURCE_RESOLUTION       (QEvent::User + 1004)
#define VIDEO_EVENT_NEW_SEEKING                 (QEvent::User + 1005)
#define VIDEO_EVENT_NEW_FULLSCREEN_DISPLAY      (QEvent::User + 1006)

///////////////////////////////////////////////////////////////////////////////

class VideoEvent:
    public QEvent
{
public:
    VideoEvent(int pReason, QString pDescription):QEvent(QEvent::User), mReason(pReason), mDescription(pDescription)
    {

    }
    ~VideoEvent()
    {

    }
    int GetReason()
    {
        return mReason;
    }
    QString GetDescription(){
        return mDescription;
    }

private:
    int mReason;
    QString mDescription;
};

///////////////////////////////////////////////////////////////////////////////

VideoWidget::VideoWidget(QWidget* pParent):
    QWidget(pParent)
{
	mTimeLastWidgetUpdate = QTime::currentTime();
    mCurrentFrameRate = 0;
    mLiveMarkerActive = false;
    mMosaicMode = false;
	mPaintEventCounter = 0;
    mResX = 640;
    mResY = 480;
    mVideoScaleFactor = 1.0;
    mCurrentFrameNumber = 0;
    mLastFrameNumber = 0;
    mHourGlassAngle = 0;
    mHourGlassOffset = 0;
    mCustomEventReason = 0;
    mPendingNewFrameSignals = 0;
    mShowLiveStats = false;
    mRecorderStarted = false;
    mVideoPaused = false;
    mAspectRatio = ASPECT_RATIO_INDEX_ORIGINAL;
    mVideoSourceDARHoriz = -1;
    mVideoSourceDARVert = -1;
    mVideoMirroredHorizontal = false;
    mVideoMirroredVertical = false;
    mCurrentApplicationFocusedWidget = NULL;
    mVideoSource = NULL;
    mVideoWorker = NULL;
    mMainWindow = NULL;
    mAssignedAction = NULL;
    mSmoothPresentation = CONF.GetSmoothVideoPresentation();
    mSystemStatePresentation = false;
    parentWidget()->hide();
    hide();
    mMediaFilterSystemState = NULL;
}

void VideoWidget::Init(QMainWindow* pMainWindow, ParticipantWidget *pParticipantWidget, MediaSource *pVideoSource, QMenu *pMenu, QString pName, bool pVisible)
{
    mVideoSource = pVideoSource;
    mVideoTitle = pName;
    mMainWindow = pMainWindow;
    mParticipantWidget = pParticipantWidget;

    //####################################################################
    //### create the remaining necessary menu item
    //####################################################################

    if (pMenu != NULL)
    {
        mAssignedAction = pMenu->addAction(pName);
        mAssignedAction->setCheckable(true);
        mAssignedAction->setChecked(pVisible);
        QIcon tIcon;
        tIcon.addPixmap(QPixmap(":/images/22_22/Checked.png"), QIcon::Normal, QIcon::On);
        tIcon.addPixmap(QPixmap(":/images/22_22/Unchecked.png"), QIcon::Normal, QIcon::Off);
        mAssignedAction->setIcon(tIcon);
    }
    if (mAssignedAction != NULL)
        connect(mAssignedAction, SIGNAL(triggered()), this, SLOT(ToggleVisibility()));

    //####################################################################
    //### update GUI
    //####################################################################
    setWindowTitle(pName);
    SetResolutionFormat(CIF);
    if (mVideoSource != NULL)
    {
        mVideoWorker = new VideoWorkerThread(pName, mVideoSource, this);
        mVideoWorker->start(QThread::TimeCriticalPriority);
    }

    mHourGlassTimer = new QTimer(this);
    connect(mHourGlassTimer, SIGNAL(timeout()), this, SLOT(ShowHourGlass()));
    mHourGlassTimer->start(250);
    LOG(LOG_VERBOSE, "Created hour glass timer with ID 0x%X", mHourGlassTimer->timerId());

    //####################################################################
    //### speedup video presentation by setting the following
    //####################################################################
    setAutoFillBackground(false);
	#if !defined(APPLE)
    	setAttribute(Qt::WA_NoSystemBackground, true);
    	setAttribute(Qt::WA_PaintOnScreen, true);
    	setAttribute(Qt::WA_OpaquePaintEvent, true);
	#endif

    SetVisible(pVisible);
    mNeedBackgroundUpdatesUntillNextFrame = true;

    // trigger periodic timer event
    mTimerId = startTimer(VIDEO_WIDGET_TIMER_PERIOD);
}

VideoWidget::~VideoWidget()
{
    LOG(LOG_VERBOSE, "Going to destroy video widget..");

    if (mTimerId != -1)
        killTimer(mTimerId);

	// we are going to destroy mCurrentFrame -> stop repainting now!
	setUpdatesEnabled(false);

	if (mVideoWorker != NULL)
    {
    	mVideoWorker->StopGrabber();
    	if (!mVideoWorker->wait(3000))
        {
            LOG(LOG_WARN, "Going to force termination of worker thread");
            mVideoWorker->terminate();
        }

        if (!mVideoWorker->wait(5000))
        {
            LOG(LOG_ERROR, "Termination of VideoWorker-Thread timed out");
        }
    	delete mVideoWorker;
    }
    if (mAssignedAction != NULL)
        delete mAssignedAction;

    if(mMediaFilterSystemState != NULL)
        delete mMediaFilterSystemState;

    LOG(LOG_VERBOSE, "Destroyed");
}

///////////////////////////////////////////////////////////////////////////////

int64_t sLastSendActivityToSystem = 0;
void VideoWidget::SendActivityToSystem()
{
    if (sLastSendActivityToSystem == 0)
    {// first call
        sLastSendActivityToSystem = Time::GetTimeStamp();
    }else
    {// 1+ call
        int64_t tTime = Time::GetTimeStamp();
        if (tTime < sLastSendActivityToSystem + VIDEO_WIDGET_MIN_TIME_PERIOD_BETWEEN_ACTIVITY_SIMULATION * 1000)
        {
            //LOG(LOG_VERBOSE, "SendActivityToSystem() will be skipped, because min. period is %"PRId64" ms and last call was only %"PRId64" ms ago", (int64_t)VIDEO_WIDGET_MIN_TIME_PERIOD_BETWEEN_ACTIVITY_SIMULATION, (tTime - sLastSendActivityToSystem) / 1000);
            return;
        }
        //LOG(LOG_VERBOSE, "Starting SendActivityToSystem()..");
        sLastSendActivityToSystem = tTime;
    }

    #ifdef APPLE
        UpdateSystemActivity(UsrActivity);
    #endif

	//TODO: FreeBSD?

    #ifdef WINDOWS
        if (SetThreadExecutionState(ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED) == 0)
            LOGEX(System, LOG_ERROR, "Unable to signal activity to the system");
    #endif

    #ifdef LINUX
        //The following is based on mediawidget.cpp from "kaffeine" (GPL2) project, Copyright (C) 2007-2010 Christoph Pfister <christophpfister@gmail.com>
        // KDE
        QDBusInterface("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver").call(QDBus::NoBlock, "SimulateUserActivity");

        // GNOME
        QDBusInterface("org.gnome.ScreenSaver", "/", "org.gnome.ScreenSaver").call(QDBus::NoBlock, "SimulateUserActivity");

        // X11 - deactivated because of namespace pollution regarding "Time" (conflicts between HBTime.h and some definitions in X11 headers) - is there still a need for direct X11 support?
        //XScreenSaverSuspend(QX11Info::display(), true);
    #endif
}

///////////////////////////////////////////////////////////////////////////////

void VideoWidget::closeEvent(QCloseEvent* pEvent)
{
	// are we a fullscreen widget?
	if (IsFullScreen())
    {
        LOG(LOG_VERBOSE, "Got closeEvent in VideoWidget while it is in fullscreen mode, will forward this to main window");
        pEvent->ignore();
        QApplication::postEvent(mMainWindow, new QCloseEvent());
    }else
    {
    	ToggleVisibility();
    }
}

void VideoWidget::InitializeMenuVideoSettings(QMenu *pMenu)
{
    QAction *tAction;

    GrabResolutions tGrabResolutions = mVideoSource->GetSupportedVideoGrabResolutions();
    GrabResolutions::iterator tIt;
    int tCurResX, tCurResY;
    vector<string> tRegisteredVideoSinks = mVideoSource->ListRegisteredMediaSinks();
    vector<string>::iterator tRegisteredVideoSinksIt;

    mVideoSource->GetVideoGrabResolution(tCurResX, tCurResY);

    pMenu->clear();

    //###############################################################################
    //### SCREENSHOT
    //###############################################################################
    tAction = pMenu->addAction(QPixmap(":/images/22_22/Save_Picture.png"), Homer::Gui::VideoWidget::tr("Save picture"));

    //###############################################################################
    //### RECORD
    //###############################################################################
    if (mVideoSource->SupportsRecording())
    {
        if (mVideoSource->IsRecording())
        {
            tAction = pMenu->addAction(QPixmap(":/images/22_22/AV_Stop.png"), Homer::Gui::VideoWidget::tr("Stop recording"));
            tAction->setShortcut(Qt::Key_E);
        }else
            tAction = pMenu->addAction(QPixmap(":/images/22_22/AV_Record.png"), Homer::Gui::VideoWidget::tr("Start recording"));
        if (!mVideoSource->IsRecording())
        {
            tAction = pMenu->addAction(QPixmap(":/images/22_22/AV_Record.png"), Homer::Gui::VideoWidget::tr("Quick recording"));
            tAction->setShortcut(Qt::Key_R);
        }
    }

    pMenu->addSeparator();

    //###############################################################################
    //### STREAM INFO
    //###############################################################################
    if (mShowLiveStats)
        tAction = pMenu->addAction(QPixmap(":/images/22_22/Info.png"), Homer::Gui::VideoWidget::tr("Hide source info"));
    else
        tAction = pMenu->addAction(QPixmap(":/images/22_22/Info.png"), Homer::Gui::VideoWidget::tr("Show source info"));
    tAction->setCheckable(true);
    tAction->setChecked(mShowLiveStats);
    tAction->setShortcut(Qt::Key_I);

    //###############################################################################
    //### "Video Overlay"
    //###############################################################################
    QMenu *tOverlayMenu = pMenu->addMenu(Homer::Gui::VideoWidget::tr("Video overlay"));
        //###############################################################################
        //### "Smooth presentation"
        //###############################################################################
        tAction = tOverlayMenu->addAction(Homer::Gui::VideoWidget::tr("System state"));
        tAction->setCheckable(true);
        tAction->setChecked(mSystemStatePresentation);
        tAction->setShortcut(Qt::Key_O);

        //###############################################################################
        //### "Live marker"
        //###############################################################################
        if (mVideoSource->SupportsMarking())
        {
            tOverlayMenu->addSeparator();

            tAction =  tOverlayMenu->addAction(Homer::Gui::VideoWidget::tr("Live marker"));
            tAction->setCheckable(true);
            tAction->setChecked(mVideoSource->MarkerActive());
            tAction->setShortcut(Qt::Key_K);
        }

    //###############################################################################
    //### Playback settings
    //###############################################################################
    QMenu *tPlaybackMenu = pMenu->addMenu(QPixmap(":/images/22_22/Configuration_Video.png"), Homer::Gui::VideoWidget::tr("Playback"));

            //###############################################################################
            //### "Full screen"
            //###############################################################################
            if (IsFullScreen())
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Window mode"));
            }else
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Full screen"));
            }
            QList<QKeySequence> tFSKeys;
            tFSKeys.push_back(Qt::Key_F);
            tFSKeys.push_back(Qt::Key_Escape);
            tAction->setShortcuts(tFSKeys);

            //###############################################################################
            //### "Smooth presentation"
            //###############################################################################
            if (mSmoothPresentation)
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Fast display"));
            }else
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Smooth display"));
            }
            tAction->setShortcut(Qt::Key_S);

            //###############################################################################
            //### ASPECT RATION
            //###############################################################################
            QMenu *tAspectRatioMenu = tPlaybackMenu->addMenu(Homer::Gui::VideoWidget::tr("Aspect ratio"));
            tAspectRatioMenu->menuAction()->setShortcut(Qt::Key_A);
                //###############################################################################
                //### "Keep aspect ratio"
                //###############################################################################

                for (int i = 0; i < VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS; i++)
                {
                    tAction = tAspectRatioMenu->addAction(Homer::Gui::VideoWidget::tr(SupportedAspectRatios[i].name.c_str()));
                    tAction->setCheckable(true);
                    if (mAspectRatio == i)
                        tAction->setChecked(true);
                    else
                        tAction->setChecked(false);
                }

            //###############################################################################
            //### RESOLUTIONS
            //###############################################################################
            QMenu *tResMenu = tPlaybackMenu->addMenu(Homer::Gui::VideoWidget::tr("Resolution"));
                //###############################################################################
                //### add all possible resolutions which are reported by the media source
                //###############################################################################
                if (tGrabResolutions.size())
                {
                    for (tIt = tGrabResolutions.begin(); tIt != tGrabResolutions.end(); tIt++)
                    {
                        QAction *tResAction = tResMenu->addAction(QString(tIt->Name.c_str()) + QString("  (%1 x %2)").arg(tIt->ResX, 4, 10, (const QChar)' ').arg(tIt->ResY, 4, 10, (const QChar)' '));
                        tResAction->setCheckable(true);
                        if ((tIt->ResX == tCurResX) && (tIt->ResY == tCurResY))
                            tResAction->setChecked(true);
                        else
                            tResAction->setChecked(false);
                    }
                }

            //###############################################################################
            //### SCALING
            //###############################################################################
            QMenu *tScaleMenu = tPlaybackMenu->addMenu(Homer::Gui::VideoWidget::tr("Scaling"));
            for (int i = 1; i < 5; i++)
            {
                QAction *tScaleAction = tScaleMenu->addAction(QString(" %1%").arg((int)(i * 50), 3, 10, (const QChar)' '));
                tScaleAction->setCheckable(true);
                if (IsCurrentScaleFactor((float)i / 2))
                {
                    //LOG(LOG_INFO, "Scaling factor %f matches", (float)i / 2));
                    tScaleAction->setChecked(true);
                }else
                    tScaleAction->setChecked(false);
            }

            //###############################################################################
            //### MIRRORING
            //###############################################################################
            if (mVideoMirroredHorizontal)
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Unmirror horizontally"));
                tAction->setCheckable(true);
                tAction->setChecked(true);
            }else
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Mirror horizontally"));
                tAction->setCheckable(true);
                tAction->setChecked(false);
            }

            if (mVideoMirroredVertical)
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Unmirror vertically"));
                tAction->setCheckable(true);
                tAction->setChecked(true);
            }else
            {
                tAction = tPlaybackMenu->addAction(Homer::Gui::VideoWidget::tr("Mirror vertically"));
                tAction->setCheckable(true);
                tAction->setChecked(false);
            }

    //###############################################################################
    //### STREAM RELAY
    //###############################################################################
    if(mVideoSource->SupportsRelaying())
    {
        QMenu *tVideoSinksMenu = pMenu->addMenu(QPixmap(":/images/22_22/ArrowRight.png"), Homer::Gui::VideoWidget::tr("Streaming"));
        tAction =  tVideoSinksMenu->addAction(QPixmap(":/images/22_22/Plus.png"), Homer::Gui::VideoWidget::tr("Send video"));
        QMenu *tRegisteredVideoSinksMenu = tVideoSinksMenu->addMenu(QPixmap(":/images/22_22/ArrowRight.png"), Homer::Gui::VideoWidget::tr("Running streams"));

        if (tRegisteredVideoSinks.size())
        {
            for (tRegisteredVideoSinksIt = tRegisteredVideoSinks.begin(); tRegisteredVideoSinksIt != tRegisteredVideoSinks.end(); tRegisteredVideoSinksIt++)
            {
                QAction *tSinkAction = tRegisteredVideoSinksMenu->addAction(QString(tRegisteredVideoSinksIt->c_str()));
                tSinkAction->setCheckable(true);
                tSinkAction->setChecked(true);
            }
        }
    }

    if(CONF.DebuggingEnabled())
    {
        //###############################################################################
        //### STREAM PAUSE/CONTINUE
        //###############################################################################
        QIcon tIcon10;
        if (mVideoPaused)
            tAction = pMenu->addAction(QPixmap(":/images/22_22/AV_Play.png"), Homer::Gui::VideoWidget::tr("Play source"));
        else
            tAction = pMenu->addAction(QPixmap(":/images/22_22/Exit.png"), Homer::Gui::VideoWidget::tr("Pause source"));
    }

    pMenu->addSeparator();

    //###############################################################################
    //### RESET SOURCE
    //###############################################################################
    tAction = pMenu->addAction(QPixmap(":/images/22_22/Reload.png"), Homer::Gui::VideoWidget::tr("Reset source"));
}

void VideoWidget::SelectedMenuVideoSettings(QAction *pAction)
{
    if (pAction != NULL)
    {
        GrabResolutions tGrabResolutions = mVideoSource->GetSupportedVideoGrabResolutions();
        GrabResolutions::iterator tIt;
        vector<string> tRegisteredVideoSinks = mVideoSource->ListRegisteredMediaSinks();
        vector<string>::iterator tRegisteredVideoSinksIt;

        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Show window")) == 0)
        {
            ToggleVisibility();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Close window")) == 0)
        {
            ToggleVisibility();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Reset source")) == 0)
        {
            mVideoWorker->ResetSource();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Save picture")) == 0)
        {
            SavePicture();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Stop recording")) == 0)
        {
            StopRecorder();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Start recording")) == 0)
        {
            StartRecorder();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Quick recording")) == 0)
        {
            StartRecorder(true);
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Mirror horizontally")) == 0)
        {
            mVideoMirroredHorizontal = true;
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Unmirror horizontally")) == 0)
        {
            mVideoMirroredHorizontal = false;
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Mirror vertically")) == 0)
        {
            mVideoMirroredVertical = true;
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Unmirror vertically")) == 0)
        {
            mVideoMirroredVertical = false;
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Show source info")) == 0)
        {
            mShowLiveStats = true;
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Hide source info")) == 0)
        {
            mShowLiveStats = false;
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Pause source")) == 0)
        {
            mVideoPaused = true;
            mVideoWorker->SetFrameDropping(true);
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Play source")) == 0)
        {
            mVideoPaused = false;
            mVideoWorker->SetFrameDropping(false);
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Send video")) == 0)
        {
            DialogAddNetworkSink();
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("Live marker")) == 0)
        {
            mLiveMarkerActive = !mVideoSource->MarkerActive();
            if (mLiveMarkerActive)
                setCursor(Qt::PointingHandCursor);
            else
                setCursor(Qt::ArrowCursor);
            mVideoSource->SetMarker(mLiveMarkerActive);
            return;
        }
        if (pAction->text().compare(Homer::Gui::VideoWidget::tr("System state")) == 0)
        {
            ToggleInVideoSystemState();
            return;
        }
        if ((pAction->text().compare(Homer::Gui::VideoWidget::tr("Smooth display")) == 0) || (pAction->text().compare(Homer::Gui::VideoWidget::tr("Fast display")) == 0))
        {
            ToggleSmoothPresentationMode();
            return;
        }
        if ((pAction->text().compare(Homer::Gui::VideoWidget::tr("Full screen")) == 0) || (pAction->text().compare(Homer::Gui::VideoWidget::tr("Window mode")) == 0))
        {
            ToggleFullScreenMode(!IsFullScreen());
            return;
        }

        //### CHANGE ASPECT RATO
        for (int i = 0; i < VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS; i++)
        {
            if (pAction->text().compare(Homer::Gui::VideoWidget::tr(SupportedAspectRatios[i].name.c_str())) == 0)
            {
                mAspectRatio = i;
                mNeedBackgroundUpdatesUntillNextFrame = true;
                return;
            }
        }

        //### UNREGISTER SINK
        for (tRegisteredVideoSinksIt = tRegisteredVideoSinks.begin(); tRegisteredVideoSinksIt != tRegisteredVideoSinks.end(); tRegisteredVideoSinksIt++)
        {
            if (pAction->text().compare(QString(tRegisteredVideoSinksIt->c_str())) == 0)
            {
                mVideoSource->UnregisterGeneralMediaSink((*tRegisteredVideoSinksIt));
                return;
            }
        }

        //### RESOLUTION
        if (tGrabResolutions.size())
        {
            //printf("%s\n", pAction->text().toStdString().c_str());
            for (tIt = tGrabResolutions.begin(); tIt != tGrabResolutions.end(); tIt++)
            {
                //printf("to compare: |%s| |%s|\n", (QString(tIt->Name.c_str()) + QString("  (%1 x %2)").arg(tIt->ResX).arg(tIt->ResY)).toStdString().c_str(), pAction->text().toStdString().c_str());
                if (pAction->text().compare(QString(tIt->Name.c_str()) + QString("  (%1 x %2)").arg(tIt->ResX, 4, 10, (const QChar)' ').arg(tIt->ResY, 4, 10, (const QChar)' ')) == 0)
                {
                    SetResolution(tIt->ResX, tIt->ResY);
                    return;
                }
            }
        }
        //### SCALING
        for (int i = 1; i < 5; i++)
        {
            if (pAction->text() == QString(" %1%").arg((int)(i * 50), 3, 10, (const QChar)' '))
            {
                SetScaling((float)i/2);
            }
        }
    }
}

void VideoWidget::contextMenuEvent(QContextMenuEvent *pEvent)
{
    if(IsFullScreen()){
        QMenu *tMenu = mParticipantWidget->GetMenuSettings();
        tMenu->exec(pEvent->globalPos());
    }else{
        QMenu tMenu(this);
        InitializeMenuVideoSettings(&tMenu);
        ((MainWindow*)mMainWindow)->AddGlobalContextMenu(&tMenu);

        //###############################################################################
        //### RESULTING REACTION
        //###############################################################################
        QAction* tPopupRes = tMenu.exec(pEvent->globalPos());
        if (tPopupRes != NULL)
            SelectedMenuVideoSettings(tPopupRes);
    }
}

void VideoWidget::DialogAddNetworkSink()
{
    AddNetworkSinkDialog tANSDialog(this, Homer::Gui::VideoWidget::tr("Target for video streaming"), DATA_TYPE_VIDEO, mVideoSource);

    tANSDialog.exec();
}

QStringList VideoWidget::GetVideoInfo()
{
	QStringList tVideoInfo;
	if (mVideoSource == NULL)
		return tVideoInfo;

	QString tAspectRatio = Homer::Gui::VideoWidget::tr(SupportedAspectRatios[mAspectRatio].name.c_str());

	int tHour = 0, tMin = 0, tSec = 0, tTime = mVideoSource->GetSeekPos();
    tSec = tTime % 60;
    tTime /= 60;
    tMin = tTime % 60;
    tHour = tTime / 60;

    int tMaxHour = 0, tMaxMin = 0, tMaxSec = 0, tMaxTime = mVideoSource->GetSeekEnd();
    tMaxSec = tMaxTime % 60;
    tMaxTime /= 60;
    tMaxMin = tMaxTime % 60;
    tMaxHour = tMaxTime / 60;

    //############################################
    //### Line 1: video source
    QString tLine_Source = "";
    tLine_Source = Homer::Gui::VideoWidget::tr("Source:")+ " " + mVideoWorker->GetCurrentDevice();

	//############################################
    //### Line 2: current video frame, dropped chunks, buffered packets
    QString tLine_Frame = "";
    tLine_Frame = "Frame: " + QString("%1").arg(mCurrentFrameNumber);
    if (mVideoSource->SupportsDecoderFrameStatistics())
    {
        tLine_Frame += " (";
        tLine_Frame += QString("%1*i, ").arg(mVideoSource->DecodedIFrames()) + QString("%1*p, ").arg(mVideoSource->DecodedPFrames()) + QString("%1*b").arg(mVideoSource->DecodedBFrames());
        if (mVideoSource->DecodedSFrames() > 0)
        	tLine_Frame += QString(",%1*s").arg(mVideoSource->DecodedSFrames());
        if (mVideoSource->DecodedSIFrames() > 0)
        	tLine_Frame += QString(",%1*si").arg(mVideoSource->DecodedSIFrames());
        if (mVideoSource->DecodedSPFrames() > 0)
        	tLine_Frame += QString(",%1*sp").arg(mVideoSource->DecodedSPFrames());
        if (mVideoSource->DecodedBIFrames() > 0)
        	tLine_Frame += QString(",%1*bi").arg(mVideoSource->DecodedBIFrames());
        tLine_Frame += ")";
    }

    if (mVideoSource->GetChunkDropCounter())
    {
        tLine_Frame += " (" + QString("%1").arg(mVideoSource->GetChunkDropCounter()) + " " + Homer::Gui::VideoWidget::tr("lost packets");
        tLine_Frame += (mVideoSource->GetRelativeLoss() ? (", " + QString("%1").arg(mVideoSource->GetRelativeLoss(), 2, 'f', 2, (QLatin1Char)' ') + Homer::Gui::VideoWidget::tr("% loss")) + ")" : ")");
    }
    tLine_Frame += (mVideoSource->GetFragmentBufferCounter() ? (" (" + QString("%1").arg(mVideoSource->GetFragmentBufferCounter()) + "/" + QString("%1").arg(mVideoSource->GetFragmentBufferSize()) + " " + Homer::Gui::VideoWidget::tr("buffered packets") + ")") : "");

    //############################################
    //### Line 3: FPS and pre-buffer time
    QString tLine_Fps = "";
    tLine_Fps = "Fps: " + QString("%1").arg(mCurrentFrameRate, 4, 'f', 2, ' ') + "/" + QString("%1").arg(mVideoSource->GetOutputFrameRate(), 4, 'f', 2, ' ');
    if (mVideoSource->GetFrameBufferSize() > 0)
    {
    	tLine_Fps += " (" + QString("%1").arg(mVideoSource->GetFrameBufferCounter()) + "/" + QString("%1").arg(mVideoSource->GetFrameBufferSize()) + ", " + QString("%1").arg(mVideoSource->GetFrameBufferTime(), 2, 'f', 2, (QLatin1Char)' ') + " s " + Homer::Gui::VideoWidget::tr("buffered");
    	float tPreBufferTime = mVideoSource->GetFrameBufferPreBufferingTime();
    	if (tPreBufferTime > 0)
    	    tLine_Fps += " [" + QString("%1").arg(tPreBufferTime, 2, 'f', 2, (QLatin1Char)' ') + " s " + Homer::Gui::VideoWidget::tr("pre-buffer") + "])";
    	else
    	    tLine_Fps += ")";
    }

    //############################################
    //### Line 4: video codec and resolution
    int tInputBitRate = mVideoSource->GetInputBitRate();
    QString tLine_InputCodec = "";
    QString tCodecName = QString(mVideoSource->GetSourceCodecStr().c_str());
    int tSourceResX = 0, tSourceResY = 0;
    mVideoSource->GetVideoSourceResolution(tSourceResX, tSourceResY);
    int tDARHoriz, tDARVert;
    mVideoSource->GetVideoDisplayAspectRation(tDARHoriz, tDARVert);
    tLine_InputCodec = Homer::Gui::VideoWidget::tr("Source codec:")+ " " + ((tCodecName != "") ? tCodecName : Homer::Gui::VideoWidget::tr("unknown"));
    tLine_InputCodec += " (" + QString("%1").arg(tSourceResX) + "*" + QString("%1").arg(tSourceResY);
    tLine_InputCodec += (tDARHoriz > 0 ? ", " + QString("%1").arg(tDARHoriz) + ":" + QString("%1").arg(tDARVert) : "");
    tLine_InputCodec += (tInputBitRate > 0 ? ", " + QString("%1 kbit/s").arg(tInputBitRate / 1000) : "");
    tLine_InputCodec += ", " + QString("%1").arg(mVideoSource->GetDecoderOutputFrameDelay()) + " " + Homer::Gui::VideoWidget::tr("frames delay") + ")";

    //############################################
    //### Line 5: video output
    float tAVDrift = mParticipantWidget->GetAVDrift();
    QString tLine_Playback = "";
    tLine_Playback = Homer::Gui::VideoWidget::tr("Playback:") + " " + QString("%1").arg(mCurrentFrameOutputWidth) + "*" + QString("%1").arg(mCurrentFrameOutputHeight) + " (" + tAspectRatio + ")" + (mSmoothPresentation ? Homer::Gui::VideoWidget::tr("[smoothed]") : "");
    if (tAVDrift >= 0.0)
        tLine_Playback += (tAVDrift != 0.0f ? QString(" (" + Homer::Gui::VideoWidget::tr("A/V drift:") + " +%1 s)").arg(tAVDrift, 2, 'f', 2, (QLatin1Char)' ') : "");
    else
        tLine_Playback += (tAVDrift != 0.0f ? QString(" (" + Homer::Gui::VideoWidget::tr("A/V drift:") + " %1 s)").arg(tAVDrift, 2, 'f', 2, (QLatin1Char)' ') : "");

    float tUserAVDrift = mParticipantWidget->GetUserAVDrift();
    if (tUserAVDrift != 0)
    {
        if (tUserAVDrift > 0.0)
            tLine_Playback += (tUserAVDrift != 0.0f ? QString(" [user A/V drift: +%1 s]").arg(tUserAVDrift, 2, 'f', 2, (QLatin1Char)' ') : "");
        else if (tAVDrift < 0.0)
            tLine_Playback += (tUserAVDrift != 0.0f ? QString(" [user A/V drift: %1 s]").arg(tUserAVDrift, 2, 'f', 2, (QLatin1Char)' ') : "");
    }
    float tVideoDelayAVDrift = mParticipantWidget->GetVideoDelayAVDrift();
    if (tVideoDelayAVDrift != 0)
    {
        if (tVideoDelayAVDrift > 0.0)
            tLine_Playback += (tVideoDelayAVDrift != 0.0f ? QString(" [A/V adjust: +%1 s]").arg(tVideoDelayAVDrift, 2, 'f', 2, (QLatin1Char)' ') : "");
        else if (tAVDrift < 0.0)
            tLine_Playback += (tVideoDelayAVDrift != 0.0f ? QString(" [A/V adjust: %1 s]").arg(tVideoDelayAVDrift, 2, 'f', 2, (QLatin1Char)' ') : "");
    }

    //############################################
    //### Line 6: current position within file
    QString tLine_Time = "";
    if (mVideoSource->SupportsSeeking())
    {
        tLine_Time = Homer::Gui::VideoWidget::tr("Time index:")+ " " + QString("%1:%2:%3").arg(tHour, 2, 10, (QLatin1Char)'0').arg(tMin, 2, 10, (QLatin1Char)'0').arg(tSec, 2, 10, (QLatin1Char)'0') + "/" + QString("%1:%2:%3").arg(tMaxHour, 2, 10, (QLatin1Char)'0').arg(tMaxMin, 2, 10, (QLatin1Char)'0').arg(tMaxSec, 2, 10, (QLatin1Char)'0');
    }

    //############################################
    //### Line 7: video muxer
    QString tLine_OutputCodec = "";
    QString tMuxCodecName = QString(mVideoSource->GetMuxingCodec().c_str());
    int tMuxResX = 0, tMuxResY = 0;
    mVideoSource->GetMuxingResolution(tMuxResX, tMuxResY);
    if (mVideoSource->SupportsMuxing())
    {
        tLine_OutputCodec = Homer::Gui::VideoWidget::tr("Streaming codec:")+ " " + ((tMuxCodecName != "") ? tMuxCodecName : Homer::Gui::VideoWidget::tr("unknown")) + " (" + QString("%1").arg(tMuxResX) + "*" + QString("%1").arg(tMuxResY);
        tLine_OutputCodec += ", " + QString("%1").arg(mVideoSource->GetEncoderBufferedFrames()) + " " + Homer::Gui::VideoWidget::tr("frames buffered");
        tLine_OutputCodec += (mVideoSource->GetMuxingBufferCounter() ? (", " + QString("%1").arg(mVideoSource->GetMuxingBufferCounter()) + "/" + QString("%1").arg(mVideoSource->GetMuxingBufferSize()) + " " + Homer::Gui::VideoWidget::tr("buffered frames") + ")") : ")");
    }

    //############################################
    //### Line 8: network peer
    QString tLine_Peer = "";
    QString tPeerName = mVideoWorker->GetSourceBroadcasterStreamName();
    if (tPeerName != "")
    	tLine_Peer = Homer::Gui::VideoWidget::tr("Sender:") + " " + tPeerName;
    int tSynchPackets = mVideoSource->GetSynchronizationPoints();
    if (tSynchPackets > 0)
    {
        tLine_Peer += " (" + QString("%1").arg(tSynchPackets) + " " + Homer::Gui::VideoWidget::tr("synch. packets");
        float tDelay = (float)mVideoSource->GetEndToEndDelay() / 1000;
        if (tDelay > 0)
            tLine_Peer += ", " + QString("%1").arg(tDelay, 2, 'f', 2, (QLatin1Char)' ') + " ms " + Homer::Gui::VideoWidget::tr("delay") + ")";
        else
            tLine_Peer += ")";
    }

    //############################################
    //### Line 9: current recorder position
    QString tLine_RecorderTime = "";
    if ((mVideoSource->SupportsRecording()) && (mVideoSource->IsRecording()))
    {
        int tHour = 0, tMin = 0, tSec = 0, tTime = mVideoSource->RecordingTime();
        tSec = tTime % 60;
        tTime /= 60;
        tMin = tTime % 60;
        tHour = tTime / 60;

        tLine_RecorderTime = Homer::Gui::VideoWidget::tr("Recorded:") + "  " + QString("%1:%2:%3").arg(tHour, 2, 10, (QLatin1Char)'0').arg(tMin, 2, 10, (QLatin1Char)'0').arg(tSec, 2, 10, (QLatin1Char)'0');
    }


    //derive resulting video statistic
    if (tLine_Source != "")
        tVideoInfo += tLine_Source;
    if (tLine_Frame != "")
        tVideoInfo += tLine_Frame;
    if (tLine_Fps != "")
        tVideoInfo += tLine_Fps;
    if (tLine_InputCodec != "")
        tVideoInfo += tLine_InputCodec;
    if (tLine_Playback != "")
        tVideoInfo += tLine_Playback;
    if (tLine_Time != "")
        tVideoInfo += tLine_Time;
    if (tLine_OutputCodec != "")
        tVideoInfo += tLine_OutputCodec;
    if (tLine_Peer != "")
        tVideoInfo += tLine_Peer;
    if (tLine_RecorderTime != "")
        tVideoInfo += tLine_RecorderTime;

    return tVideoInfo;
}

void VideoWidget::ShowFrame(void* pBuffer)
{
    int tMSecs = QTime::currentTime().msec();

	if (!isVisible())
        return;

    setUpdatesEnabled(false);


    //#############################################################
    //### auto-update aspect ratio based on source DAR value
    //#############################################################
    int tDARHoriz, tDARVert;
    mVideoSource->GetVideoDisplayAspectRation(tDARHoriz, tDARVert);
    if ((tDARHoriz > 0) && (tDARVert > 0))
    {
        if((tDARHoriz != mVideoSourceDARHoriz) || (tDARVert != mVideoSourceDARVert))
        {
            // change in DAR value detected
            for (int i = 0; i < VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS; i++)
            {
                if((tDARHoriz == SupportedAspectRatios[i].horz) && (tDARVert == SupportedAspectRatios[i].vert))
                {
                    LOG(LOG_WARN, "Auto-applying new DAR value: %d:%d", tDARHoriz, tDARVert);
                    mAspectRatio = i;
                    mNeedBackgroundUpdatesUntillNextFrame = true;
                }
            }

            mVideoSourceDARHoriz = tDARHoriz;
            mVideoSourceDARVert = tDARVert;
        }
    }

    //#############################################################
    //### get frame from media source and mirror it if selected
    //#############################################################
    QImage tCurrentFrame = QImage((unsigned char*)pBuffer, mResX, mResY, QImage::Format_RGB32);
    if (tCurrentFrame.isNull())
    {
        setUpdatesEnabled(true);
    	return;
    }else
    	mCurrentFrame = tCurrentFrame;

    if ((mVideoMirroredHorizontal) || (mVideoMirroredVertical))
    	mCurrentFrame = mCurrentFrame.mirrored(mVideoMirroredHorizontal, mVideoMirroredVertical);

    //#############################################################
    //### scale to the dimension of this video widget
    //#############################################################
    // keep aspect ratio if requested
	QTime tTime = QTime::currentTime();
	Qt::AspectRatioMode tAspectMode = Qt::IgnoreAspectRatio;
	float tSelectedAspectMode = SupportedAspectRatios[mAspectRatio].ratio;

	if (tSelectedAspectMode == -1 /* window */)
	{
        mCurrentFrameOutputWidth = width();
        mCurrentFrameOutputHeight = height();
	}else if (tSelectedAspectMode == 0 /* original */)
	{
        mCurrentFrameOutputWidth = width();
        mCurrentFrameOutputHeight = height();
        tAspectMode = Qt::KeepAspectRatio;
	}else
	{
        mCurrentFrameOutputWidth = mCurrentFrame.width();
        mCurrentFrameOutputHeight = (int)mCurrentFrameOutputWidth / SupportedAspectRatios[mAspectRatio].ratio; // adapt aspect ratio
	}

	// resize frame to best fitting size, related to video widget
	if ((mAspectRatio != ASPECT_RATIO_INDEX_WINDOW) && (mAspectRatio != ASPECT_RATIO_INDEX_ORIGINAL))
	{
		float tRatio = (float)width() / mCurrentFrameOutputWidth;
		int tNewFrameOutputWidth = width();
		int tNewFrameOutputHeight = (int)(tRatio * mCurrentFrameOutputHeight);
		if(tNewFrameOutputHeight > height())
		{
			tRatio = (float)height() / mCurrentFrameOutputHeight;
			mCurrentFrameOutputHeight = height();
			mCurrentFrameOutputWidth = (int)(tRatio * mCurrentFrameOutputWidth);
		}else
		{
			mCurrentFrameOutputHeight = tNewFrameOutputHeight;
			mCurrentFrameOutputWidth = tNewFrameOutputWidth;
		}
	}

	mCurrentFrame = mCurrentFrame.scaled(mCurrentFrameOutputWidth, mCurrentFrameOutputHeight, tAspectMode, mSmoothPresentation ? Qt::SmoothTransformation : Qt::FastTransformation);
	mCurrentFrameOutputWidth = mCurrentFrame.width();
	mCurrentFrameOutputHeight = mCurrentFrame.height();

    int tTimeDiff = QTime::currentTime().msecsTo(tTime);
    // did we spend too much time with transforming the image?
    if ((mSmoothPresentation) && (tTimeDiff > 1000 / 3)) // we assume min. of 3 FPS!
    {
        mSmoothPresentation = false;
        CONF.SetSmoothVideoPresentation(false);
        ShowInfo(Homer::Gui::VideoWidget::tr("System too busy"), Homer::Gui::VideoWidget::tr("Your system is too busy to do smooth transformation. Fast transformation will be used from now."));
    }

    QImage *tShownFrame = &mCurrentFrame;
    QPainter *tPainter = new QPainter(tShownFrame);

    //#############################################################
    //### draw statistics
    //#############################################################
    if (mShowLiveStats)
    {
        QFont tFont = QFont("Tahoma", 12, QFont::Bold);
        tFont.setFixedPitch(true);
        tPainter->setRenderHint(QPainter::TextAntialiasing, true);
        tPainter->setFont(tFont);

        QStringList tVideoInfo = GetVideoInfo();
        int tStatLines = tVideoInfo.size();

        // #######################
        // ### black shadow text
        // #######################
        tPainter->setPen(QColor(Qt::darkRed));
        for (int i = 0; i < tStatLines; i++)
    		tPainter->drawText(10, 41 + i * 20, tVideoInfo[i]);

        // #######################
        // ### red foreground text
        // #######################
        tPainter->setPen(QColor(Qt::red));
        for (int i = 0; i < tStatLines; i++)
    		tPainter->drawText(9, 40 + i * 20, tVideoInfo[i]);
    }

    //#############################################################
    //### draw record icon
    //#############################################################
    if (mVideoSource->IsRecording())
    {
        if (tMSecs % 500 < 250)
        {
            QPixmap tPixmap = QPixmap(":/images/22_22/AV_Record_active.png");
            tPainter->drawPixmap(10, 10, tPixmap);
        }else
        {
            QPixmap tPixmap = QPixmap(":/images/22_22/AV_Record.png");
            tPainter->drawPixmap(10, 10, tPixmap);
        }
    }

    //#############################################################
    //### draw pause icon
    //#############################################################
    if ((mVideoPaused) and (tMSecs % 500 < 250))
    {
        QPixmap tPixmap = QPixmap(":/images/22_22/AV_Paused.png");
        tPainter->drawPixmap(30, 10, tPixmap);
    }

    //#############################################################
    //### draw muted icon
    //#############################################################
	#ifdef VIDEO_WIDGET_SHOW_MUTE_STATE_IN_FULLSCREEN
    	if ((mParticipantWidget->GetAudioWorker()->GetMuteState()) and (tMSecs % 500 < 250))
		{
			QPixmap tPixmap = QPixmap(":/images/22_22/SpeakerMuted.png");
			tPainter->drawPixmap(50, 10, tPixmap);
		}
	#endif

    //#############################################################
    //### draw status text per OSD
    //#############################################################
    // are we a fullscreen widget?
    if ((IsFullScreen()) && (mOsdStatusMessage != "") && (Time::GetTimeStamp() < mOsdStatusMessageTimeout))
    {
        // define font for OSD text
        QFont tFont1 = QFont("Arial", 26, QFont::Light);
        tFont1.setFixedPitch(true);
        tPainter->setRenderHint(QPainter::TextAntialiasing, true);
        tPainter->setFont(tFont1);

        // calculate text width and height
        QFontMetrics tFm = tPainter->fontMetrics();
        int tTextWidth = tFm.width(mOsdStatusMessage);
        int tTextHeight = tFm.height();

        // select color white
        tPainter->setPen(QColor(Qt::black));

        // draw OSD text in black
        int tFrameWidth = mCurrentFrame.width();
        int tFrameHeight = mCurrentFrame.height();
        if ((tTextWidth > tFrameWidth) || (tTextHeight > tFrameHeight))
            tPainter->drawText(5, 41, mOsdStatusMessage);
        else
            tPainter->drawText((tFrameWidth - tTextWidth) / 2 + 1, tTextHeight + 1, mOsdStatusMessage);

        tPainter->setPen(QColor(Qt::white));

        // draw OSD text in black
        if ((tTextWidth > tFrameWidth) || (tTextHeight > tFrameHeight))
            tPainter->drawText(4, 40, mOsdStatusMessage);
        else
            tPainter->drawText((tFrameWidth - tTextWidth) / 2, tTextHeight, mOsdStatusMessage);
    }

    delete tPainter;
    setUpdatesEnabled(true);

    if(mNeedBackgroundUpdatesUntillNextFrame)
    {
    	mNeedBackgroundUpdatesUntillNextFrame = false;
    	mNeedBackgroundUpdate = true;
    }

    //#############################################################
    //### prevent system sleep mode
    //#############################################################
    if (CONF.GetPreventScreensaverInFullscreenMode())
    {
        if (IsFullScreen())
            SendActivityToSystem();
    }
}

void VideoWidget::ShowHourGlass()
{
    if (!isVisible())
        return;

    setUpdatesEnabled(false);

    int tWidth = width(), tHeight = height();

    //printf("Res: %d %d\n", mResX, mResY);
    mCurrentFrame = QImage(tWidth, tHeight, QImage::Format_RGB32);
    mCurrentFrame.fill(QColor(Qt::black).rgb());

    QPixmap tPixmap = QPixmap(":/images/Sandglass.png");
    if (!tPixmap.isNull())
    	tPixmap = tPixmap.scaledToHeight(40, Qt::SmoothTransformation);

    QImage *tShownFrame = &mCurrentFrame;
    QPainter *tPainter1 = new QPainter(tShownFrame);
    tPainter1->setRenderHint(QPainter::Antialiasing);

    tPainter1->save();
    tPainter1->translate((qreal)(tWidth / 2), (qreal)(tHeight / 2));

    int tHourGlassOffsetMax = 20 < (tHeight / 2 - 20) ? 20 : (tHeight / 2 - 20);
    if ((mHourGlassOffset += 2) > tHourGlassOffsetMax * 2)
        mHourGlassOffset = 0;
    if (mHourGlassOffset <= tHourGlassOffsetMax)
        tPainter1->translate(0, mHourGlassOffset);
    else
        tPainter1->translate(0, tHourGlassOffsetMax * 2 - mHourGlassOffset);

    if ((mHourGlassAngle += 10) > 360)
        mHourGlassAngle = 0;
    tPainter1->rotate(mHourGlassAngle);

    tPainter1->translate((qreal)-(tPixmap.width() / 2), (qreal)-(tPixmap.height() / 2));
    tPainter1->drawPixmap(0, 0, tPixmap);
    tPainter1->restore();

    delete tPainter1;
    setUpdatesEnabled(true);
}

void VideoWidget::InformAboutSeekingComplete()
{
    QApplication::postEvent(this, new VideoEvent(VIDEO_EVENT_NEW_SEEKING, ""));
}

void VideoWidget::InformAboutNewFullScreenDisplay()
{
    QApplication::postEvent(this, new VideoEvent(VIDEO_EVENT_NEW_FULLSCREEN_DISPLAY, ""));
}

void VideoWidget::InformAboutNewFrame()
{
	mTimeLastWidgetUpdate = QTime::currentTime();
    mPendingNewFrameSignals++;
    QApplication::postEvent(this, new VideoEvent(VIDEO_EVENT_NEW_FRAME, ""));
}

void VideoWidget::InformAboutOpenError(QString pSourceName)
{
    QApplication::postEvent(this, new VideoEvent(VIDEO_EVENT_SOURCE_OPEN_ERROR, pSourceName));
}

void VideoWidget::InformAboutNewSource()
{
    QApplication::postEvent(this, new VideoEvent(VIDEO_EVENT_NEW_SOURCE, ""));
}

void VideoWidget::InformAboutNewSourceResolution()
{
    QApplication::postEvent(this, new VideoEvent(VIDEO_EVENT_NEW_SOURCE_RESOLUTION, ""));
}

bool VideoWidget::SetOriginalResolution()
{
	bool tResult = false;

    GrabResolutions tGrabResolutions = mVideoSource->GetSupportedVideoGrabResolutions();
    GrabResolutions::iterator tIt;
    if (tGrabResolutions.size())
    {
        for (tIt = tGrabResolutions.begin(); tIt != tGrabResolutions.end(); tIt++)
        {
            //LOG(LOG_ERROR, "Res: %s", tIt->Name.c_str());
            if (tIt->Name == "Original")
            {
            	tResult = true;
            	LOG(LOG_VERBOSE, "Setting original resolution %d*%d", tIt->ResX, tIt->ResY);
                SetResolution(tIt->ResX, tIt->ResY);
            }
        }
    }

    return tResult;
}

void VideoWidget::ShowOsdMessage(QString pText)
{
	mOsdStatusMessage = pText;
	mOsdStatusMessageTimeout = Time::GetTimeStamp() + VIDEO_WIDGET_OSD_PERIOD * 1000 * 1000;
}

VideoWorkerThread* VideoWidget::GetWorker()
{
    return mVideoWorker;
}

void VideoWidget::SetResolution(int mX, int mY)
{
    if (!mRecorderStarted)
    {
    	LOG(LOG_VERBOSE, "Setting video resolution to %d * %d", mX, mY);

		setUpdatesEnabled(false);
		if ((mResX != mX) || (mResY != mY))
		{
			mResX = mX;
			mResY = mY;

			if(mVideoWorker != NULL)
				mVideoWorker->SetGrabResolution(mResX, mResY);

			if (!IsFullScreen())
			{
				setMinimumSize(0, 0);//mResX, mResY);
				//resize(0, 0);
			}
		}
		setUpdatesEnabled(true);
		mNeedBackgroundUpdatesUntillNextFrame = true;
    }else
    {
		ShowInfo(Homer::Gui::VideoWidget::tr("Recording active"), Homer::Gui::VideoWidget::tr("The settings for video playback cannot be changed if the recording is active!"));
    }
}

void VideoWidget::SetScaling(float pVideoScaleFactor)
{
	LOG(LOG_VERBOSE, "Setting video scaling to %f", pVideoScaleFactor);

	if (!IsFullScreen())
	{
		int tX = mResX * pVideoScaleFactor;
		int tY = mResY * pVideoScaleFactor;
		LOG(LOG_VERBOSE, "Setting video output resolution to %d * %d", tX, tY);

		setMinimumSize(tX, tY);
		resize(0, 0);
		
		mNeedBackgroundUpdatesUntillNextFrame = true;
	}else
		LOG(LOG_VERBOSE, "SetScaling skipped because fullscreen mode detected");
}

bool VideoWidget::IsCurrentScaleFactor(float pScaleFactor)
{
	//LOG(LOG_VERBOSE, "Checking scale factor %f:  %d <=> %d, %d <=> %d", pScaleFactor, width(), (int)(mResX * pScaleFactor), height(), (int)(mResY * pScaleFactor));
	return ((width() == mResX * pScaleFactor) || (height() == mResY * pScaleFactor));
}

void VideoWidget::SetResolutionFormat(VideoFormat pFormat)
{
    int tResX;
    int tResY;

    MediaSource::VideoFormat2Resolution(pFormat, tResX, tResY);
    SetResolution(tResX, tResY);
}

void VideoWidget::ShowFullScreen(int &pPosX, int &pPosY)
{
    // get the global position of this video widget
    QPoint tWidgetMiddleAsGlobalPos = mMainWindow->pos() + mapTo(mMainWindow, pos()) + QPoint(width() / 2, height() / 2);

    // get screen that contains the largest part of he VideoWidget
	QDesktopWidget *tDesktop = new QDesktopWidget();

	// get the number of available screens
	int tAvailScreens = tDesktop->numScreens();

	int tUsedScreen = -1;
	QRect tScreenRes;
	QRect tUsedScreenRes;
	int tXDiff = INT_MAX / 2;
	int tYDiff = INT_MAX / 2;
	for (int i = 0; i < tAvailScreens; i++)
	{
	    // get screen geometry
	    tScreenRes = QApplication::desktop()->screenGeometry(i);

	    // check if the widget is actually part of the screen
	    //HINT: we can not use "tDesktop->screenNumber(this)" here because in Linux this fails to return the correct screen number
	    int tScreenMiddleX = tScreenRes.x() + tScreenRes.width() / 2;
	    int tScreenMiddleY = tScreenRes.y() + tScreenRes.height() / 2;

	    int tCurXDiff = abs(tWidgetMiddleAsGlobalPos.x() - tScreenMiddleX);
        int tCurYDiff = abs(tWidgetMiddleAsGlobalPos.y() - tScreenMiddleY);
        if (tCurXDiff + tCurYDiff < tXDiff + tYDiff)
        {
            LOG(LOG_VERBOSE, "Screen %d with diff: x=%d, y=%d", i, tCurXDiff, tCurYDiff);
            tXDiff = tCurXDiff;
            tYDiff = tCurYDiff;
            tUsedScreen = i;
            tUsedScreenRes = tScreenRes;
        }else
            LOG(LOG_VERBOSE, "Ignoring screen %d with diff: x=%d, y=%d", i, tCurXDiff, tCurYDiff);
	}

	if (tUsedScreen != -1)
	{
        // prepare and set fullscreen on the corresponding screen
        move(QPoint(tUsedScreenRes.x(), tUsedScreenRes.y()));
        resize(tUsedScreenRes.width(), tUsedScreenRes.height());
        LOG(LOG_VERBOSE, "Showing video widget (%d,%d) on screen %d in fullscreen at pos=(%d,%d) and resolution (%d*%d)", tWidgetMiddleAsGlobalPos.x(), tWidgetMiddleAsGlobalPos.y(), tUsedScreen, tUsedScreenRes.x(), tUsedScreenRes.y(), tUsedScreenRes.width(), tUsedScreenRes.height());
        pPosX = tUsedScreenRes.x();
        pPosY = tUsedScreenRes.y();
    }

	// trigger fullscreen mode (anyhow)
	showFullScreen();

    delete tDesktop;
}

void VideoWidget::ToggleFullScreenMode(bool pActive)
{
    setUpdatesEnabled(false);
    LOG(LOG_VERBOSE, "Found window state: %d", (int)windowState());
    if (!pActive)
    {// show the window normal
        setWindowFlags(windowFlags() ^ Qt::Window);
        showNormal();
        mParticipantWidget->StopFullscreenMode();
        if (cursor().shape() == Qt::BlankCursor)
        {
            FullscreenMarkUserActive();
            LOG(LOG_VERBOSE, "Showing the mouse cursor again, current timeout is %d seconds", VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME);
        }
        mMainWindow->setFocus(Qt::TabFocusReason);
    }else
    {// show the window as fullscreen picture
        setWindowFlags(windowFlags() | Qt::Window);
        mTimeOfLastMouseMove = QTime::currentTime();
        int tX = -1, tY = -1;
        ShowFullScreen(tX, tY);
        mParticipantWidget->StartFullscreenMode(tX, tY);
    }
    setUpdatesEnabled(true);
	mNeedBackgroundUpdatesUntillNextFrame = true;
}

void VideoWidget::ToggleInVideoSystemState()
{
    mSystemStatePresentation = !mSystemStatePresentation;
    LOG(LOG_VERBOSE, "Toggling in-video system state to: %d", mSystemStatePresentation);
    if (mSystemStatePresentation)
    {
        if(mMediaFilterSystemState == NULL)
        {
            mMediaFilterSystemState = new MediaFilterSystemState(mVideoSource);
            mVideoSource->RegisterMediaFilter(mMediaFilterSystemState);
        }

        ShowOsdMessage(Homer::Gui::VideoWidget::tr("system state activated"));
    }else
    {
        if(mMediaFilterSystemState != NULL)
        {
            mVideoSource->UnregisterMediaFilter(mMediaFilterSystemState);
            mMediaFilterSystemState = NULL;
        }

        ShowOsdMessage(Homer::Gui::VideoWidget::tr("system state deactivated"));
    }
}

void VideoWidget::ToggleSmoothPresentationMode()
{
    mSmoothPresentation = !mSmoothPresentation;
    if (mSmoothPresentation)
    {
    	ShowOsdMessage(Homer::Gui::VideoWidget::tr("filtering activated"));
    }else
    {
    	ShowOsdMessage(Homer::Gui::VideoWidget::tr("filtering deactivated"));
    }
}

void VideoWidget::ToggleMosaicMode(bool pActive)
{
	mMosaicMode = pActive;
}

void VideoWidget::ToggleVisibility()
{
    if (isVisible())
        SetVisible(false);
    else
        SetVisible(true);
}

void VideoWidget::SetVisible(bool pVisible)
{
    if (pVisible)
    {
        if (!isVisible())
        {
            #ifdef VIDEO_WIDGET_DROP_WHEN_INVISIBLE
                mVideoWorker->SetFrameDropping(false);
            #endif
            move(mWinPos);
            parentWidget()->show();
            show();
            if (mAssignedAction != NULL)
                mAssignedAction->setChecked(true);
        }
    }else
    {
        if (isVisible())
        {
            #ifdef VIDEO_WIDGET_DROP_WHEN_INVISIBLE
                mVideoWorker->SetFrameDropping(true);
            #endif
            mWinPos = pos();
            parentWidget()->hide();
            hide();
            if (mAssignedAction != NULL)
                mAssignedAction->setChecked(false);
        }
    }
}

void VideoWidget::SavePicture()
{
    QString tFileName = QFileDialog::getSaveFileName(this,
                                                     Homer::Gui::VideoWidget::tr("Save picture"),
                                                     CONF.GetDataDirectory() + "/Homer-Snapshot.png",
                                                     "Windows Bitmap (*.bmp);;"\
                                                     "Joint Photographic Experts Group (*.jpg);;"\
                                                     "Portable Graymap (*.pgm);;"\
                                                     "Portable Network Graphics (*.png);;"\
                                                     "Portable Pixmap (*.ppm);;"\
                                                     "X11 Bitmap (*.xbm);;"\
                                                     "X11 Pixmap (*.xpm)",
                                                     &*(new QString("Portable Network Graphics (*.png)")),
                                                     CONF_NATIVE_DIALOGS);

    if (tFileName.isEmpty())
        return;

    CONF.SetDataDirectory(tFileName.left(tFileName.lastIndexOf('/')));

    mCurrentFrame.setText("Description", "Homer-Snapshot-" + mVideoTitle);
    LOG(LOG_VERBOSE, "Going to save screenshot to %s", tFileName.toStdString().c_str());
    mCurrentFrame.save(tFileName);
}

void VideoWidget::StartRecorder(bool pQuickRecording)
{
    QString tFileName;
    if(!pQuickRecording)
        tFileName = OverviewPlaylistWidget::LetUserSelectVideoSaveFile(this, Homer::Gui::VideoWidget::tr("Save recorded video"));
    else
        tFileName = QDir::homePath() + "/Homer-" + QDate::currentDate().toString("yyyy-MM-dd") + "_" + QTime::currentTime().toString("hh-mm-ss") + ".avi";

    if (tFileName.isEmpty())
        return;

    // get the quality value from the user
    int tQuality = 100;
    if(!pQuickRecording)
    {
        bool tAck = false;
        QStringList tPosQuals;
        for (int i = 1; i < 11; i++)
            tPosQuals.append(QString("%1").arg(i * 10));
        QString tQualityStr = QInputDialog::getItem(this, Homer::Gui::VideoWidget::tr("Select recording quality"), Homer::Gui::VideoWidget::tr("Record with quality:") + "                                      ", tPosQuals, 0, false, &tAck);
        if(tQualityStr.isEmpty())
            return;

        if (!tAck)
            return;

        // convert QString to int
        bool tConvOkay = false;
        int tQuality = tQualityStr.toInt(&tConvOkay, 10);
        if (!tConvOkay)
        {
            LOG(LOG_ERROR, "Error while converting QString to int");
            return;
        }
    }

    // finally start the recording
    mVideoWorker->StartRecorder(tFileName.toStdString(), tQuality);

    //record source data
    mRecorderStarted = true;
}

void VideoWidget::StopRecorder()
{
    mVideoWorker->StopRecorder();
    mRecorderStarted = false;
}

void VideoWidget::dragEnterEvent(QDragEnterEvent *pEvent)
{
    // forward the event
    mParticipantWidget->dragEnterEvent(pEvent);
}

void VideoWidget::dropEvent(QDropEvent *pEvent)
{
    // forward the event
    mParticipantWidget->dropEvent(pEvent);
}

void VideoWidget::paintEvent(QPaintEvent *pEvent)
{
	mPaintEventCounter ++;
	#ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
		LOG(LOG_VERBOSE, "Paint event %"PRId64"", mPaintEventCounter);
	#endif

	QWidget::paintEvent(pEvent);

    QPainter tPainter(this);
    QColor tBackgroundColor;

    // selected background color depends on the window state
    if ((IsFullScreen()) || (mMosaicMode))
        tBackgroundColor = QColor(Qt::black);
    else
        tBackgroundColor = QColor(Qt::black); //QApplication::palette().brush(backgroundRole()).color();
    #ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
        tBackgroundColor = QColor((int)((long)256 * qrand() / RAND_MAX), (int)((long)256 * qrand() / RAND_MAX), (int)((long)256 * qrand() / RAND_MAX));
    #endif

    // wait until valid new frame is available to be drawn
    if (mCurrentFrame.isNull())
    {
        tPainter.fillRect(0, 0, width(), height(), tBackgroundColor);
        return;
    }

    // force background update as long as we don't have the focus -> we are maybe covered by other applications GUI
    // force background update if focus has changed
    QWidget *tWidget = QApplication::focusWidget();
    if (((tWidget == NULL) || (tWidget != mCurrentApplicationFocusedWidget)) && (!IsFullScreen()))
    {
        #ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
            LOG(LOG_VERBOSE, "Focused widget has changed, background-update enforced, focused widget: %p", tWidget);
        #endif
        mNeedBackgroundUpdate = true;
        mCurrentApplicationFocusedWidget = tWidget;
    }

    #ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
        if ((mCurrentFrame.width() > width()) || (mCurrentFrame.height() > height()))
            LOG(LOG_WARN, "Current frame is too big: %dx%d with available widget area: %dx%d", mCurrentFrame.width(), mCurrentFrame.height(), width(), height());
    #endif

//TODO: fix the repaint bugs and deactivate continuous background-painting again
//    if ((mNeedBackgroundUpdate) || (mNeedBackgroundUpdatesUntillNextFrame))
    {
        //### calculate background surrounding the current frame
        int tFrameWidth = width() - mCurrentFrame.width();
        if (tFrameWidth > 0)
        {

            tPainter.fillRect(0, 0, tFrameWidth / 2, height(), tBackgroundColor);
            tPainter.fillRect(mCurrentFrame.width() + tFrameWidth / 2, 0, tFrameWidth / 2 + 1, height(), tBackgroundColor);
        }

        int tFrameHeight = height() - mCurrentFrame.height();
        if (tFrameHeight > 0)
        {
            tPainter.fillRect(tFrameWidth / 2, 0, mCurrentFrame.width(), tFrameHeight / 2, tBackgroundColor);
            tPainter.fillRect(tFrameWidth / 2, mCurrentFrame.height() + tFrameHeight / 2, mCurrentFrame.width(), tFrameHeight / 2 + 1, tBackgroundColor);
        }
        #ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
            LOG(LOG_VERBOSE, "Background-update %d, %d", mNeedBackgroundUpdate, mNeedBackgroundUpdatesUntillNextFrame);
            LOG(LOG_VERBOSE, "Current frame size: %dx%d, widget size: %dx%d, background size: %dx%d", mCurrentFrame.width(), mCurrentFrame.height(), width(), height(), tFrameWidth, tFrameHeight);
        #endif
        mNeedBackgroundUpdate = false;
    }

    // draw only fitting new frames (otherwise we could have a race condition and a too big frame which might be drawn)
    if ((mCurrentFrame.width() <= width()) && (mCurrentFrame.height() <= height()))
        tPainter.drawImage((width() - mCurrentFrame.width()) / 2, (height() - mCurrentFrame.height()) / 2, mCurrentFrame);

    pEvent->accept();
}

void VideoWidget::resizeEvent(QResizeEvent *pEvent)
{
	// enforce an update of the currently depicted video picture
	InformAboutNewFrame();

	setUpdatesEnabled(false);
    QWidget::resizeEvent(pEvent);
    mNeedBackgroundUpdatesUntillNextFrame = true;
    pEvent->accept();
    setUpdatesEnabled(true);
}

bool VideoWidget::IsFullScreen()
{
	return ((windowState() & Qt::WindowFullScreen) != 0);
}

void VideoWidget::keyPressEvent(QKeyEvent *pEvent)
{
	//LOG(LOG_VERBOSE, "Got video window key press event with key %s(0x%x, mod: 0x%x)", pEvent->text().toStdString().c_str(), pEvent->key(), (int)pEvent->modifiers());

    if ((pEvent->key() == Qt::Key_T) && (pEvent->modifiers() == 0) && (!pEvent->isAutoRepeat()))
    {
        // forward the event to the main widget
        QCoreApplication::postEvent(mMainWindow, new QKeyEvent(QEvent::KeyPress, pEvent->key(), pEvent->modifiers(), pEvent->text()));

        return;
    }
    if (CONF.DebuggingEnabled())
    {
        if (pEvent->key() == Qt::Key_Z)
        {
            mVideoWorker->ResetSource();
            return;
        }
    }
    if ((pEvent->key() == Qt::Key_Escape) && (pEvent->modifiers() == 0) && (IsFullScreen()))
	{
        ToggleFullScreenMode(!IsFullScreen());
        pEvent->accept();
        return;
	}
    if ((pEvent->key() == Qt::Key_O) && (pEvent->modifiers() == 0))
    {
        ToggleInVideoSystemState();
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_K) && (pEvent->modifiers() == 0) && (mVideoSource->SupportsMarking()))
    {
        mLiveMarkerActive = !mVideoSource->MarkerActive();
        if (mLiveMarkerActive)
            setCursor(Qt::PointingHandCursor);
        else
            setCursor(Qt::ArrowCursor);
        mVideoSource->SetMarker(mLiveMarkerActive);
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_F)  && (pEvent->modifiers() == 0))
    {
        ToggleFullScreenMode(!IsFullScreen());
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_S) && (pEvent->modifiers() == 0))
    {
        ToggleSmoothPresentationMode();
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_E) && (pEvent->modifiers() == 0))
    {
        if (mVideoSource->IsRecording())
        {
            StopRecorder();
        }
    }
    if ((pEvent->key() == Qt::Key_R) && (pEvent->modifiers() == 0))
    {
        if(mVideoSource->SupportsRecording())
        {
            if (!mVideoSource->IsRecording())
            {
                StartRecorder(true);
            }
        }
    }
    if ((pEvent->key() == Qt::Key_I) && (pEvent->modifiers() == 0))
    {
        if (mShowLiveStats)
        	mShowLiveStats = false;
        else
        	mShowLiveStats = true;
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_M) && (pEvent->modifiers() == 0))
    {
		if (mParticipantWidget->GetAudioWorker() != NULL)
		{
            mParticipantWidget->GetAudioWorker()->SetMuteState(!mParticipantWidget->GetAudioWorker()->GetMuteState());
            if (mParticipantWidget->GetAudioWorker()->GetMuteState())
                ShowOsdMessage(Homer::Gui::VideoWidget::tr("Audio muted"));
            else
                ShowOsdMessage(Homer::Gui::VideoWidget::tr("Audio output active"));
		}
		pEvent->accept();
		return;
    }
    if (((pEvent->key() == Qt::Key_Space) || (pEvent->key() == Qt::Key_MediaTogglePlayPause) || (pEvent->key() == Qt::Key_MediaPlay) || (pEvent->key() == Qt::Key_Play)) && (pEvent->modifiers() == 0))
    {
        if ((mVideoWorker->IsPlayingFile()) || (mParticipantWidget->GetAudioWorker()->IsPlayingFile()))
        {
            if (((mVideoWorker->IsPlayingFile()) && (mParticipantWidget->isVideoFilePaused())) || ((mParticipantWidget->GetAudioWorker()->IsPlayingFile()) && (mParticipantWidget->isAudioFilePaused())))
            {
                ShowOsdMessage(Homer::Gui::VideoWidget::tr("Playing.."));
                mParticipantWidget->ActionPlayPauseMovieFile();
                pEvent->accept();
                return;
            }else
            {
                ShowOsdMessage(Homer::Gui::VideoWidget::tr("Pausing.."));
                mParticipantWidget->ActionPlayPauseMovieFile();
                pEvent->accept();
                return;
            }
        }
    }
    if((pEvent->key() == Qt::Key_MediaNext) && (pEvent->modifiers() == 0))
    {
        PLAYLISTWIDGET.PlayNext();
    }
    if((pEvent->key() == Qt::Key_MediaPrevious) && (pEvent->modifiers() == 0))
    {
        PLAYLISTWIDGET.PlayPrevious();
    }
    if((pEvent->key() == Qt::Key_VolumeMute) && (pEvent->modifiers() == 0))
    {
        mParticipantWidget->GetAudioWorker()->SetMuteState(!mParticipantWidget->GetAudioWorker()->GetMuteState());
    }
    if ((pEvent->key() == Qt::Key_Right) && (IsFullScreen()))
    {
        if (pEvent->modifiers() & Qt::ControlModifier)
            mParticipantWidget->SeekMovieFileRelative(SEEK_BIG_STEP);
        else if (pEvent->modifiers() & Qt::AltModifier)
            mParticipantWidget->SeekMovieFileRelative(SEEK_MEDIUM_STEP);
        else
            mParticipantWidget->SeekMovieFileRelative(SEEK_SMALL_STEP);
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_Left) && (IsFullScreen()))
    {
        if (pEvent->modifiers() & Qt::ControlModifier)
            mParticipantWidget->SeekMovieFileRelative(-SEEK_BIG_STEP -SEEK_BACKWARD_DRIFT);
        else if (pEvent->modifiers() & Qt::AltModifier)
            mParticipantWidget->SeekMovieFileRelative(-SEEK_MEDIUM_STEP -SEEK_BACKWARD_DRIFT);
        else
            mParticipantWidget->SeekMovieFileRelative(-SEEK_SMALL_STEP - SEEK_BACKWARD_DRIFT);
        pEvent->accept();
        return;
    }
    if ((pEvent->key() == Qt::Key_A) && (pEvent->modifiers() == 0))
    {
    	mAspectRatio++;
    	if(mAspectRatio >= VIDEO_WIDGET_SUPPORTED_ASPECT_RATIOS)
    		mAspectRatio = 0;
    	mNeedBackgroundUpdatesUntillNextFrame = true;
    	pEvent->accept();
    	return;
    }
    if ((pEvent->key() == Qt::Key_Plus) || (pEvent->key() == Qt::Key_Up))
    {
		int tOffset = 25;
		int tNewVolumeValue = mParticipantWidget->GetAudioWorker()->GetVolume() + tOffset;
		if ((tNewVolumeValue > 0) && (tNewVolumeValue <= 300))
		{
			ShowOsdMessage(Homer::Gui::VideoWidget::tr("Volume:") + " " + QString("%1 %").arg(tNewVolumeValue));
			mParticipantWidget->GetAudioWorker()->SetVolume(tNewVolumeValue);
		}
	}
    if ((pEvent->key() == Qt::Key_Minus) || (pEvent->key() == Qt::Key_Down))
    {
		int tOffset = -25;
		int tNewVolumeValue = mParticipantWidget->GetAudioWorker()->GetVolume() + tOffset;
		if ((tNewVolumeValue > 0) && (tNewVolumeValue <= 300))
		{
			ShowOsdMessage(Homer::Gui::VideoWidget::tr("Volume:") +" " + QString("%1 %").arg(tNewVolumeValue));
			mParticipantWidget->GetAudioWorker()->SetVolume(tNewVolumeValue);
		}
	}
    pEvent->accept(); // otherwise we have endless loop because Qt would redirect the event back to the participant widget (its the parent widget!) andso on
}

void VideoWidget::keyReleaseEvent(QKeyEvent *pEvent)
{
    //LOG(LOG_VERBOSE, "Got video window key release event with key %s(%d, mod: %d)", pEvent->text().toStdString().c_str(), pEvent->key(), (int)pEvent->modifiers());

    if ((pEvent->key() == Qt::Key_T) && (!pEvent->isAutoRepeat()))
    {
        // forward the event to the main widget
        QCoreApplication::postEvent(mMainWindow, new QKeyEvent(QEvent::KeyRelease, pEvent->key(), pEvent->modifiers(), pEvent->text()));

        return;
    }

    QWidget::keyReleaseEvent(pEvent);
}

void VideoWidget::mouseDoubleClickEvent(QMouseEvent *pEvent)
{
    ToggleFullScreenMode(!IsFullScreen());
    pEvent->accept();
}

void VideoWidget::wheelEvent(QWheelEvent *pEvent)
{
    int tOffset = 20;
    if (pEvent->delta() < 0)
        tOffset = -20;
    if(mParticipantWidget->GetAudioWorker()->GetVolume() < 50)
        tOffset /= 2;
    if(mParticipantWidget->GetAudioWorker()->GetVolume() < 25)
        tOffset /= 2;
    //LOG(LOG_VERBOSE, "Got new wheel event with orientation %d and delta %d, derived volume offset: %d", (int)pEvent->orientation(), pEvent->delta(), tOffset);
	if (pEvent->orientation() == Qt::Vertical)
	{
		if (mParticipantWidget->GetAudioWorker() != NULL)
		{
            int tNewVolumeValue = mParticipantWidget->GetAudioWorker()->GetVolume() + tOffset;
            if ((tNewVolumeValue >= 0) && (tNewVolumeValue <= 300))
            {
                ShowOsdMessage("Volume: " + QString("%1 %").arg(tNewVolumeValue));
                mParticipantWidget->GetAudioWorker()->SetVolume(tNewVolumeValue);
            }
		}else
			LOG(LOG_VERBOSE, "Cannot adjust audio volume because determined audio worker is invalid");
	}
}

void VideoWidget::mouseMoveEvent(QMouseEvent *pEvent)
{
	//LOG(LOG_VERBOSE, "Got video window mouse move event with mouse buttons %d and position: (%d,%d)", (int)pEvent->buttons(), pEvent->pos().x(), pEvent->pos().y());
	if ((pEvent->buttons() & Qt::LeftButton) && (!IsFullScreen()))
	{
		QPoint tPoint;
		tPoint = pEvent->globalPos() - mMovingMainWindowReferencePos;
		mMovingMainWindowReferencePos = pEvent->globalPos();
		if (mParticipantWidget->isFloating())
		{// move floating video widget
		    //LOG(LOG_VERBOSE, "Moving video window to relative position: (%d,%d)", tPoint.x(), tPoint.y());

		    if (mParticipantWidget->pos() + tPoint != mParticipantWidget->pos())
		        mParticipantWidget->move(mParticipantWidget->pos() + tPoint);
		}else
		{// move main window
		    //LOG(LOG_VERBOSE, "Moving main window to relative position: (%d,%d)", tPoint.x(), tPoint.y());

            if (mMainWindow->pos() + tPoint != mMainWindow->pos())
                mMainWindow->move(mMainWindow->pos() + tPoint);
		}
	}
    mTimeOfLastMouseMove = QTime::currentTime();
    if (cursor().shape() == Qt::BlankCursor)
    {
        FullscreenMarkUserActive();
        LOG(LOG_VERBOSE, "Showing the mouse cursor again, current timeout is %d seconds", VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME);
    }

    /* live marker */
    if (mLiveMarkerActive)
    {
        int tX = pEvent->x() - (width() - mCurrentFrame.width()) / 2;
        int tY = pEvent->y() - (height() - mCurrentFrame.height()) / 2;
        float tRelX = 100 * tX / mCurrentFrame.width();
        float tRelY = 100 * tY / mCurrentFrame.height();
        if ((tRelX >= 0) && (tRelX <= 100) && (tRelY >= 0) && (tRelY <= 100))
        {
            //LOG(LOG_WARN, "Calculated position: %d,%d, relative position in frame: %.2f, %.2f", tX, tY, tRelX, tRelY);
            mVideoSource->MoveMarker(tRelX, tRelY);
        }
    }

    QWidget::mouseMoveEvent(pEvent);
}

void VideoWidget::mousePressEvent(QMouseEvent *pEvent)
{
	if (pEvent->button() == Qt::LeftButton)
	{
		mIsMovingMainWindow = true;
		mMovingMainWindowReferencePos = pEvent->globalPos();
	}
	QWidget::mousePressEvent(pEvent);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent *pEvent)
{
	if (pEvent->button() == Qt::LeftButton)
		mIsMovingMainWindow = false;
	QWidget::mouseReleaseEvent(pEvent);
}

void VideoWidget::focusOutEvent(QFocusEvent *pEvent)
{
	mIsMovingMainWindow = false;
	QWidget::focusOutEvent(pEvent);
}

void VideoWidget::timerEvent(QTimerEvent *pEvent)
{
    #ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
        LOG(LOG_VERBOSE, "New timer event");
    #endif

	#if VIDEO_WIDGET_FORCE_PERIODIC_UPDATE_TIME > 0
        if (mTimeLastWidgetUpdate.msecsTo(QTime::currentTime()) > VIDEO_WIDGET_FORCE_PERIODIC_UPDATE_TIME)
        {
        	mTimeLastWidgetUpdate = QTime::currentTime();
        	InformAboutNewFrame();
        }
	#endif

	if (pEvent->timerId() != mTimerId)
    {
        LOG(LOG_WARN, "Qt event timer ID %d doesn't match the expected one %d", pEvent->timerId(), mTimerId);
        pEvent->ignore();
        return;
    }

    if (IsFullScreen())
    {
        int tTimeSinceLastMouseMove = mTimeOfLastMouseMove.msecsTo(QTime::currentTime());
        if (tTimeSinceLastMouseMove > VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME * 1000)
        {// mouse should be hidden
            if (cursor().shape() != Qt::BlankCursor)
            {
                FullscreenMarkUserIdle();
                LOG(LOG_VERBOSE, "Hiding the mouse cursor after timeout of %d seconds", VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME);
            }
        }else
        {// mouse should be vissible
            if (cursor().shape() == Qt::BlankCursor)
            {
                FullscreenMarkUserActive();
                LOG(LOG_VERBOSE, "Showing the mouse cursor again, current timeout is %d seconds", VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME);
            }
        }
    }else
    {
        if (cursor().shape() == Qt::BlankCursor)
        {
            FullscreenMarkUserActive();
            LOG(LOG_VERBOSE, "Showing the mouse cursor again, current timeout is %d seconds", VIDEO_WIDGET_FS_MAX_MOUSE_IDLE_TIME);
        }
    }
}

void VideoWidget::customEvent(QEvent *pEvent)
{
    void* tFrame;

    // make sure we have a user event here
    if (pEvent->type() != QEvent::User)
    {
        pEvent->ignore();
        return;
    }

    VideoEvent *tVideoEvent = (VideoEvent*)pEvent;

    switch(tVideoEvent->GetReason())
    {
        case VIDEO_EVENT_NEW_FRAME:
        	if (mPendingNewFrameSignals)
        	{
				#ifdef VIDEO_WIDGET_DEBUG_FRAMES
					if (mPendingNewFrameSignals > 2)
						LOG(LOG_VERBOSE, "System too slow?, %d pending signals about new frames", mPendingNewFrameSignals);
				#endif

				// acknowledge the event to Qt
				tVideoEvent->accept();

				mLastFrameNumber = mCurrentFrameNumber;
				// hint: we don't have to synchronize with resolution changes because Qt has only one synchronous working event loop!

				int tLoopCount = 0;
				while (mPendingNewFrameSignals)
				{
                    if (tLoopCount > 0)
                    {
                        #ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
                            if (tLoopCount > 0)
                                LOG(LOG_VERBOSE, "Called GetCurrentFrameRef() %d times", tLoopCount);
                        #endif
                        mVideoWorker->ReleaseCurrentFrameRef();
                    }
					mCurrentFrameNumber = mVideoWorker->GetCurrentFrameRef(&tFrame, &mCurrentFrameRate);
                    mPendingNewFrameSignals--;

					// video delay
					int tWorkerLastFrame = mVideoWorker->GetLastFrameNumber();
					if ((mCurrentFrameNumber != tWorkerLastFrame) && (mCurrentFrameNumber > 0) && (tWorkerLastFrame > 0))
					{
					    if (mCurrentFrameRate != 0)
					    {
	                        // video play out drift
	                        int tFrameDiff = tWorkerLastFrame - mCurrentFrameNumber;
	                        float tVideoDelay = tFrameDiff / mCurrentFrameRate;
                            #ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
	                            LOG(LOG_WARN, "We show frame %d while we already grabbed frame %d, video delay is %.2f", mCurrentFrameNumber, tWorkerLastFrame, tVideoDelay);
                            #endif
	                        mParticipantWidget->ReportVideoDelay(tVideoDelay);
					    }
					}else
                        mParticipantWidget->ReportVideoDelay(0);
                    #ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
                        LOG(LOG_VERBOSE, "We show frame %d while we already grabbed frame %d", mCurrentFrameNumber, tWorkerLastFrame);
                    #endif
                    tLoopCount++;
				}

				if (isVisible())
				{
					if (mCurrentFrameNumber > -1)
					{
						// make sure there is no hour glass anymore
						if (mHourGlassTimer->isActive())
						{
							LOG(LOG_VERBOSE, "Deactivating hour glass because first frame was received");

							mHourGlassTimer->stop();

							//#############################################################################
							//### deactivate background painting and speedup video presentation
							//### each future painting task will be managed by our own paintEvent function
							//#############################################################################
							setAutoFillBackground(false);
							#if !defined(APPLE)
								setAttribute(Qt::WA_NoSystemBackground, true);
								setAttribute(Qt::WA_PaintOnScreen, true);
								setAttribute(Qt::WA_OpaquePaintEvent, true);
							#endif
							mNeedBackgroundUpdatesUntillNextFrame = true;
						}

						// display the current video frame
						ShowFrame(tFrame);
						#ifdef VIDEO_WIDGET_DEBUG_FRAMES
							LOG(LOG_WARN, "Showing frame: %d, pending signals about new frames %d", mCurrentFrameNumber, mPendingNewFrameSignals);
						#endif

						// do we have a gap?
						if (mLastFrameNumber < mCurrentFrameNumber - 1)
						{
							#ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
								LOG(LOG_WARN, "Gap between frames, [%d->%d]", mLastFrameNumber, mCurrentFrameNumber);
							#endif
						}

						// do we have a frame order problem?
						if ((mLastFrameNumber > mCurrentFrameNumber) && (mCurrentFrameNumber  > 32 /* -1 means error, 1 is received after every reset, use "32" because of possible latencies */))
						{
							if (mLastFrameNumber - mCurrentFrameNumber == FRAME_BUFFER_SIZE -1)
								LOG(LOG_WARN, "Buffer overrun occurred, received frames in wrong order, [%d->%d]", mLastFrameNumber, mCurrentFrameNumber);
							else
								LOG(LOG_WARN, "Frames received in wrong order, [%d->%d]", mLastFrameNumber, mCurrentFrameNumber);
						}
						//if (tlFrameNumber == tFrameNumber)
							//printf("VideoWidget-unnecessary frame grabbing detected!\n");
					}else
					{
                        #ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
					        LOG(LOG_WARN, "Current frame number is invalid (%d)", mCurrentFrameNumber);
                        #endif
					}
				}
				// release the reference to the last grabbed frame
				mVideoWorker->ReleaseCurrentFrameRef();
        	}else
        	{
				#ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
        			LOG(LOG_VERBOSE, "Got signal about new frame but frame queue is already empty");
				#endif
        	}
			break;
        case VIDEO_EVENT_SOURCE_OPEN_ERROR:
            tVideoEvent->accept();
            if (tVideoEvent->GetDescription() != "")
                ShowWarning(Homer::Gui::VideoWidget::tr("Video source not available"), Homer::Gui::VideoWidget::tr("The selected video source \"") + tVideoEvent->GetDescription() + Homer::Gui::VideoWidget::tr("\" is not available. Please, select another one!"));
            else
            	ShowWarning(Homer::Gui::VideoWidget::tr("Video source not available"), Homer::Gui::VideoWidget::tr("The selected video source auto detection was not successful. Please, connect an additional video device to your hardware!"));
            break;
        case VIDEO_EVENT_NEW_SOURCE:
            tVideoEvent->accept();
            if (SetOriginalResolution())
            {
				if(!mHourGlassTimer->isActive())
				{
					LOG(LOG_VERBOSE, "Reactivating hour glass timer");
					mHourGlassTimer->start(250);
				}
            }
            SetVisible(true);
            break;
        case VIDEO_EVENT_NEW_SOURCE_RESOLUTION:
        	mNeedBackgroundUpdatesUntillNextFrame = true;
            break;
        case VIDEO_EVENT_NEW_SEEKING:
            mParticipantWidget->InformAboutVideoSeekingComplete();
            break;
        case VIDEO_EVENT_NEW_FULLSCREEN_DISPLAY:
            ToggleFullScreenMode(true);
            break;
        default:
            break;
    }
    mCustomEventReason = 0;
}

void VideoWidget::FullscreenMarkUserActive()
{
    unsetCursor();
    mParticipantWidget->ShowFullscreenMovieControls();
}

void VideoWidget::FullscreenMarkUserIdle()
{
    setCursor(Qt::BlankCursor);
    mParticipantWidget->HideFullscreenMovieControls();
}

VideoWorkerThread::VideoWorkerThread(QString pName, MediaSource *pVideoSource, VideoWidget *pVideoWidget):
    MediaSourceGrabberThread(pName, pVideoSource)
{
    LOG(LOG_VERBOSE, "Created");
    mSetGrabResolutionAsap = false;
    mWaitForFirstFrameAfterSeeking = false;
    mMissingFrames = 0;
    mResX = 352;
    mResY = 288;
    mFrameWidthLastGrabbedFrame = -1;
    mFrameHeightLastGrabbedFrame = -1;
    if (pVideoSource == NULL)
        LOG(LOG_ERROR, "Video source is NULL");
    mVideoWidget = pVideoWidget;
    mFrameCurrentIndex = FRAME_BUFFER_SIZE - 1;
    mFrameGrabIndex = 0;
    mDropFrames = false;
    mSetFullScreenDisplayAsap = false;
    mCurrentFrameRefTaken = false;
    InitFrameBuffers(Homer::Gui::VideoWorkerThread::tr(MESSAGE_WAITING_FOR_FIRST_DATA));
}

VideoWorkerThread::~VideoWorkerThread()
{
    DeinitFrameBuffers();
    LOG(LOG_VERBOSE, "Destroyed");
}

void VideoWorkerThread::InitFrameBuffers(QString pMessage)
{
    mPendingNewFrames = 0;
    for (int i = 0; i < FRAME_BUFFER_SIZE; i++)
    {
        mFrame[i] = mMediaSource->AllocChunkBuffer(mFrameSize[i], MEDIA_VIDEO);

        mFrameNumber[i] = 0;

        LOG(LOG_VERBOSE, "Initiating frame buffer %d with resolution %d*%d", i, mResX, mResY);
        QImage tFrameImage = QImage((uchar*)mFrame[i], mResX, mResY, QImage::Format_RGB32);
        QPainter *tPainter = new QPainter(&tFrameImage);
        tPainter->setRenderHint(QPainter::TextAntialiasing, false);
        tPainter->fillRect(0, 0, mResX, mResY, QColor(Qt::black));
        tPainter->setFont(QFont("Arial", 16));
        tPainter->setPen(QColor(Qt::white));
        tPainter->drawText(5, 70, pMessage);

        delete tPainter;
    }
}

void VideoWorkerThread::DeinitFrameBuffers()
{
    mPendingNewFrames = 0;
    for (int i = 0; i < FRAME_BUFFER_SIZE; i++)
    {
        LOG(LOG_VERBOSE, "Releasing chunk buffer %d at %p", i, mFrame[i]);
        mMediaSource->FreeChunkBuffer(mFrame[i]);
        mFrame[i] = NULL;
        mFrameSize[i] = 0;
    }
}

void VideoWorkerThread::SetFrameDropping(bool pDrop)
{
    mDropFrames = pDrop;
}

void VideoWorkerThread::GetGrabResolution(int &pX, int &pY)
{
    mMediaSource->GetVideoGrabResolution(pX, pY);
}

void VideoWorkerThread::SetFullScreenDisplay()
{
    mSetFullScreenDisplayAsap = true;
}

void VideoWorkerThread::SetGrabResolution(int pX, int pY)
{
    if ((mResX != pX) || (mResY != pY))
    {
    	LOG(LOG_VERBOSE, "Setting grab resolution to %d*%d", pX, pY);

    	// avoid race conditions when grab resolution is update in parallel
    	mDeliverMutex.lock();
        mDesiredResX = pX;
        mDesiredResY = pY;
        mSetGrabResolutionAsap = true;
        mGrabbingCondition.wakeAll();
        mDeliverMutex.unlock();
    }
}

VideoDevices VideoWorkerThread::GetPossibleDevices()
{
    VideoDevices tResult;

    LOG(LOG_VERBOSE, "Enumerate all video devices..");
    mMediaSource->getVideoDevices(tResult);

    return tResult;
}

void VideoWorkerThread::DoPlayNewFile()
{
    LOG(LOG_VERBOSE, "DoPlayNewFile now...");

    VideoDevices tList = GetPossibleDevices();
    VideoDevices::iterator tIt;
    bool tFound = false;

    for (tIt = tList.begin(); tIt != tList.end(); tIt++)
    {
        if (QString(tIt->Name.c_str()).contains(mDesiredFile))
        {
            tFound = true;
            break;
        }
    }

    // found something?
    if (!tFound)
    {
        LOG(LOG_VERBOSE, "Unregistering old file sources..");
        mMediaSource->DeleteAllRegisteredMediaFileSources();

        LOG(LOG_VERBOSE, "File is new, going to add..");
    	MediaSourceFile *tVSource = new MediaSourceFile(mDesiredFile.toStdString());
        if (tVSource != NULL)
        {
            VideoDevices tVList;
            tVSource->getVideoDevices(tVList);
            mMediaSource->RegisterMediaSource(tVSource);
            SetCurrentDevice(mDesiredFile);
        }
    }else{
        LOG(LOG_VERBOSE, "File is already known, we select it as video media source");
        SetCurrentDevice(mDesiredFile);
    }

    mEofReached = false;
    mPlayNewFileAsap = false;
	mPaused = false;
	mFrameTimestamps.clear();
}

void VideoWorkerThread::DoSetGrabResolution()
{
    LOG(LOG_VERBOSE, "DoSetGrabResolution now...");

    // lock
    mDeliverMutex.lock();

    // delete old frame buffers
    DeinitFrameBuffers();

    // set new resolution for frame grabbing
    mMediaSource->SetVideoGrabResolution(mDesiredResX, mDesiredResY);

    // check if resolution changed
    int tGotResX, tGotResY;
    mMediaSource->GetVideoGrabResolution(tGotResX, tGotResY);
    if ((tGotResX != mDesiredResX) || (tGotResY != mDesiredResY))
    	LOG(LOG_WARN, "Got resolution %d*%d but we requested %d*%d", tGotResX, tGotResY, mDesiredResX, mDesiredResY);

    mResX = tGotResX;
	mResY = tGotResY;

    // create new frame buffers
    InitFrameBuffers(Homer::Gui::VideoWorkerThread::tr(MESSAGE_WAITING_FOR_DATA_AFTER_RESOLUTION));

    mVideoWidget->InformAboutNewSourceResolution();
    mSetGrabResolutionAsap = false;
    mFrameTimestamps.clear();

    // unlock
    mDeliverMutex.unlock();

    LOG(LOG_VERBOSE, "..DoSetGrabResolution finished");
}

void VideoWorkerThread::DoSeek()
{
    MediaSourceGrabberThread::DoSeek();
    mWaitForFirstFrameAfterSeeking = true;
}

void VideoWorkerThread::DoSyncClock()
{
    LOG(LOG_VERBOSE, "DoSyncClock now...");

    // lock
    mDeliverMutex.lock();

    int64_t tShiftOffset = 0;
    if (mSyncClockMasterSource != NULL)
        tShiftOffset = mSyncClockMasterSource->GetSynchronizationTimestamp() - mMediaSource->GetSynchronizationTimestamp();

    LOG(LOG_VERBOSE, "Shifting time of source %s by %"PRId64"", mMediaSource->GetStreamName().c_str(), tShiftOffset);

    mSourceAvailable = mMediaSource->TimeShift(tShiftOffset);
    if(!mSourceAvailable)
    {
        if (mSyncClockMasterSource != NULL)
            LOG(LOG_WARN, "Source isn't available anymore after synch. %s with %s", mMediaSource->GetStreamName().c_str(), mSyncClockMasterSource->GetStreamName().c_str());
        else
            LOG(LOG_WARN, "Source isn't available anymore after re-calibrating %s", mMediaSource->GetStreamName().c_str());
    }
    mEofReached = false;
    mSyncClockAsap = false;
    mSeekAsap = false;
    mWaitForFirstFrameAfterSeeking = true;

    // unlock
    mDeliverMutex.unlock();

    LOG(LOG_VERBOSE, "DoSyncClock finished");
}

void VideoWorkerThread::DoSetCurrentDevice()
{
    LOG(LOG_VERBOSE, "DoSetCurrentDevice now...");
    // lock
    mDeliverMutex.lock();

    bool tNewSourceSelected = false;

    if ((mSourceAvailable = mMediaSource->SelectDevice(mDeviceName.toStdString(), MEDIA_VIDEO, tNewSourceSelected)))
    {
        LOG(LOG_VERBOSE, "We opened a new source: %d", tNewSourceSelected);

        bool tHadAlreadyInputData = false;
        for (int i = 0; i < FRAME_BUFFER_SIZE; i++)
        {
            if (mFrameNumber[i] > 0)
            {
                tHadAlreadyInputData = true;
                break;
            }
        }
        if (!tHadAlreadyInputData)
        {
            LOG(LOG_VERBOSE, "Haven't found any input data, will force a hard reset of video source");
            mMediaSource->CloseGrabDevice();
            mSourceAvailable = mMediaSource->OpenVideoGrabDevice(mResX, mResY);
            if (!mSourceAvailable)
            {
                LOG(LOG_WARN, "Video source is (temporary) not available after hard reset of media source in DoSetCurrentDevice()");
            }
        }else
        {
            if (!tNewSourceSelected)
            {
                if (mMediaSource->GetCurrentDeviceName() == mDeviceName.toStdString())
                { // do we have what we required?
                    if (mMediaSource->SupportsSeeking())
                    {
                        // seek to the beginning if we have reselected the source file
                        LOG(LOG_VERBOSE, "Seeking to the beginning of the source file");
                        mMediaSource->Seek(0);
                        mSeekAsap = false;
                    }
                    if (mResetMediaSourceAsap)
                    {
                        LOG(LOG_VERBOSE, "Haven't selected new video source, reset of current source forced");
                        mSourceAvailable = mMediaSource->Reset(MEDIA_VIDEO);
                        if (!mSourceAvailable)
                        {
                            LOG(LOG_WARN, "Video source is (temporary) not available after Reset() in DoSetCurrentDevice()");
                        }
                    }
                }else
                {
                    if (!mTryingToOpenAFile)
                        mVideoWidget->InformAboutOpenError(mDeviceName);
                    else
                        LOG(LOG_VERBOSE, "Couldn't open video file source %s", mDeviceName.toStdString().c_str());
                }
            }
        }
        // we had an source reset in every case because "SelectDevice" does this if old source was already opened
        mResetMediaSourceAsap = false;
        mPaused = false;
        mVideoWidget->InformAboutNewSource();
        LOG(LOG_VERBOSE, "Unregistering old file sources..");
        mMediaSource->DeleteAllRegisteredMediaFileSources();
    }else
    {
        if (!mSourceAvailable)
            LOG(LOG_WARN, "Video source is (temporary) not available after SelectDevice() in DoSetCurrentDevice()");
        if (!mTryingToOpenAFile)
            mVideoWidget->InformAboutOpenError(mDeviceName);
        else
            LOG(LOG_VERBOSE, "Couldn't open video file source %s", mDeviceName.toStdString().c_str());
    }

    mTryingToOpenAFile = false;
    mSetCurrentDeviceAsap = false;
    mCurrentFile = mDesiredFile;
    mFrameTimestamps.clear();

    // unlock
    mDeliverMutex.unlock();
}

void VideoWorkerThread::DoSetFullScreenDisplay()
{
    if (mLastFrameNumber > 0)
    {// switch after the first frame was received
        mSetFullScreenDisplayAsap = false;
        mVideoWidget->InformAboutNewFullScreenDisplay();
    }
}

void VideoWorkerThread::HandlePlayFileError()
{
    StopFile();
    SetCurrentDevice(MEDIA_SOURCE_HOMER_LOGO);
}

void VideoWorkerThread::HandlePlayFileSuccess()
{
    mVideoWidget->SetVisible(true);
}

int VideoWorkerThread::GetCurrentFrameRef(void **pFrame, float *pFrameRate)
{
    int tResult = -1;

    mCurrentFrameRefTaken = false;

    // lock
    if (mDeliverMutex.tryLock(100 /* try to lock for 100 ms */))
    {
        mCurrentFrameRefTaken = true;
        if ((!mSetGrabResolutionAsap) && (!mResetMediaSourceAsap))
        {
            if (mPendingNewFrames)
            {
                #ifdef VIDEO_WIDGET_DEBUG_FRAMES
                    if (mPendingNewFrames > 1)
                        LOG(LOG_VERBOSE, "Found %d pending frames", mPendingNewFrames);
                #endif

                mFrameCurrentIndex++;
                if (mFrameCurrentIndex >= FRAME_BUFFER_SIZE)
                    mFrameCurrentIndex = 0;
                mPendingNewFrames--;

                if (mFrameCurrentIndex == mFrameGrabIndex)
                {
                    #ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
                        LOG(LOG_WARN, "Current index %d is the current grab index, delivering old frame instead", mFrameCurrentIndex);
                    #endif
                    mFrameCurrentIndex--;
                    if (mFrameCurrentIndex < 0)
                        mFrameCurrentIndex = FRAME_BUFFER_SIZE - 1;

                }
            }else
            {
                mMissingFrames++;
                #ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
                    LOG(LOG_VERBOSE, "Missing new frame (%d overall missed frames), delivering old frame instead", mMissingFrames);
                #endif
            }

            CalculateFrameRate(pFrameRate);

            *pFrame = mFrame[mFrameCurrentIndex];
            tResult = mFrameNumber[mFrameCurrentIndex];
        }else
            LOG(LOG_WARN, "Can't deliver new frame, pending frames: %d, grab resolution invalid: %d, have to reset source: %d, source available: %d", mPendingNewFrames, mSetGrabResolutionAsap, mResetMediaSourceAsap, mSourceAvailable);
    }else
    {// timeout

    }

	#ifdef VIDEO_WIDGET_DEBUG_FRAMES
    	LOG(LOG_VERBOSE, "GetCurrentFrame() delivered frame %d from index %d, pending frames: %d, missing frames: %d, grab index: %d", tResult, mFrameCurrentIndex, mPendingNewFrames, mMissingFrames, mFrameGrabIndex);
	#endif

    return tResult;
}

void VideoWorkerThread::ReleaseCurrentFrameRef()
{
    if (mCurrentFrameRefTaken)
    {
        // unlock
        mDeliverMutex.unlock();
    }

    mCurrentFrameRefTaken = false;
}

int VideoWorkerThread::GetLastFrameNumber()
{
    return mLastFrameNumber;
}

VideoWidget *VideoWorkerThread::GetVideoWidget()
{
	return mVideoWidget;
}

void VideoWorkerThread::run()
{
    int tFrameSize;
    bool  tGrabSuccess;
    int tFrameNumber = -1;

    // if grabber was stopped before source has been opened this BOOL is reset
    mWorkerNeeded = true;

    // reset timestamp list
    mFrameTimestamps.clear();

    // assign default thread name
    SVC_PROCESS_STATISTIC.AssignThreadName("Video-Grabber()");

    // start the video source
    mCodec = CONF.GetVideoCodec();
    if (!(mSourceAvailable = mMediaSource->OpenVideoGrabDevice(mResX, mResY)))
    {
    	LOG(LOG_WARN, "Couldn't open video grabbing device \"%s\"", mMediaSource->GetCurrentDeviceName().c_str());
    	mVideoWidget->InformAboutOpenError(QString(mMediaSource->GetCurrentDeviceName().c_str()));
    }else
    {
        mVideoWidget->InformAboutNewSource();
    }

    mLastFrameNumber = 0;
    mTimeLastDetectedVideoResolutionChange = QTime::currentTime();

    LOG(LOG_WARN, "================ Entering main VIDEO WORKER loop for media source %s", mMediaSource->GetStreamName().c_str());
    while(mWorkerNeeded)
    {
    	// store last frame number
        mLastFrameNumber = tFrameNumber;

        // has input stream changed?
        if (mMediaSource->HasInputStreamChanged())
        {
        	LOG(LOG_WARN, "Detected source change");
        	mVideoWidget->InformAboutNewSource();
        	mResetMediaSourceAsap = true;
        }

        if (mSyncClockAsap)
            DoSyncClock();

        if (mSeekAsap)
            DoSeek();

        // play new file from disc
        if (mPlayNewFileAsap)
        	DoPlayNewFile();

        // input device
        if (mSetCurrentDeviceAsap)
            DoSetCurrentDevice();

        // input stream preferences
        if (mSetInputStreamPreferencesAsap)
            DoSetInputStreamPreferences();

        // input channel
        if(mSelectInputStreamAsap)
        	DoSelectInputStream();

        // reset video source
        if (mResetMediaSourceAsap)
            DoResetMediaSource();

        // change the resolution
        if (mSetGrabResolutionAsap)
            DoSetGrabResolution();

        // start video recording
        if (mStartRecorderAsap)
            DoStartRecorder();

        // stop video recording
        if (mStopRecorderAsap)
            DoStopRecorder();

        if (mSetFullScreenDisplayAsap)
            DoSetFullScreenDisplay();

        mGrabbingStateMutex.lock();

        if ((!mPaused) && (mSourceAvailable))
        {
            mGrabbingStateMutex.unlock();

            // set input frame size
			tFrameSize = mFrameSize[mFrameGrabIndex];

			// get new frame from video grabber
			QTime tTime = QTime::currentTime();
			tFrameNumber = mMediaSource->GrabChunk(mFrame[mFrameGrabIndex], tFrameSize, mDropFrames);
            #ifdef DEBUG_VIDEOWIDGET_PERFORMANCE
			    LOG(LOG_WARN, "Grabbing new video frame took: %d ms", tTime.msecsTo(QTime::currentTime()));
            #endif
			mEofReached = ((tFrameNumber == GRAB_RES_EOF) && (!mMediaSource->HasInputStreamChanged()));
			if (mEofReached)
			{
			    mSourceAvailable = false;
                LOG(LOG_WARN, "Got EOF signal from video source, marking VIDEO source as unavailable");
			}

			#ifdef VIDEO_WIDGET_DEBUG_FRAMES
				LOG(LOG_WARN, "Got from media source the frame %d with size of %d bytes and stored it as index %d, already pending frames: %d", tFrameNumber, tFrameSize, mFrameGrabIndex, mPendingNewFrames);
			#endif

			// do we have a valid new video frame?
			if ((tFrameNumber >= 0) && (tFrameSize > 0))
			{
			    if (mWaitForFirstFrameAfterSeeking)
			    {
			        LOG(LOG_VERBOSE, "Got first decoded frame %d after seeking", tFrameNumber);
			        mWaitForFirstFrameAfterSeeking = false;

			        // inform about new seeking position
			        mVideoWidget->InformAboutSeekingComplete();
			    }

			    // has the source resolution changed in the meantime? -> thread it as new source
			    int tSourceResX = 0, tSourceResY = 0;
			    mMediaSource->GetVideoSourceResolution(tSourceResX, tSourceResY);
			    if ((mFrameWidthLastGrabbedFrame != tSourceResX) || (mFrameHeightLastGrabbedFrame != tSourceResY))
			    {
			        if (mTimeLastDetectedVideoResolutionChange.msecsTo(QTime::currentTime()) >= VIDEO_WIDGET_MINIMUM_TIME_DIFF_BETWEEN_AUTO_RESOLUTION_UPDATES * 1000)
			        {
                        if ((tSourceResX != mResX) || (tSourceResY != mResY))
                        {
                            LOG(LOG_WARN, "Remote source changed, video resolution changed: %d*%d => %d*%d", mResX, mResY, tSourceResX, tSourceResY);

                            // reset the local media source
                            LOG(LOG_WARN, "Will reset local media source because input stream changed..");
                            mResetMediaSourceAsap = true; //DoResetMediaSource();

                            mTimeLastDetectedVideoResolutionChange = QTime::currentTime();

                            // let the GUI react on this event
                            mVideoWidget->InformAboutNewSource();
                        }
			        }
			    }
			    mFrameWidthLastGrabbedFrame = tSourceResX;
			    mFrameHeightLastGrabbedFrame = tSourceResY;

			    if (!mResetMediaSourceAsap)
			    {
                    // lock
                    mDeliverMutex.lock();

                    mFrameNumber[mFrameGrabIndex] = tFrameNumber;
                    if (mPendingNewFrames < FRAME_BUFFER_SIZE)
                    {
                        mPendingNewFrames++;
                        mVideoWidget->InformAboutNewFrame();
                    }else
                    {
						#ifdef DEBUG_VIDEOWIDGET_FRAME_DELIVERY
                    		LOG(LOG_WARN, "System too slow?, frame buffer of %d entries is full, will drop all frames, grab index: %d, current read index: %d", FRAME_BUFFER_SIZE, mFrameGrabIndex, mFrameCurrentIndex);
						#endif
                        mPendingNewFrames = 1;
                        mFrameCurrentIndex = mFrameGrabIndex -1;
                        if (mFrameCurrentIndex < 0)
                            mFrameCurrentIndex = FRAME_BUFFER_SIZE - 1;

                    }
                    mFrameGrabIndex++;
                    if (mFrameGrabIndex >= FRAME_BUFFER_SIZE)
                        mFrameGrabIndex = 0;

                    // store timestamp starting from frame number 3 to avoid peaks
                    if(tFrameNumber > 3)
                    {
                        //HINT: locking is done via mDeliverMutex!
                        mFrameTimestamps.push_back(Time::GetTimeStamp());
                        //LOG(LOG_WARN, "Time %"PRId64"", Time::GetTimeStamp());
                        while (mFrameTimestamps.size() > FPS_MEASUREMENT_STEPS)
                            mFrameTimestamps.removeFirst();
                    }

                     // unlock
                    mDeliverMutex.unlock();

                    if ((mLastFrameNumber > tFrameNumber) && (tFrameNumber > 9 /* -1 means error, 1 is received after every reset, use "9" because of possible latencies */))
                        LOG(LOG_ERROR, "Frame ordering problem detected (%d -> %d)", mLastFrameNumber, tFrameNumber);
			    }else
			        LOG(LOG_VERBOSE, "Video frame dropped because source will be immediately reset");
			}else
			{
				LOG(LOG_VERBOSE, "Invalid grabbing result: %d, frame size: %d", tFrameNumber, tFrameSize);
				if (mMediaSource->GetSourceType() != SOURCE_NETWORK)
				{// file/mem/dev based source
					usleep(100 * 1000); // check for new frames every 1/10 seconds
				}else
				{// network based source

				}
			}
        }else
        {
        	if (mSourceAvailable)
        		LOG(LOG_VERBOSE, "VideoWorkerThread is in pause state");
        	else
        		LOG(LOG_VERBOSE, "VideoWorkerThread waits for available grabbing device");

        	mGrabbingCondition.wait(&mGrabbingStateMutex);
            mGrabbingStateMutex.unlock();

        	LOG(LOG_VERBOSE, "Continuing processing");
        }
    }

    LOG(LOG_WARN, "VIDEO WORKER loop finished for media source %s <<<<<<<<<<<<<<<<", mMediaSource->GetStreamName().c_str());

    mMediaSource->CloseGrabDevice();
    mMediaSource->DeleteAllRegisteredMediaSinks();

    LOG(LOG_WARN, "VIDEO WORKER thread finished for media source %s <<<<<<<<<<<<<<<<", mMediaSource->GetStreamName().c_str());
}

///////////////////////////////////////////////////////////////////////////////

}} //namespace
