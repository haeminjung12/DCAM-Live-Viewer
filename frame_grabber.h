#pragma once
#include <QtCore>
#include <QtGui>
#include <atomic>
#include "frame_types.h"
#include <functional>

class DcamController;

class FrameGrabber : public QThread {
    Q_OBJECT
public:
    FrameGrabber(DcamController* ctrl, QObject* parent=nullptr);

    void setDisplayEvery(int n);
    void startGrabbing();
    void stopGrabbing();
    void setRecordHook(std::function<void(const QImage&)> hook) { recordHook = std::move(hook); }

signals:
    void frameReady(const QImage& img, FrameMeta meta, double fps);

protected:
    void run() override;

private:
    DcamController* controller;
    std::atomic<bool> running;
    int displayEvery;
    std::function<void(const QImage&)> recordHook;
};
