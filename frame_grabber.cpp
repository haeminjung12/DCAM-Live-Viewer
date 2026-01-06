#include "frame_grabber.h"
#include "dcam_controller.h"

FrameGrabber::FrameGrabber(DcamController* ctrl, QObject* parent)
    : QThread(parent), controller(ctrl), running(false), displayEvery(1) {}

void FrameGrabber::setDisplayEvery(int n) {
    displayEvery = std::max(1, n);
}

void FrameGrabber::startGrabbing() {
    running = true;
    if (!isRunning()) start();
}

void FrameGrabber::stopGrabbing() {
    running = false;
    wait(5000);
}

void FrameGrabber::run() {
    try {
        QElapsedTimer secondTimer;
        secondTimer.start();
        int framesThisSecond = 0;
        double currentFps = 0.0;
        int displayCounter = 0;
        QElapsedTimer emitTimer;
        emitTimer.start();
        const qint64 minEmitIntervalMs = 15; // cap UI updates ~66 FPS
        qint64 lastEmitMs = 0;

        while (running) {
            if (!controller->isOpened()) {
                QThread::msleep(50);
                continue;
            }

            DCAMWAIT_START wait = {};
            wait.size = sizeof(wait);
            wait.eventmask = DCAMWAIT_CAPEVENT_FRAMEREADY;
            wait.timeout = 1000; // ms
            DCAMERR err = dcamwait_start(controller->waitHandle(), &wait);
            if (failed(err)) {
                QThread::msleep(5);
                continue;
            }

            QImage img;
            FrameMeta meta;
            if (controller->lockLatestFrame(img, meta)) {
                if (recordHook) {
                    recordHook(img);
                }
                framesThisSecond++;
                if (secondTimer.elapsed() >= 1000) {
                    currentFps = framesThisSecond * 1000.0 / secondTimer.elapsed();
                    framesThisSecond = 0;
                    secondTimer.restart();
                }
                displayCounter++;
                if (displayCounter >= displayEvery &&
                    emitTimer.elapsed() - lastEmitMs >= minEmitIntervalMs) {
                    displayCounter = 0;
                    lastEmitMs = emitTimer.elapsed();
                    emit frameReady(img, meta, currentFps);
                }
            }
        }
    } catch (const std::exception& e) {
        qCritical() << "FrameGrabber exception:" << e.what();
        emit frameReady(QImage(), FrameMeta(), 0.0);
    } catch (...) {
        qCritical() << "FrameGrabber unknown exception";
        emit frameReady(QImage(), FrameMeta(), 0.0);
    }
}
