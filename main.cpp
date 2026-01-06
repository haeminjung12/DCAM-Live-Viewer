#include <QtWidgets>
#include <QtCore>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QScrollArea>
#include <QWheelEvent>
#include <QScrollBar>
#include <algorithm>
#include <atomic>
#include <exception>
#include <csignal>
#include <thread>
#include <vector>
#include <cmath>
#include "frame_types.h"
#include "dcam_controller.h"
#include "frame_grabber.h"

namespace {
QMutex gLogMutex;
QFile gLogFile;
QString gLogPath;

class ZoomImageView : public QScrollArea {
public:
    ZoomImageView(QWidget* parent=nullptr)
        : QScrollArea(parent), label(new QLabel), scale(1.0) {
        label->setBackgroundRole(QPalette::Base);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        label->setScaledContents(true);
        setWidget(label);
        setAlignment(Qt::AlignCenter);
        setWidgetResizable(false);
    }

    void setImage(const QImage& img) {
        if (img.isNull()) return;
        lastImage = img;
        updatePixmap();
    }

    void resetScale() {
        scale = 1.0;
        updatePixmap();
    }

protected:
    void wheelEvent(QWheelEvent* ev) override {
        if (lastImage.isNull()) {
            QScrollArea::wheelEvent(ev);
            return;
        }
        double numDegrees = ev->angleDelta().y();
        double factor = std::pow(1.0015, numDegrees);
        double newScale = std::clamp(scale * factor, 0.02, 20.0);
        if (qFuzzyCompare(newScale, scale)) {
            ev->accept();
            return;
        }

        QPointF vpPos = ev->position();
        QPointF contentPos = (vpPos + QPointF(horizontalScrollBar()->value(),
                                              verticalScrollBar()->value())) / scale;

        scale = newScale;
        updatePixmap();

        horizontalScrollBar()->setValue(int(contentPos.x() * scale - vpPos.x()));
        verticalScrollBar()->setValue(int(contentPos.y() * scale - vpPos.y()));
        ev->accept();
    }

private:
    void updatePixmap() {
        if (lastImage.isNull()) return;
        const int w = std::max(1, static_cast<int>(std::lround(lastImage.width() * scale)));
        const int h = std::max(1, static_cast<int>(std::lround(lastImage.height() * scale)));
        QSize scaledSize(w, h);
        QPixmap pm = QPixmap::fromImage(lastImage).scaled(scaledSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        label->setPixmap(pm);
        label->resize(pm.size());
    }

    QLabel* label;
    QImage lastImage;
    double scale;
};

void pruneLogs() {
    QFileInfo fi(gLogFile);
    QString baseDir = fi.dir().absolutePath();
    QString baseName = "session_log";
    QStringList files = QDir(baseDir).entryList(QStringList() << (baseName + "*.txt"), QDir::Files, QDir::Time);
    for (int i = 50; i < files.size(); ++i) {
        QFile::remove(baseDir + "/" + files[i]);
    }
}

void logMessage(const QString& msg) {
    QMutexLocker locker(&gLogMutex);
    if (!gLogFile.isOpen()) {
        gLogFile.open(QIODevice::Append | QIODevice::Text);
    }
    if (!gLogFile.isOpen()) return;
    QTextStream ts(&gLogFile);
    const QString line = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") + " " + msg;
    ts << line << "\n";
    ts.flush();
    gLogFile.close();
    // mirror to console for visibility when launched from a terminal
    QTextStream out(stdout);
    out << line << "\n";
    out.flush();
    pruneLogs();
}

void qtLogHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    Q_UNUSED(ctx);
    QString level;
    switch (type) {
        case QtDebugMsg: level="DEBUG"; break;
        case QtInfoMsg: level="INFO"; break;
        case QtWarningMsg: level="WARN"; break;
        case QtCriticalMsg: level="CRIT"; break;
        case QtFatalMsg: level="FATAL"; break;
    }
    logMessage(QString("[%1] %2").arg(level, msg));
}

void termHandler() {
    logMessage("std::terminate called");
    std::_Exit(1);
}
} // namespace


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    gLogPath = QCoreApplication::applicationDirPath() + "/session_log.txt";
    gLogFile.setFileName(gLogPath);
    pruneLogs();
    qInstallMessageHandler(qtLogHandler);
    std::set_terminate(termHandler);

    QWidget window;
    window.setWindowTitle("Hamamatsu Live View");
    window.resize(1280, 800);
    window.setMinimumSize(900, 600);

    // Live view area with zoomable/pannable view
    auto imageView = new ZoomImageView;
    imageView->setMinimumSize(640, 480);
    imageView->setStyleSheet("background:#000;");
    imageView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    // Info panel
    auto statusLabel = new QLabel("Status: Not initialized");
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    statusLabel->setTextFormat(Qt::PlainText);
    auto statsLabel = new QLabel("Resolution: --\nFPS: --\nFrame: --");
    statsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    statsLabel->setTextFormat(Qt::PlainText);
    statsLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    statsLabel->setMinimumWidth(220);

    // Buttons
    auto initBtn = new QPushButton("Init / Open");
    auto startBtn = new QPushButton("Start");
    auto stopBtn = new QPushButton("Stop");
    auto reconnectBtn = new QPushButton("Reconnect");
    auto applyBtn = new QPushButton("Apply Settings");
    auto tabWidget = new QTabWidget;

    // Settings controls
    auto presetCombo = new QComboBox;
    presetCombo->addItem("2304 x 2304", QVariant::fromValue(QSize(2304,2304)));
    presetCombo->addItem("2304 x 1152", QVariant::fromValue(QSize(2304,1152)));
    presetCombo->addItem("2304 x 576", QVariant::fromValue(QSize(2304,576)));
    presetCombo->addItem("2304 x 288", QVariant::fromValue(QSize(2304,288)));
    presetCombo->addItem("2304 x 144", QVariant::fromValue(QSize(2304,144)));
    presetCombo->addItem("2304 x 72", QVariant::fromValue(QSize(2304,72)));
    presetCombo->addItem("2304 x 36", QVariant::fromValue(QSize(2304,36)));
    presetCombo->addItem("2304 x 16", QVariant::fromValue(QSize(2304,16)));
    presetCombo->addItem("2304 x 8", QVariant::fromValue(QSize(2304,8)));
    presetCombo->addItem("2304 x 4", QVariant::fromValue(QSize(2304,4)));
    presetCombo->addItem("1152 x 1152", QVariant::fromValue(QSize(1152,1152)));
    presetCombo->addItem("1152 x 576", QVariant::fromValue(QSize(1152,576)));
    presetCombo->addItem("1152 x 288", QVariant::fromValue(QSize(1152,288)));
    presetCombo->addItem("1152 x 144", QVariant::fromValue(QSize(1152,144)));
    presetCombo->addItem("576 x 576", QVariant::fromValue(QSize(576,576)));
    presetCombo->addItem("576 x 288", QVariant::fromValue(QSize(576,288)));
    presetCombo->addItem("576 x 144", QVariant::fromValue(QSize(576,144)));
    presetCombo->addItem("288 x 288", QVariant::fromValue(QSize(288,288)));
    presetCombo->addItem("288 x 144", QVariant::fromValue(QSize(288,144)));
    presetCombo->addItem("144 x 144", QVariant::fromValue(QSize(144,144)));
    presetCombo->addItem("Custom", QVariant::fromValue(QSize(-1,-1)));

    auto customWidthSpin = new QSpinBox;
    customWidthSpin->setRange(1, 4096);
    customWidthSpin->setValue(2304);
    auto customHeightSpin = new QSpinBox;
    customHeightSpin->setRange(1, 4096);
    customHeightSpin->setValue(2304);
    presetCombo->addItem("512 x 128", QVariant::fromValue(QSize(512,128)));
    presetCombo->addItem("512 x 64", QVariant::fromValue(QSize(512,64)));
    presetCombo->addItem("256 x 64", QVariant::fromValue(QSize(256,64)));
    presetCombo->addItem("256 x 32", QVariant::fromValue(QSize(256,32)));

    auto binCombo = new QComboBox;
    binCombo->addItems({"1","2","4"});
    binCombo->setCurrentIndex(0);

    auto binIndCheck = new QCheckBox("Independent binning");
    auto binHSpin = new QSpinBox;
    auto binVSpin = new QSpinBox;
    binHSpin->setMinimum(1); binHSpin->setMaximum(8); binHSpin->setValue(1);
    binVSpin->setMinimum(1); binVSpin->setMaximum(8); binVSpin->setValue(1);

    auto exposureSpin = new QDoubleSpinBox;
    exposureSpin->setSuffix(" ms");
    exposureSpin->setDecimals(3);
    exposureSpin->setSingleStep(0.1);
    exposureSpin->setMinimum(0.01);
    exposureSpin->setMaximum(10000.0);
    exposureSpin->setValue(10.0);

    auto readoutCombo = new QComboBox;
    readoutCombo->addItem("Fastest", DCAMPROP_READOUTSPEED__FASTEST);
    readoutCombo->addItem("Slowest", DCAMPROP_READOUTSPEED__SLOWEST);
    readoutCombo->setCurrentIndex(0);

    auto logCheck = new QCheckBox("Enable logging (session_log.txt)");
    logCheck->setChecked(true);

    // Save controls
    QString defaultSaveDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (defaultSaveDir.isEmpty())
        defaultSaveDir = QCoreApplication::applicationDirPath();
    auto savePathEdit = new QLineEdit(defaultSaveDir);
    auto saveBrowseBtn = new QPushButton("...");
    auto saveStartBtn = new QPushButton("Start Save");
    auto saveStopBtn = new QPushButton("Stop Save");
    saveStopBtn->setEnabled(false);
    auto saveInfoLabel = new QLabel("Elapsed: 0.0 s\nFrames: 0");
    QDialog* savingDialog = nullptr;
    QLabel* savingDialogLabel = nullptr;
    QProgressBar* savingProgress = nullptr;

    auto displayEverySpin = new QSpinBox;
    displayEverySpin->setMinimum(1);
    displayEverySpin->setMaximum(1000);
    displayEverySpin->setValue(1);

    auto controlLayout = new QVBoxLayout;
    controlLayout->addWidget(statusLabel);
    controlLayout->addWidget(statsLabel);

    auto tabFormats = new QWidget;
    auto grid = new QGridLayout;
    grid->addWidget(new QLabel("Preset"),0,0);
    grid->addWidget(presetCombo,0,1);
    grid->addWidget(new QLabel("Custom W/H"),1,0);
    auto customLayout = new QHBoxLayout;
    customLayout->addWidget(customWidthSpin);
    customLayout->addWidget(customHeightSpin);
    grid->addLayout(customLayout,1,1);
    grid->addWidget(new QLabel("Binning"),2,0);
    grid->addWidget(binCombo,2,1);
    grid->addWidget(binIndCheck,3,0,1,2);
    grid->addWidget(new QLabel("Bin H/V"),4,0);
    auto binHVLayout = new QHBoxLayout;
    binHVLayout->addWidget(binHSpin);
    binHVLayout->addWidget(binVSpin);
    grid->addLayout(binHVLayout,4,1);
    grid->addWidget(new QLabel("Exposure (ms)"),5,0);
    grid->addWidget(exposureSpin,5,1);
    grid->addWidget(new QLabel("Readout speed"),6,0);
    grid->addWidget(readoutCombo,6,1);
    grid->addWidget(new QLabel("Display every Nth frame"),7,0);
    grid->addWidget(displayEverySpin,7,1);
    grid->addWidget(logCheck,8,0,1,2);
    tabFormats->setLayout(grid);

    tabWidget->addTab(tabFormats, "Formats / Speed");

    auto saveLayout = new QGridLayout;
    saveLayout->addWidget(new QLabel("Save path"),0,0);
    saveLayout->addWidget(savePathEdit,0,1);
    saveLayout->addWidget(saveBrowseBtn,0,2);
    saveLayout->addWidget(saveInfoLabel,1,0,1,3);
    saveLayout->addWidget(saveStartBtn,2,1);
    saveLayout->addWidget(saveStopBtn,2,2);
    auto saveWidget = new QWidget;
    saveWidget->setLayout(saveLayout);
    tabWidget->addTab(saveWidget, "Save");

    auto btnRow = new QHBoxLayout;
    btnRow->addWidget(initBtn);
    btnRow->addWidget(startBtn);
    btnRow->addWidget(stopBtn);
    btnRow->addWidget(reconnectBtn);

    controlLayout->addWidget(tabWidget);
    controlLayout->addLayout(btnRow);
    controlLayout->addWidget(applyBtn);
    controlLayout->addStretch(1);

    auto rightWidget = new QWidget;
    rightWidget->setLayout(controlLayout);
    rightWidget->setMinimumWidth(320);

    auto mainLayout = new QHBoxLayout;
    mainLayout->addWidget(imageView, 3);
    mainLayout->addWidget(rightWidget, 1);
    window.setLayout(mainLayout);

    // Logging helper
    auto logLine = [&](const QString& msg) {
        if (!logCheck->isChecked()) return;
        logMessage(msg);
    };

    DcamController controller(&window);
    FrameGrabber grabber(&controller);

    auto refreshExposureLimits = [&](){
        if (!controller.isOpened()) return;
        DCAMPROP_ATTR attr = {};
        attr.cbSize = sizeof(attr);
        attr.iProp = DCAM_IDPROP_EXPOSURETIME;
        if (!failed(dcamprop_getattr(controller.handle(), &attr))) {
            double min_ms = attr.valuemin * 1000.0;
            double max_ms = attr.valuemax * 1000.0;
            exposureSpin->setMinimum(min_ms);
            exposureSpin->setMaximum(max_ms);
        }
        double cur=0;
        if (!failed(dcamprop_getvalue(controller.handle(), DCAM_IDPROP_EXPOSURETIME, &cur))) {
            exposureSpin->setValue(cur * 1000.0);
        }
    };

    QObject::connect(presetCombo, qOverload<int>(&QComboBox::currentIndexChanged), [&](int){
        bool isCustom = presetCombo->currentData().toSize().width() < 0;
        customWidthSpin->setEnabled(isCustom);
        customHeightSpin->setEnabled(isCustom);
    });
    // Initialize state
    customWidthSpin->setEnabled(false);
    customHeightSpin->setEnabled(false);

    auto applySettings = [&](){
        QSize preset = presetCombo->currentData().toSize();
        bool isCustom = preset.width() < 0 || preset.height() < 0;
        int bin = binCombo->currentText().toInt();
        int bits = 8;
        int pixel = DCAM_PIXELTYPE_MONO8;
        double exp_ms = exposureSpin->value();
        double exp_s = exp_ms / 1000.0;
        int readout = readoutCombo->currentData().toInt();
        ApplySettings s;
        s.width = isCustom ? customWidthSpin->value() : preset.width();
        s.height = isCustom ? customHeightSpin->value() : preset.height();
        s.binning = bin;
        s.binningIndependent = binIndCheck->isChecked();
        s.binH = binHSpin->value();
        s.binV = binVSpin->value();
        s.bits = bits;
        s.pixelType = pixel;
        s.exposure_s = exp_s;
        s.readoutSpeed = readout;
        s.bundleEnabled = false;
        s.bundleCount = 0;
        logLine(QString("Apply: preset=%1x%2 bin=%3 binH=%4 binV=%5 bits=%6 pixType=%7 exp_ms=%8 readout=%9")
            .arg(s.width).arg(s.height).arg(s.binning).arg(s.binH).arg(s.binV)
            .arg(s.bits).arg(s.pixelType).arg(exp_ms,0,'f',3).arg(readout));
        QString err = controller.apply(s);
        auto logReadback = [&](){
            if (!controller.isOpened()) return;
            HDCAM h = controller.handle();
            double w=0,hgt=0,binrb=0,bitsrb=0,pt=0,fps=0,ro=0,exp_rb=0,binHrb=0,binVrb=0;
            dcamprop_getvalue(h, DCAM_IDPROP_IMAGE_WIDTH, &w);
            dcamprop_getvalue(h, DCAM_IDPROP_IMAGE_HEIGHT, &hgt);
            dcamprop_getvalue(h, DCAM_IDPROP_BINNING, &binrb);
            dcamprop_getvalue(h, DCAM_IDPROP_BITSPERCHANNEL, &bitsrb);
            dcamprop_getvalue(h, DCAM_IDPROP_IMAGE_PIXELTYPE, &pt);
            dcamprop_getvalue(h, DCAM_IDPROP_INTERNALFRAMERATE, &fps);
            dcamprop_getvalue(h, DCAM_IDPROP_READOUTSPEED, &ro);
            dcamprop_getvalue(h, DCAM_IDPROP_EXPOSURETIME, &exp_rb);
            dcamprop_getvalue(h, DCAM_IDPROP_BINNING_HORZ, &binHrb);
            dcamprop_getvalue(h, DCAM_IDPROP_BINNING_VERT, &binVrb);
            logLine(QString("Readback: w=%1 h=%2 bin=%3 binH=%4 binV=%5 bits=%6 pixType=%7 exp_ms=%8 camfps=%9 readout=%10")
                .arg(w,0,'f',0).arg(hgt,0,'f',0).arg(binrb,0,'f',1)
                .arg(binHrb,0,'f',1).arg(binVrb,0,'f',1)
                .arg(bitsrb,0,'f',0).arg(pt,0,'f',0)
                .arg(exp_rb*1000.0,0,'f',3).arg(fps,0,'f',1).arg(ro,0,'f',0));
        };
        if (!err.isEmpty()) {
            if (err.startsWith("WARN:")) {
                statusLabel->setText("Applied with warnings: " + err.mid(5));
                grabber.startGrabbing();
            } else {
                statusLabel->setText("Apply error: " + err);
            }
        } else {
            statusLabel->setText("Applied. Streaming");
            grabber.startGrabbing();
        }
        grabber.setDisplayEvery(displayEverySpin->value());
        logReadback();
    };

    auto doInit = [&]()->bool{
        QString err = controller.initAndOpen();
        if (!err.isEmpty()) {
            statusLabel->setText("Init error: " + err);
            QMessageBox::critical(&window, "Init failed", "Camera init failed:\n" + err);
            QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
            return false;
        } else {
            statusLabel->setText("Initialized.");
            refreshExposureLimits();
            // Force default exposure to 10 ms on camera and UI
            dcamprop_setvalue(controller.handle(), DCAM_IDPROP_EXPOSURETIME, 0.010);
            exposureSpin->setValue(10.0);
            logLine("Init success");
            return true;
        }
    };

    QObject::connect(initBtn, &QPushButton::clicked, doInit);

    QObject::connect(reconnectBtn, &QPushButton::clicked, [&](){
        QString err = controller.reconnect();
        if (!err.isEmpty()) statusLabel->setText("Reconnect error: " + err);
        else {
            statusLabel->setText("Reconnected.");
            refreshExposureLimits();
        }
    });

    QObject::connect(startBtn, &QPushButton::clicked, [&](){
        QString err = controller.start();
        if (!err.isEmpty()) statusLabel->setText("Start error: " + err);
        else {
            statusLabel->setText("Capture started.");
            grabber.startGrabbing();
        }
    });

    QObject::connect(stopBtn, &QPushButton::clicked, [&](){
        grabber.stopGrabbing();
        controller.stop();
        statusLabel->setText("Capture stopped.");
    });

    QObject::connect(applyBtn, &QPushButton::clicked, applySettings);

    // Save state
    auto saveBuffer = std::make_shared<std::vector<QImage>>();
    auto saveMutex = std::make_shared<QMutex>();
    std::atomic<bool> recording{false};
    std::atomic<bool> saving{false};
    QElapsedTimer recordTimer;
    std::atomic<int> recordedFrames{0};
    QTimer saveInfoTimer;
    saveInfoTimer.setInterval(200);

    QObject::connect(saveBrowseBtn, &QPushButton::clicked, [&](){
        QString dir = QFileDialog::getExistingDirectory(&window, "Select save directory", savePathEdit->text());
        if (!dir.isEmpty()) savePathEdit->setText(dir);
    });

    auto startSaving = [&](){
        if (saving.load()) {
            statusLabel->setText("Already saving to disk");
            return;
        }
        recording = true;
        {
            QMutexLocker lk(saveMutex.get());
            saveBuffer->clear();
        }
        recordedFrames = 0;
        recordTimer.restart();
        saveStartBtn->setEnabled(false);
        saveStopBtn->setEnabled(true);
        logLine("Recording started");
        statusLabel->setText("Recording...");
        saveInfoLabel->setText("Elapsed: 0.0 s\nFrames: 0");
        saveInfoTimer.start();
    };

    auto stopSaving = [&](){
        if (!recording.load()) return;
        recording = false;
        saveStartBtn->setEnabled(true);
        saveStopBtn->setEnabled(false);
        saveInfoTimer.stop();

        std::shared_ptr<std::vector<QImage>> frames = std::make_shared<std::vector<QImage>>();
        {
            QMutexLocker lk(saveMutex.get());
            frames->swap(*saveBuffer);
        }
        if (frames->empty()) {
            statusLabel->setText("No frames to save");
            return;
        }

        QString baseDir = savePathEdit->text();
        if (baseDir.isEmpty()) baseDir = QCoreApplication::applicationDirPath();
        QDir dir(baseDir);
        dir.mkpath(".");
        QString sub = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss");
        QString outDir = dir.filePath(sub);
        dir.mkpath(outDir);

        saving = true;
        statusLabel->setText("Saving to disk...");
        logLine(QString("Saving %1 frames to %2").arg(frames->size()).arg(outDir));
        if (!savingDialog) {
            savingDialog = new QDialog(&window);
            savingDialog->setWindowTitle("Saving...");
            savingDialog->setModal(true);
            auto layout = new QVBoxLayout(savingDialog);
            savingDialogLabel = new QLabel(savingDialog);
            savingProgress = new QProgressBar(savingDialog);
            savingProgress->setMinimum(0);
            layout->addWidget(savingDialogLabel);
            layout->addWidget(savingProgress);
            savingDialog->setLayout(layout);
        }
        int totalFrames = static_cast<int>(frames->size());
        savingDialogLabel->setText(QString("Saving %1 frames...").arg(totalFrames));
        savingProgress->setRange(0, totalFrames);
        savingProgress->setValue(0);
        savingDialog->show();

        std::thread([frames, outDir, logLine, statusLabel, savingDialog, savingProgress, totalFrames, &saving](){
            int width = std::max(6, static_cast<int>(std::ceil(std::log10(std::max<size_t>(1, frames->size())))));
            for (size_t i = 0; i < frames->size(); ++i) {
                const QImage& im = frames->at(i);
                QString fname = QString("%1.tiff").arg(static_cast<int>(i), width, 10, QChar('0'));
                QString path = outDir + "/" + fname;
                im.save(path, "TIFF");
                if (savingProgress && (i % 100 == 0 || i + 1 == frames->size())) {
                    int v = static_cast<int>(i + 1);
                    QMetaObject::invokeMethod(savingProgress, [savingProgress, v](){
                        savingProgress->setValue(v);
                    }, Qt::QueuedConnection);
                }
            }
            logLine(QString("Saved %1 frames to %2").arg(frames->size()).arg(outDir));
            QMetaObject::invokeMethod(statusLabel, [statusLabel](){
                statusLabel->setText("Save complete");
            }, Qt::QueuedConnection);
            if (savingDialog) {
                QMetaObject::invokeMethod(savingDialog, [savingDialog](){
                    savingDialog->hide();
                }, Qt::QueuedConnection);
            }
            saving = false;
        }).detach();
    };

    QObject::connect(saveStartBtn, &QPushButton::clicked, startSaving);
    QObject::connect(saveStopBtn, &QPushButton::clicked, stopSaving);

    QObject::connect(&saveInfoTimer, &QTimer::timeout, [&](){
        if (!recording.load()) return;
        double elapsed = recordTimer.isValid() ? recordTimer.elapsed() / 1000.0 : 0.0;
        saveInfoLabel->setText(QString("Elapsed: %1 s\nFrames: %2")
            .arg(elapsed,0,'f',1).arg(recordedFrames.load()));
    });

    grabber.setRecordHook([saveMutex, saveBuffer, &recording, &recordedFrames](const QImage& img){
        if (!recording.load()) return;
        QMutexLocker lk(saveMutex.get());
        saveBuffer->push_back(img.copy());
        recordedFrames++;
    });

    QObject::connect(&grabber, &FrameGrabber::frameReady, [&](const QImage& img, FrameMeta meta, double fps){
        if (!img.isNull()) {
        imageView->setImage(img);
        }
        statsLabel->setText(QString("Resolution: %1 x %2\nBinning: %3\nBits: %4\nFPS: %5 (Cam: %6)\nFrame: %7\nDelivered: %8 Dropped: %9\nReadout: %10")
            .arg(meta.width).arg(meta.height).arg(meta.binning,0,'f',1).arg(meta.bits)
            .arg(fps,0,'f',1).arg(meta.internalFps,0,'f',1).arg(meta.frameIndex).arg(meta.delivered).arg(meta.dropped).arg(meta.readoutSpeed,0,'f',0));
        if (logCheck->isChecked() && (meta.frameIndex % 100 == 0)) {
            logLine(QString("Frame=%1 FPS=%2 camfps=%3 delivered=%4 dropped=%5")
                .arg(meta.frameIndex).arg(fps,0,'f',1).arg(meta.internalFps,0,'f',1).arg(meta.delivered).arg(meta.dropped));
        }
    });

    QObject::connect(&app, &QApplication::aboutToQuit, [&](){
        grabber.stopGrabbing();
        controller.stop();
        controller.cleanup();
        logMessage("Exiting application");
    });

    window.show();
    doInit();
    int rc = 0;
    try {
        rc = app.exec();
    } catch (const std::exception& e) {
        logMessage(QString("Fatal exception: %1").arg(e.what()));
        rc = 1;
    } catch (...) {
        logMessage("Fatal unknown exception");
        rc = 1;
    }
    logMessage(QString("Event loop exited with code %1").arg(rc));
    return rc;
}
