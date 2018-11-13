/*****************************************************************************
 *
 * Copyright (C) 2010 Thomas Volkert <thomas@homer-conferencing.com>
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
 * Purpose: Implementation of a desktop video source
 * Since:   2010-02-15
 */

#include <MediaSourceDesktop.h>
#include <MediaSource.h>
#include <PacketStatistic.h>
#include <ProcessStatisticService.h>
#include <Logger.h>
#include <HBThread.h>
#include <Dialogs/SegmentSelectionDialog.h>

#include <QApplication>
#include <QDesktopWidget>
#include <QPainter>
#include <QPaintDevice>
#include <QWidget>
#include <QCursor>
#include <QTime>
#include <QWaitCondition>
#include <string.h>
#include <Snippets.h>
#ifdef APPLE
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace Homer { namespace Gui {

///////////////////////////////////////////////////////////////////////////////

using namespace std;
using namespace Homer::Multimedia;
using namespace Homer::Base;
using namespace Homer::Monitor;

///////////////////////////////////////////////////////////////////////////////

#define                     MAX_WIDTH                       3840 * 3
#define                     MAX_HEIGHT                      2160 * 2

///////////////////////////////////////////////////////////////////////////////

MediaSourceDesktop::MediaSourceDesktop(string pDesiredDevice):
    MediaSource("Desktop: local capture")
{
    // set category for packet statistics
    ClassifyStream(DATA_TYPE_VIDEO, SOCKET_RAW);

    mAutoScreen = false;
    mAutoDesktop = false;
    mMouseVisualization = false;
    mWidget = NULL;
    mOutputScreenshot = NULL;
    mOriginalScreenshot = NULL;
    mSourceType = SOURCE_DEVICE;

    // reset grabbing offset values
    mGrabOffsetX = 0;
    mGrabOffsetY = 0;
    mSourceResX = DESKTOP_SEGMENT_MIN_WIDTH;
    mSourceResY = DESKTOP_SEGMENT_MIN_HEIGHT;
    mRecorderChunkNumber = 0;
    mLastTimeGrabbed = QTime(0, 0, 0, 0);

    bool tNewDeviceSelected = false;
    SelectDevice(pDesiredDevice, MEDIA_VIDEO, tNewDeviceSelected);
    if (!tNewDeviceSelected)
    {
        LOG(LOG_INFO, "Haven't selected new Qt-desktop device when creating source object");
    }
    LOG(LOG_VERBOSE, "Allocating buffer for original screenshot..");
    mOriginalScreenshot = malloc(MAX_WIDTH * MAX_HEIGHT * MSD_BYTES_PER_PIXEL * sizeof(char));
    if (mOriginalScreenshot == NULL)
        LOG(LOG_ERROR, "Original buffer allocation failed");
    mOutputScreenshot = malloc(MAX_WIDTH * MAX_HEIGHT * MSD_BYTES_PER_PIXEL * sizeof(char));
    if (mOutputScreenshot == NULL)
        LOG(LOG_ERROR, "Output buffer allocation failed");
}

MediaSourceDesktop::~MediaSourceDesktop()
{
    if (mMediaSourceOpened)
        CloseGrabDevice();

    free(mOriginalScreenshot);
    free(mOutputScreenshot);
}

void MediaSourceDesktop::getVideoDevices(VideoDevices &pVList)
{
    static bool tFirstCall = true;
    VideoDeviceDescriptor tDevice;

    #ifdef MSD_DEBUG_PACKETS
        tFirstCall = true;
    #endif

    if (tFirstCall)
        LOG(LOG_VERBOSE, "Enumerating hardware..");

    //#############################
    //### screen segment
    //#############################
    tDevice.Name = MEDIA_SOURCE_DESKTOP;
    tDevice.Card = "segment";
	#ifdef APPLE
    	tDevice.Desc = "OSX Cocoa based screen segment capturing";
	#else
		tDevice.Desc = "Qt based screen segment capturing";
	#endif
	if (tFirstCall)
        LOG(LOG_VERBOSE, "Found video device: %s (card: %s)", tDevice.Name.c_str(), tDevice.Card.c_str());
    pVList.push_back(tDevice);


    QDesktopWidget *tDesktop = QApplication::desktop();
    if (tDesktop != NULL)
    {
        if (tFirstCall)
        {
            LOG(LOG_VERBOSE, "Desktop found..");
            LOG(LOG_VERBOSE, "  ..resolution: %d * %d", tDesktop->width(), tDesktop->height());
            LOG(LOG_VERBOSE, "  ..screens: %d", tDesktop->numScreens());
            LOG(LOG_VERBOSE, "  ..virtualized: %d", tDesktop->isVirtualDesktop());
        }

        for (int i = 0; i < tDesktop->numScreens(); i++)
        {
            QWidget *tScreen = tDesktop->screen(i);
            if (tFirstCall)
            {
                LOG(LOG_VERBOSE, "  ..screen %d: resolution=%d*%d, available resolution=%d*%d, position=(%d, %d)", i, tDesktop->screenGeometry(i).width(), tDesktop->screenGeometry(i).height(), tDesktop->availableGeometry(i).width(), tDesktop->availableGeometry(i).height(), tDesktop->screenGeometry(i).x(), tDesktop->screenGeometry(i).y());
            }
        }
    }

    tFirstCall = false;
}

void MediaSourceDesktop::StopGrabbing()
{
	MediaSource::StopGrabbing();
	mWaitConditionScreenshotUpdated.wakeAll();
}

string MediaSourceDesktop::GetSourceCodecStr()
{
    return "Raw";
}

string MediaSourceDesktop::GetSourceCodecDescription()
{
    return "Raw";
}

///////////////////////////////////////////////////////////////////////////////

bool MediaSourceDesktop::OpenVideoGrabDevice(int pResX, int pResY, float pFps)
{
    int tScreenId = -1;

    LOG(LOG_VERBOSE, "Trying to open the video source");

    if (mMediaType == MEDIA_AUDIO)
    {
        LOG(LOG_ERROR, "Wrong media type detected");
        return false;
    }

    SVC_PROCESS_STATISTIC.AssignThreadName("Video-Grabber(Desktop)");

    if (mMediaSourceOpened)
        return false;

    mWidget = QApplication::desktop()->screen(0); //per default support only grabbing from screen 0
    mTargetResX = pResX;
    mTargetResY = pResY;

    mCurrentDevice = mDesiredDevice;

    // don't allow too slow grabbing
    if (pFps < MIN_GRABBING_FPS)
    	pFps = MIN_GRABBING_FPS;

	mInputFrameRate = pFps;
    mOutputFrameRate = pFps;

    LOG(LOG_INFO, "Opened...");
    LOG(LOG_INFO, "    ..fps: %3.2f", mInputFrameRate);
    LOG(LOG_INFO, "    ..fps (playout): %3.2f", mOutputFrameRate);
    LOG(LOG_INFO, "    ..device: %s", mCurrentDevice.c_str());
    LOG(LOG_INFO, "    ..resolution: %d * %d", mSourceResX, mSourceResY);
    LOG(LOG_INFO, "    ..source frame size: %d", mSourceResX * mSourceResY * MSD_BYTES_PER_PIXEL);
    LOG(LOG_INFO, "    ..destination frame size: %d", mTargetResX * mTargetResY * MSD_BYTES_PER_PIXEL);

    //######################################################
    //### initiate local variables
    //######################################################
    InitFpsEmulator();
    mLastTimeGrabbed == QTime(0, 0, 0, 0);
    mInputStartPts = 0;
    mFrameNumber = 0;
    mMediaType = MEDIA_VIDEO;
    mMediaSourceOpened = true;

    return true;
}

bool MediaSourceDesktop::OpenAudioGrabDevice(int pSampleRate, int pChannels)
{
    LOG(LOG_ERROR, "Wrong media type");
    return false;
}

bool MediaSourceDesktop::CloseGrabDevice()
{
    bool tResult = false;

    LOG(LOG_VERBOSE, "%s %s source closing..", GetMediaTypeStr().c_str(), GetSourceTypeStr().c_str());

    if (mMediaType == MEDIA_AUDIO)
    {
        LOG(LOG_ERROR, "Wrong media type");
        return false;
    }

    if (mMediaSourceOpened)
    {
        mMediaSourceOpened = false;

        // stop A/V recorder
        LOG(LOG_VERBOSE, "    ..stopping %s recorder", GetMediaTypeStr().c_str());
        StopRecording();

        mWidget = NULL;

        LOG(LOG_INFO, "...%s source closed", GetMediaTypeStr().c_str());

        tResult = true;
    }else
        LOG(LOG_INFO, "...wasn't open");

    mGrabbingStopped = false;
    mScreenshotUpdated = false;
    mMediaType = MEDIA_UNKNOWN;

    return tResult;
}

bool MediaSourceDesktop::HasVariableOutputFrameRate()
{
	return true;
}

void MediaSourceDesktop::SetVideoGrabResolution(int pResX, int pResY)
{
    mMutexGrabberActive.lock();

    MediaSource::SetVideoGrabResolution(pResX, pResY);

    mMutexGrabberActive.unlock();
}

void MediaSourceDesktop::DoSetVideoGrabResolution(int pResX, int pResY)
{
    LOG(LOG_VERBOSE, "Setting desktop grab resolution to %d*%d", pResX, pResY);

    mMutexScreenshot.lock();

    SetScreenshotSize(pResX, pResY);

    mMutexScreenshot.unlock();
}

void MediaSourceDesktop::SetScreenshotSize(int pWidth, int pHeight)
{
    if (pWidth > MAX_WIDTH)
        pWidth = MAX_WIDTH;
    if (pHeight > MAX_HEIGHT)
        pHeight = MAX_HEIGHT;

    LOG(LOG_VERBOSE, "Setting screenshot size to %d * %d", pWidth, pHeight);

    mSourceResX = pWidth;
    mSourceResY = pHeight;
    GetSupportedVideoGrabResolutions();
}

bool MediaSourceDesktop::SupportsRecording()
{
	return true;
}

void MediaSourceDesktop::StopRecording()
{
    if (mRecording)
    {
        MediaSource::StopRecording();
        mRecorderChunkNumber = 0;
    }
}

void MediaSourceDesktop::SetAutoDesktop(bool pActive)
{
	if (mAutoDesktop != pActive)
	{
		LOG(LOG_VERBOSE, "Setting auto-desktop to: %d", pActive);
		mAutoDesktop = pActive;
	}
}

bool MediaSourceDesktop::GetAutoDesktop()
{
	return mAutoDesktop;
}

void MediaSourceDesktop::SetAutoScreen(bool pActive)
{
    if (mAutoScreen != pActive)
    {
        LOG(LOG_VERBOSE, "Setting auto-screen to: %d", pActive);
        mAutoScreen = pActive;
    }
}

bool MediaSourceDesktop::GetAutoScreen()
{
    return mAutoScreen;
}

void MediaSourceDesktop::SetMouseVisualization(bool pActive)
{
	mMouseVisualization = pActive;
}

bool MediaSourceDesktop::GetMouseVisualization()
{
	return mMouseVisualization;
}

void MediaSourceDesktop::CreateScreenshot()
{
    AVFrame             *tRGBFrame;
    int 				tCaptureResX = mSourceResX;
    int					tCaptureResY = mSourceResY;

    mMutexGrabberActive.lock();

//    LOG(LOG_VERBOSE, "Source: %d * %d", mSourceResX, mSourceResY);
//    LOG(LOG_VERBOSE, "Target: %d * %d", mTargetResX, mTargetResY);

    if (!mMediaSourceOpened)
    {
    	mMutexGrabberActive.unlock();
    	return;
    }

    if (mWidget == NULL)
    {
    	LOG(LOG_ERROR, "Capture widget is invalid");
    	mMutexGrabberActive.unlock();
    	return;
    }

    QTime tCurrentTime = QTime::currentTime();
    int tTimeDiff = mLastTimeGrabbed.msecsTo(tCurrentTime);

    //### skip capturing when we are too slow
    if (tTimeDiff < 1000 / (mInputFrameRate + 0.5 /* some tolerance! */))
    {
        #ifdef MSD_DEBUG_PACKETS
            LOG(LOG_VERBOSE, "Screen capturing skipped because system is too fast");
        #endif
		mMutexGrabberActive.unlock();
    	return;
    }

    if (mLastTimeGrabbed == QTime(0, 0, 0, 0))
    {
        mLastTimeGrabbed = tCurrentTime;
        mMutexGrabberActive.unlock();
        return;
    }else
        mLastTimeGrabbed = tCurrentTime;

    //### skip capturing when we are too slow
    if (tTimeDiff > 1000 / MIN_GRABBING_FPS)
    {
    	LOG(LOG_WARN, "Screen capturing skipped because system is too busy");
    	mMutexGrabberActive.unlock();
    	return;
    }

    //####################################################################
    //### AUTO DESKTOP
    //####################################################################
    QDesktopWidget *tDesktop = QApplication::desktop();
    if (mAutoDesktop)
    {
        tCaptureResX = tDesktop->availableGeometry(tDesktop->primaryScreen()).width();
        tCaptureResY = tDesktop->availableGeometry(tDesktop->primaryScreen()).height();
    }
    if (mAutoScreen)
    {
		#ifdef APPLE
			tCaptureResX = CGDisplayPixelsWide(CGMainDisplayID());
			tCaptureResY = CGDisplayPixelsHigh(CGMainDisplayID());
		#else
			tCaptureResX = tDesktop->screenGeometry(tDesktop->primaryScreen()).width();
			tCaptureResY = tDesktop->screenGeometry(tDesktop->primaryScreen()).height();
		#endif
	}

    //####################################################################
    //### GRABBING
    //####################################################################
    QPixmap tSourcePixmap;
    // screen capturing
	#if !defined(APPLE) || defined(HOMER_QT5)
    	tSourcePixmap = QPixmap::grabWindow(mWidget->winId(), mGrabOffsetX, mGrabOffsetY, tCaptureResX, tCaptureResY);
	#else
		CGImageRef tOSXWindowImage = CGWindowListCreateImage(CGRectInfinite, kCGWindowListOptionOnScreenOnly, mWidget->winId(), kCGWindowImageDefault);
		tSourcePixmap = QPixmap::fromMacCGImageRef(tOSXWindowImage).copy(mGrabOffsetX, mGrabOffsetY, tCaptureResX, tCaptureResY);
		CGImageRelease(tOSXWindowImage);
	#endif

	//####################################################################
	//### SCALING to source resolution
	//####################################################################
	if ((tSourcePixmap.width() != mSourceResX) || (tSourcePixmap.height() != mSourceResY))
	{// we have to adapt the assumed source resolution
		//LOG(LOG_VERBOSE, "Have to rescale from %d*%d to %d*%d", tSourcePixmap.width(), tSourcePixmap.height(), mSourceResX, mSourceResY);
		tSourcePixmap = tSourcePixmap.scaled(mSourceResX, mSourceResY);
	}

	//####################################################################
	//### MOUSE VISUALIZATION
	//####################################################################
	if (mMouseVisualization)
	{
		QPoint tMousePos = QCursor::pos();
		if ((tMousePos.x() < tSourcePixmap.width()) && (tMousePos.y() < tSourcePixmap.height()))
		{// mouse is in visible area
			int tMousePosInSourcePixmapX = mSourceResX * tMousePos.x() / tCaptureResX;
			int tMousePosInSourcePixmapY = mSourceResY * tMousePos.y() / tCaptureResY;

			//LOG(LOG_VERBOSE, "Mouse position: %d*%d", tMousePosInSourcePixmapX, tMousePosInSourcePixmapY);
			QPainter *tPainter = new QPainter(&tSourcePixmap);
			//TODO: add support for click visualization
			tPainter->drawPixmap(tMousePosInSourcePixmapX, tMousePosInSourcePixmapY, QPixmap(":/images/MouseBlack.png").scaled(16, 32));
			delete tPainter;
		}
	}

	if(!tSourcePixmap.isNull())
    {
		// record screenshot via ffmpeg
		if (mRecording)
		{
			if ((tRGBFrame = AllocFrame()) == NULL)
			{
				LOG(LOG_ERROR, "Unable to allocate memory for RGB frame");
			}else
			{
				QImage tSourceImage = QImage((unsigned char*)mOriginalScreenshot, mSourceResX, mSourceResY, QImage::Format_RGB32);
				QPainter *tSourcePainter = new QPainter(&tSourceImage);
				tSourcePainter->drawPixmap(0, 0, tSourcePixmap);
				delete tSourcePainter;

				// Assign appropriate parts of buffer to image planes in tRGBFrame
				FillFrame(tRGBFrame, mOriginalScreenshot, PIX_FMT_RGB32, mSourceResX, mSourceResY);

				// set frame number in corresponding entries within AVFrame structure
				tRGBFrame->pts = mRecorderChunkNumber;
				tRGBFrame->coded_picture_number = mRecorderChunkNumber;
				tRGBFrame->display_picture_number = mRecorderChunkNumber;
                mRecorderChunkNumber++;

				// emulate set FPS
				tRGBFrame->pts = GetPtsFromFpsEmulator();

				// re-encode the frame and write it to file
				RecordFrame(tRGBFrame);
			}
		}

		// lock screenshot buffer
		mMutexScreenshot.lock();
		if (mOutputScreenshot == NULL)
		{
		    LOG(LOG_ERROR, "Invalid screenshot buffer: %p  %d*%d", mOutputScreenshot, mTargetResX, mTargetResY);

		    mMutexScreenshot.unlock();
		    mMutexGrabberActive.unlock();
		    return;
		}
		int tTargetResX = mTargetResX;
		if (tTargetResX > MAX_WIDTH)
		    tTargetResX = MAX_WIDTH;
        int tTargetResY = mTargetResY;
        if (tTargetResY > MAX_HEIGHT)
            tTargetResY = MAX_HEIGHT;
		QImage tTargetImage = QImage((unsigned char*)mOutputScreenshot, tTargetResX, tTargetResY, QImage::Format_RGB32);
		QPainter *tTargetPainter = new QPainter(&tTargetImage);
		QPixmap tScaledSourcePixmap = tSourcePixmap.scaled(tTargetResX, tTargetResY);
		tTargetPainter->drawPixmap(0, 0, tScaledSourcePixmap);
		delete tTargetPainter;

	    RelayChunkToMediaFilters((char*)mOutputScreenshot, tTargetResX * tTargetResY * MSD_BYTES_PER_PIXEL, 1);

		mScreenshotUpdated = true;
		// notify consumer about new screenshot
		mWaitConditionScreenshotUpdated.wakeAll();
		// unlock screenshot buffer again
		mMutexScreenshot.unlock();
    }else
    	LOG(LOG_ERROR, "Source pixmap is invalid");

    mMutexGrabberActive.unlock();
}

int MediaSourceDesktop::GrabChunk(void* pChunkBuffer, int& pChunkSize, bool pDropChunk)
{
    // lock grabbing
    mGrabMutex.lock();

    if (pChunkBuffer == NULL)
    {
        // unlock grabbing
        mGrabMutex.unlock();

        LOG(LOG_ERROR, "Tried to grab while chunk buffer doesn't exist");
        return -1;
    }

    if (!mMediaSourceOpened)
    {
        // unlock grabbing
        mGrabMutex.unlock();

        LOG(LOG_ERROR, "Tried to grab while video source is closed");
        return -1;
    }

    if (mGrabbingStopped)
    {
        // unlock grabbing
        mGrabMutex.unlock();

        LOG(LOG_ERROR, "Tried to grab while video source is paused");
        return -1;
    }

    int tTargetResX = mTargetResX;
    if (tTargetResX > MAX_WIDTH)
        tTargetResX = MAX_WIDTH;
    int tTargetResY = mTargetResY;
    if (tTargetResY > MAX_HEIGHT)
        tTargetResY = MAX_HEIGHT;

    if ((pChunkSize != 0 /* the application doesn't give us the chunk size */) && (pChunkSize < tTargetResX * tTargetResY * MSD_BYTES_PER_PIXEL))
    {
        // unlock grabbing
        mGrabMutex.unlock();

        LOG(LOG_ERROR, "Tried to grab while chunk buffer is too small (given: %d needed: %d)", pChunkSize, tTargetResX * tTargetResY * MSD_BYTES_PER_PIXEL);
        return -1;
    }

    // additional Qt based lock for QWaitCondition
    mMutexScreenshot.lock();

    // waiting for new data
    mWaitConditionScreenshotUpdated.wait(&mMutexScreenshot);

    // was wait interrupted because of a call to StopGrabbing ?
	if (mGrabbingStopped)
	{
	    // unlock again and enable new screenshots
	    mMutexScreenshot.unlock();

	    // unlock grabbing
		mGrabMutex.unlock();
		return -1;
	}

    memcpy(pChunkBuffer, mOutputScreenshot, tTargetResX * tTargetResY * MSD_BYTES_PER_PIXEL);
    mScreenshotUpdated = false;

    // unlock again and enable new screenshots
    mMutexScreenshot.unlock();

    // return size of decoded frame
    pChunkSize = tTargetResX * tTargetResY * MSD_BYTES_PER_PIXEL;

    // unlock grabbing
    mGrabMutex.unlock();

    AnnouncePacket(pChunkSize);

    return ++mFrameNumber;
}

GrabResolutions MediaSourceDesktop::GetSupportedVideoGrabResolutions()
{
    VideoFormatDescriptor tFormat;

    mSupportedVideoFormats.clear();

    tFormat.Name="CIF";        //      352 � 288
    tFormat.ResX = 352;
    tFormat.ResY = 288;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="VGA";       //      640 * 480
    tFormat.ResX = 640;
    tFormat.ResY = 480;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="DVD";        //      720 � 576
    tFormat.ResX = 720;
    tFormat.ResY = 576;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="CIF9";       //     1056 � 864
    tFormat.ResX = 1056;
    tFormat.ResY = 864;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="SXGA";       //     1280 � 1024
    tFormat.ResX = 1280;
    tFormat.ResY = 1024;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="WXGA+";      //     1440 � 900
    tFormat.ResX = 1440;
    tFormat.ResY = 900;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="SXGA+";       //     1440 � 1050
    tFormat.ResX = 1440;
    tFormat.ResY = 1050;
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="WUXGA";       //     1920 * 1200
    tFormat.ResX = 1920;
    tFormat.ResY = 1200;
    mSupportedVideoFormats.push_back(tFormat);
    tFormat.Name="UHD";       //     3840 * 2160
    tFormat.ResX = 3840;
    tFormat.ResY = 2160;
    mSupportedVideoFormats.push_back(tFormat);

    QDesktopWidget *tDesktop = QApplication::desktop();
    tFormat.Name = MEDIA_SOURCE_DESKTOP_RESOLUTION_ID;
    tFormat.ResX = tDesktop->availableGeometry(tDesktop->primaryScreen()).width();
    tFormat.ResY = tDesktop->availableGeometry(tDesktop->primaryScreen()).height();
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name = MEDIA_SOURCE_SCREEN_RESOLUTION_ID;
	#if defined(APPLE)
    	tFormat.ResX = CGDisplayPixelsWide(CGMainDisplayID());
    	tFormat.ResY = CGDisplayPixelsHigh(CGMainDisplayID());
	#else
		tFormat.ResX = tDesktop->screenGeometry(tDesktop->primaryScreen()).width();
		tFormat.ResY = tDesktop->screenGeometry(tDesktop->primaryScreen()).height();
	#endif
    mSupportedVideoFormats.push_back(tFormat);

    tFormat.Name="Original";
    tFormat.ResX = mSourceResX;
    tFormat.ResY = mSourceResY;
    mSupportedVideoFormats.push_back(tFormat);

    return mSupportedVideoFormats;
}

///////////////////////////////////////////////////////////////////////////////

int MediaSourceDesktop::SelectSegment(QWidget *pParent)
{
	int tResult = QDialog::Rejected;

	if (mRecording)
	{
		DoShowInfo(GetObjectNameStr(this).c_str(), __LINE__, pParent, "Recording active", "Desktop capture settings cannot be changed if recording is active");
		return tResult;
	}
    int tOldGrabOffsetX = mGrabOffsetX;
    int tOldGrabOffsetY = mGrabOffsetY;
    int tOldSourceResX = mSourceResX;
    int tOldSourceResY = mSourceResY;

    SegmentSelectionDialog *tSelectDialog = new SegmentSelectionDialog(pParent, this);
    tSelectDialog->Init();
    tResult = tSelectDialog->exec();
    if (tResult == QDialog::Rejected)
    {
        LOG(LOG_VERBOSE, "Resetting to original segment capture configuration");
        mGrabOffsetX = tOldGrabOffsetX;
        mGrabOffsetY = tOldGrabOffsetY;
        mSourceResX = tOldSourceResX;
        mSourceResY = tOldSourceResY;
    }else

    delete tSelectDialog;

    return tResult;
}

///////////////////////////////////////////////////////////////////////////////

}} //namespace
