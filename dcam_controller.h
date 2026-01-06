#pragma once
#include <QtCore>
#include <atomic>
#include "../Hamamatsu_DCAMSDK4_v25056964/dcamsdk4/inc/dcamapi4.h"
#include "../Hamamatsu_DCAMSDK4_v25056964/dcamsdk4/inc/dcamprop.h"
#include "frame_types.h"

class DcamController : public QObject {
    Q_OBJECT
public:
    explicit DcamController(QObject* parent=nullptr);
    ~DcamController() override;

    QString initAndOpen();
    QString reconnect();
    QString start();
    void stop();
    void cleanup();

    QString apply(const ApplySettings& s);
    QString readProps(QString& out);

    bool isOpened() const { return opened; }
    HDCAM handle() const { return hdcam; }
    HDCAMWAIT waitHandle() const { return hwait; }

    bool lockLatestFrame(QImage& outImage, FrameMeta& meta);

private:
    QString errText(const QString& label, DCAMERR err) const;
    HDCAM hdcam;
    HDCAMWAIT hwait;
    bool opened;
    qint64 frameCounter;
};
