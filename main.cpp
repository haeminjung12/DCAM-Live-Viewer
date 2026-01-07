#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QtWidgets>
#include <QtCore>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>
#include <QScrollArea>
#include <QWheelEvent>
#include <QScrollBar>
#include <QStandardPaths>
#include <windows.h>
#include <algorithm>
#include <functional>
#include <atomic>
#include <exception>
#include <csignal>
#include <thread>
#include <vector>
#include <cmath>
#include <string>
#include <iostream>
#include "log_teebuf.h"
#include "frame_types.h"
#include "dcam_controller.h"
#include "frame_grabber.h"

namespace {
QMutex gLogMutex;
QFile gLogFile;
QString gLogPath;
void logMessage(const QString& msg);
void installLogTees();

class ZoomImageView : public QScrollArea {
public:
    ZoomImageView(QWidget* parent=nullptr)
        : QScrollArea(parent), label(new QLabel), scale(1.0), hasImage(false), zoomSteps(0), effectiveScale(1.0) {
        label->setBackgroundRole(QPalette::Base);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        label->setScaledContents(true); // paint-time scaling instead of allocating huge pixmaps
        setWidget(label);
        setAlignment(Qt::AlignCenter);
        setWidgetResizable(false);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setMouseTracking(true);
    }

    void setZoomChanged(const std::function<void(double)>& cb) { onZoomChanged = cb; }

    void setImage(const QImage& img) {
        if (img.isNull()) return;
        if (!hasImage) {
            scale = 1.0;
            effectiveScale = 1.0;
            hasImage = true;
            zoomSteps = 0;
            if (onZoomChanged) onZoomChanged(effectiveScale);
            if (horizontalScrollBar()) horizontalScrollBar()->setValue(0);
            if (verticalScrollBar()) verticalScrollBar()->setValue(0);
        }
        // Make a deep copy so the buffer is stable while frames keep streaming.
        lastImage = img.copy();
        basePixmap = QPixmap::fromImage(lastImage);
        updatePixmap();
    }

    void resetScale() {
        scale = 1.0;
        effectiveScale = 1.0;
        zoomSteps = 0;
        if (onZoomChanged) onZoomChanged(effectiveScale);
        hasImage = !lastImage.isNull();
        updatePixmap();
    }

protected:
    void wheelEvent(QWheelEvent* ev) override {
        try {
            if (lastImage.isNull()) {
                QScrollArea::wheelEvent(ev);
                return;
            }
            // Normalize to wheel ticks (120 per detent)
            double ticks = ev->angleDelta().y() / 120.0;
            double oldScale = scale;
            int newSteps = std::clamp(zoomSteps + static_cast<int>(std::round(ticks)), -50, 50);
            double desiredScale = std::pow(1.25, newSteps); // ~1.25x per tick
            double maxScale = computeMaxScale();
            double newScale = std::clamp(desiredScale, 0.05, maxScale); // avoid zero/negative and clamp max
            if (qFuzzyCompare(newScale, scale)) {
                ev->accept();
                return;
            }
            // Keep steps consistent with the clamped scale to avoid runaway values.
            zoomSteps = static_cast<int>(std::lround(std::log(newScale) / std::log(1.25)));

            QPointF vpPos = ev->position();
            QPointF contentPos = (vpPos + QPointF(horizontalScrollBar()->value(),
                                                  verticalScrollBar()->value())) / oldScale;

            scale = newScale;
            logMessage(QString("Zoom wheel ticks=%1 steps=%2 scale=%3").arg(ticks,0,'f',2).arg(zoomSteps).arg(scale,0,'f',2));
            logMessage(QString("Zoom before update: vp=(%1,%2) content=(%3,%4)")
                .arg(vpPos.x(),0,'f',1).arg(vpPos.y(),0,'f',1)
                .arg(contentPos.x(),0,'f',1).arg(contentPos.y(),0,'f',1));

            updatePixmap();

            horizontalScrollBar()->setValue(int(contentPos.x() * scale - vpPos.x()));
            verticalScrollBar()->setValue(int(contentPos.y() * scale - vpPos.y()));
            logMessage(QString("Zoom after update: hVal=%1 vVal=%2").arg(horizontalScrollBar()->value()).arg(verticalScrollBar()->value()));
            ev->accept();
        } catch (const std::exception& e) {
            logMessage(QString("Zoom wheel exception: %1").arg(e.what()));
        } catch (...) {
            logMessage("Zoom wheel exception: unknown");
        }
    }

private:
    double computeMaxScale() const {
        if (basePixmap.isNull()) return 1.56;
        int w = basePixmap.width();
        int h = basePixmap.height();
        int maxDim = (std::min(w, h) <= 256) ? 8192 : 4096;
        double dimCap = static_cast<double>(maxDim) / static_cast<double>(std::max(w, h));
        // Allow more zoom for small dimensions but cap to a sane upper bound.
        return std::clamp(std::max(1.56, dimCap * 2.0), 0.1, 8.0);
    }

    void updatePixmap() {
        try {
            if (basePixmap.isNull() || scale <= 0.0) return;
            if (updatingPixmap.test_and_set()) {
                // Skip re-entrant calls that can happen when zooming rapidly during streaming.
                return;
            }
            int baseW = basePixmap.width();
            int baseH = basePixmap.height();
            QSize targetSize = (scale == 1.0)
                ? basePixmap.size()
                : QSize(std::max(1, int(std::lround(baseW * scale))),
                        std::max(1, int(std::lround(baseH * scale))));

            int maxDim = (std::min(baseW, baseH) <= 256) ? 8192 : 4096;
            if (targetSize.width() > maxDim || targetSize.height() > maxDim) {
                double factor = static_cast<double>(maxDim) / static_cast<double>(std::max(targetSize.width(), targetSize.height()));
                targetSize.setWidth(std::max(1, int(std::lround(targetSize.width() * factor))));
                targetSize.setHeight(std::max(1, int(std::lround(targetSize.height() * factor))));
                logMessage(QString("updatePixmap clamped target to %1x%2").arg(targetSize.width()).arg(targetSize.height()));
            }

            label->setPixmap(basePixmap);
            label->resize(targetSize);
            label->setAlignment(Qt::AlignCenter);
            effectiveScale = static_cast<double>(targetSize.width()) / static_cast<double>(baseW);
            logMessage(QString("updatePixmap scaled=%1x%2 scaleReq=%3 scaleEff=%4")
                       .arg(targetSize.width()).arg(targetSize.height())
                       .arg(scale,0,'f',2).arg(effectiveScale,0,'f',2));
            if (onZoomChanged) onZoomChanged(effectiveScale);
            updatingPixmap.clear();
        } catch (const std::exception& e) {
            logMessage(QString("updatePixmap exception: %1").arg(e.what()));
            updatingPixmap.clear();
        } catch (...) {
            logMessage("updatePixmap exception: unknown");
            updatingPixmap.clear();
        }
    }

    QLabel* label;
    QImage lastImage;
    QPixmap basePixmap;
    double scale;
    double effectiveScale;
    bool hasImage;
    int zoomSteps;
    std::atomic_flag updatingPixmap = ATOMIC_FLAG_INIT;
    std::function<void(double)> onZoomChanged;
};

static QString formatTimeSeconds(double seconds) {
    if (seconds < 0) seconds = 0;
    int totalMs = static_cast<int>(std::lround(seconds * 1000.0));
    int ms = totalMs % 1000;
    int totalSec = totalMs / 1000;
    int s = totalSec % 60;
    int totalMin = totalSec / 60;
    int m = totalMin % 60;
    int h = totalMin / 60;
    if (h > 0) {
        return QString("%1:%2:%3.%4")
            .arg(h,2,10,QChar('0'))
            .arg(m,2,10,QChar('0'))
            .arg(s,2,10,QChar('0'))
            .arg(ms,3,10,QChar('0'));
    }
    return QString("%1:%2.%3")
        .arg(m,2,10,QChar('0'))
        .arg(s,2,10,QChar('0'))
        .arg(ms,3,10,QChar('0'));
}

class ViewerWindow : public QWidget {
public:
    ViewerWindow(QWidget* parent=nullptr)
        : QWidget(parent), fps(0.0) {
        setWindowFlags(Qt::Window);
        setWindowTitle("Capture Viewer");
        resize(1100, 800);
        setMinimumSize(800, 600);

        imageView = new ZoomImageView;
        imageView->setMinimumSize(640, 480);
        imageView->setStyleSheet("background:#000;");
        imageView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        frameLabel = new QLabel("Frame: -- / --");
        timeLabel = new QLabel("Time: -- / --");
        frameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        timeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);

        folderEdit = new QLineEdit;
        folderEdit->setPlaceholderText("Select capture folder...");
        auto browseBtn = new QPushButton("...");
        auto loadBtn = new QPushButton("Load");
        recentCombo = new QComboBox;
        recentCombo->setMinimumWidth(200);

        slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 0);
        slider->setEnabled(false);

        prevBtn = new QPushButton("<");
        nextBtn = new QPushButton(">");
        prevBtn->setEnabled(false);
        nextBtn->setEnabled(false);

        auto folderRow = new QHBoxLayout;
        folderRow->addWidget(new QLabel("Folder"));
        folderRow->addWidget(folderEdit, 1);
        folderRow->addWidget(browseBtn);
        folderRow->addWidget(loadBtn);

        auto recentRow = new QHBoxLayout;
        recentRow->addWidget(new QLabel("Recent"));
        recentRow->addWidget(recentCombo, 1);

        auto navRow = new QHBoxLayout;
        navRow->addWidget(prevBtn);
        navRow->addWidget(nextBtn);
        navRow->addWidget(frameLabel, 1);

        auto infoCol = new QVBoxLayout;
        infoCol->addLayout(folderRow);
        infoCol->addLayout(recentRow);
        infoCol->addWidget(timeLabel);
        infoCol->addLayout(navRow);
        infoCol->addWidget(slider);
        infoCol->addStretch(1);

        auto rightPane = new QWidget;
        rightPane->setLayout(infoCol);
        rightPane->setMinimumWidth(320);

        auto layout = new QHBoxLayout;
        layout->addWidget(imageView, 3);
        layout->addWidget(rightPane, 1);
        setLayout(layout);

        imageView->setZoomChanged(nullptr);

        QObject::connect(browseBtn, &QPushButton::clicked, [this](){
            QString dir = QFileDialog::getExistingDirectory(this, "Select capture folder", folderEdit->text());
            if (!dir.isEmpty()) folderEdit->setText(dir);
        });
        QObject::connect(loadBtn, &QPushButton::clicked, [this](){
            loadFolder(folderEdit->text());
        });
        QObject::connect(recentCombo, &QComboBox::activated, [this](int idx){
            if (idx < 0) return;
            QString dir = recentCombo->itemText(idx);
            if (!dir.isEmpty()) {
                folderEdit->setText(dir);
                loadFolder(dir);
            }
        });
        QObject::connect(slider, &QSlider::valueChanged, [this](int v){
            loadFrame(v);
        });
        QObject::connect(prevBtn, &QPushButton::clicked, [this](){
            if (frameFiles.isEmpty()) return;
            int v = std::max(0, slider->value() - 1);
            slider->setValue(v);
        });
        QObject::connect(nextBtn, &QPushButton::clicked, [this](){
            if (frameFiles.isEmpty()) return;
            int v = std::min(slider->maximum(), slider->value() + 1);
            slider->setValue(v);
        });

        auto leftShortcut = new QShortcut(QKeySequence(Qt::Key_Left), this);
        auto rightShortcut = new QShortcut(QKeySequence(Qt::Key_Right), this);
        auto ctrlLeftShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left), this);
        auto ctrlRightShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right), this);
        auto pageUpShortcut = new QShortcut(QKeySequence(Qt::Key_PageUp), this);
        auto pageDownShortcut = new QShortcut(QKeySequence(Qt::Key_PageDown), this);
        QObject::connect(leftShortcut, &QShortcut::activated, [this](){
            stepFrames(-1);
        });
        QObject::connect(rightShortcut, &QShortcut::activated, [this](){
            stepFrames(1);
        });
        QObject::connect(ctrlLeftShortcut, &QShortcut::activated, [this](){
            stepFrames(-5);
        });
        QObject::connect(ctrlRightShortcut, &QShortcut::activated, [this](){
            stepFrames(5);
        });
        QObject::connect(pageUpShortcut, &QShortcut::activated, [this](){
            stepFrames(-10);
        });
        QObject::connect(pageDownShortcut, &QShortcut::activated, [this](){
            stepFrames(10);
        });

        loadRecentFolders();
    }

private:
    void stepFrames(int delta) {
        if (frameFiles.isEmpty()) return;
        int v = std::clamp(slider->value() + delta, 0, slider->maximum());
        slider->setValue(v);
    }

    void loadRecentFolders() {
        QSettings settings;
        QStringList recent = settings.value("viewer/recentFolders").toStringList();
        recentCombo->clear();
        for (const QString& path : recent) {
            recentCombo->addItem(path);
        }
    }

    void updateRecentFolders(const QString& dirPath) {
        QSettings settings;
        QStringList recent = settings.value("viewer/recentFolders").toStringList();
        recent.removeAll(dirPath);
        recent.prepend(dirPath);
        while (recent.size() > 10) recent.removeLast();
        settings.setValue("viewer/recentFolders", recent);
        recentCombo->clear();
        for (const QString& path : recent) {
            recentCombo->addItem(path);
        }
    }

    void loadFolder(const QString& dirPath) {
        QDir dir(dirPath);
        if (!dir.exists()) {
            QMessageBox::warning(this, "Folder not found", "The selected folder does not exist.");
            return;
        }
        QStringList filters;
        filters << "*.tif" << "*.tiff" << "*.TIF" << "*.TIFF";
        frameFiles = dir.entryList(filters, QDir::Files, QDir::Name);
        for (QString& f : frameFiles) {
            f = dir.absoluteFilePath(f);
        }
        fps = readFpsFromInfo(dir.absoluteFilePath("capture_info.txt"));
        slider->setEnabled(!frameFiles.isEmpty());
        prevBtn->setEnabled(!frameFiles.isEmpty());
        nextBtn->setEnabled(!frameFiles.isEmpty());
        int count = static_cast<int>(frameFiles.size());
        slider->setRange(0, std::max(0, count - 1));
        slider->setValue(0);
        updateTimeLabel(0);
        if (frameFiles.isEmpty()) {
            frameLabel->setText("Frame: -- / --");
        } else {
            frameLabel->setText(QString("Frame: %1 / %2").arg(1).arg(count));
            updateRecentFolders(dir.absolutePath());
        }
    }

    double readFpsFromInfo(const QString& infoPath) const {
        QFile f(infoPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return 0.0;
        QTextStream ts(&f);
        double foundFps = 0.0;
        while (!ts.atEnd()) {
            QString line = ts.readLine().trimmed();
            if (line.startsWith("Internal FPS:", Qt::CaseInsensitive) ||
                line.startsWith("FPS:", Qt::CaseInsensitive)) {
                QStringList parts = line.split(":");
                if (parts.size() >= 2) {
                    bool ok = false;
                    double val = parts.last().trimmed().toDouble(&ok);
                    if (ok) foundFps = val;
                }
            }
        }
        return foundFps;
    }

    void loadFrame(int index) {
        if (frameFiles.isEmpty()) return;
        int count = static_cast<int>(frameFiles.size());
        index = std::clamp(index, 0, count - 1);
        QImageReader reader(frameFiles.at(index));
        reader.setAutoTransform(true);
        QImage img = reader.read();
        if (img.isNull()) {
            QMessageBox::warning(this, "Read error", "Failed to load image:\n" + reader.errorString());
            return;
        }
        imageView->setImage(img);
        frameLabel->setText(QString("Frame: %1 / %2").arg(index + 1).arg(count));
        updateTimeLabel(index);
    }

    void updateTimeLabel(int index) {
        int count = static_cast<int>(frameFiles.size());
        if (fps <= 0.0 || count == 0) {
            timeLabel->setText("Time: -- / --");
            return;
        }
        double totalSec = static_cast<double>(count) / fps;
        double currentSec = static_cast<double>(index) / fps;
        timeLabel->setText(QString("Time: %1 / %2").arg(formatTimeSeconds(currentSec)).arg(formatTimeSeconds(totalSec)));
    }

    ZoomImageView* imageView;
    QLabel* frameLabel;
    QLabel* timeLabel;
    QLineEdit* folderEdit;
    QComboBox* recentCombo;
    QSlider* slider;
    QPushButton* prevBtn;
    QPushButton* nextBtn;
    QStringList frameFiles;
    double fps;
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

void installLogTees() {
    static LogTeeBuf loggerBuf(std::cout.rdbuf(), [](const QString& m){ logMessage(m); });
    static std::ostream loggerStream(&loggerBuf);
    std::cout.rdbuf(loggerStream.rdbuf());
    std::cerr.rdbuf(loggerStream.rdbuf());
}
} // namespace


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("Hamamatsu");
    QCoreApplication::setApplicationName("qt_hama_gui");

    gLogPath = QCoreApplication::applicationDirPath() + "/session_log.txt";
    gLogFile.setFileName(gLogPath);
    if (QFile::exists(gLogPath)) QFile::remove(gLogPath);
    pruneLogs();
    qInstallMessageHandler(qtLogHandler);
    std::set_terminate(termHandler);
    installLogTees();

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
    auto startBtn = new QPushButton("Start");
    auto stopBtn = new QPushButton("Stop");
    auto reconnectBtn = new QPushButton("Reconnect");
    auto applyBtn = new QPushButton("Apply Settings");
    auto viewerBtn = new QPushButton("Viewer");
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

    auto bitsCombo = new QComboBox;
    bitsCombo->addItems({"8","12","16"});
    bitsCombo->setCurrentIndex(0); // default 8-bit

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
    auto saveOpenBtn = new QPushButton("Open Folder");
    auto saveStartBtn = new QPushButton("Start Save");
    auto saveStopBtn = new QPushButton("Stop Save");
    saveStopBtn->setEnabled(false);
    auto captureBtn = new QPushButton("Capture Frame");
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
    grid->addWidget(new QLabel("Bits"),5,0);
    grid->addWidget(bitsCombo,5,1);
    grid->addWidget(new QLabel("Exposure (ms)"),6,0);
    grid->addWidget(exposureSpin,6,1);
    grid->addWidget(new QLabel("Readout speed"),7,0);
    grid->addWidget(readoutCombo,7,1);
    grid->addWidget(new QLabel("Display every Nth frame"),8,0);
    grid->addWidget(displayEverySpin,8,1);
    grid->addWidget(logCheck,9,0,1,2);
    tabFormats->setLayout(grid);

    tabWidget->addTab(tabFormats, "Formats / Speed");

    auto saveLayout = new QGridLayout;
    saveLayout->addWidget(new QLabel("Save path"),0,0);
    saveLayout->addWidget(savePathEdit,0,1);
    saveLayout->addWidget(saveBrowseBtn,0,2);
    saveLayout->addWidget(saveOpenBtn,0,3);
    saveLayout->addWidget(saveInfoLabel,1,0,1,4);
    saveLayout->addWidget(saveStartBtn,2,2);
    saveLayout->addWidget(saveStopBtn,2,3);
    saveLayout->addWidget(captureBtn,3,2,1,2);
    auto saveWidget = new QWidget;
    saveWidget->setLayout(saveLayout);
    tabWidget->addTab(saveWidget, "Save");

    auto btnRow = new QHBoxLayout;
    btnRow->addWidget(startBtn);
    btnRow->addWidget(stopBtn);
    btnRow->addWidget(reconnectBtn);

    controlLayout->addWidget(tabWidget);
    controlLayout->addWidget(viewerBtn);
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

    imageView->setZoomChanged(nullptr);

    DcamController controller(&window);
    FrameGrabber grabber(&controller);
    QImage lastFrame;
    FrameMeta lastMeta{};
    bool viewerOnly = false;

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
        int bits = bitsCombo->currentText().toInt();
        int pixel = (bits > 8) ? DCAM_PIXELTYPE_MONO16 : DCAM_PIXELTYPE_MONO8;
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

    auto setViewerOnly = [&](){
        viewerOnly = true;
        statusLabel->setText("Viewer-only mode (camera init failed).");
        startBtn->setEnabled(false);
        stopBtn->setEnabled(false);
        reconnectBtn->setEnabled(false);
        applyBtn->setEnabled(false);
        tabWidget->setEnabled(false);
    };

    auto doInit = [&]()->bool{
        QString err = controller.initAndOpen();
        if (!err.isEmpty()) {
            statusLabel->setText("Init error: " + err);
            auto choice = QMessageBox::question(
                &window,
                "Init failed",
                "Camera init failed:\n" + err + "\n\nLaunch viewer-only mode?",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
            if (choice == QMessageBox::Yes) {
                logLine("Init failed; switching to viewer-only mode.");
                setViewerOnly();
                return false;
            }
            QMetaObject::invokeMethod(&app, "quit", Qt::QueuedConnection);
            return false;
        } else {
            statusLabel->setText("Initialized.");
            refreshExposureLimits();
            // Force default exposure to 10 ms on camera and UI
            dcamprop_setvalue(controller.handle(), DCAM_IDPROP_EXPOSURETIME, 0.010);
            exposureSpin->setValue(10.0);
            // Apply selected bits/pixel type on init
            int bits = bitsCombo->currentText().toInt();
            int pixel = (bits > 8) ? DCAM_PIXELTYPE_MONO16 : DCAM_PIXELTYPE_MONO8;
            dcamprop_setvalue(controller.handle(), DCAM_IDPROP_IMAGE_PIXELTYPE, pixel);
            dcamprop_setvalue(controller.handle(), DCAM_IDPROP_BITSPERCHANNEL, bits);
            logLine("Init success");
            return true;
        }
    };

    QObject::connect(reconnectBtn, &QPushButton::clicked, [&](){
        QString err = controller.reconnect();
        if (!err.isEmpty()) statusLabel->setText("Reconnect error: " + err);
        else {
            statusLabel->setText("Reconnected.");
            refreshExposureLimits();
        }
    });

    QObject::connect(startBtn, &QPushButton::clicked, [&](){
        if (viewerOnly) return;
        if (controller.isOpened()) {
            int bits = bitsCombo->currentText().toInt();
            int pixel = (bits > 8) ? DCAM_PIXELTYPE_MONO16 : DCAM_PIXELTYPE_MONO8;
            dcamprop_setvalue(controller.handle(), DCAM_IDPROP_IMAGE_PIXELTYPE, pixel);
            dcamprop_setvalue(controller.handle(), DCAM_IDPROP_BITSPERCHANNEL, bits);
        }
        QString err = controller.start();
        if (!err.isEmpty()) statusLabel->setText("Start error: " + err);
        else {
            statusLabel->setText("Capture started.");
            grabber.startGrabbing();
        }
    });

    QObject::connect(stopBtn, &QPushButton::clicked, [&](){
        if (viewerOnly) return;
        grabber.stopGrabbing();
        controller.stop();
        statusLabel->setText("Capture stopped.");
    });

    QObject::connect(applyBtn, &QPushButton::clicked, [&](){
        if (viewerOnly) return;
        applySettings();
    });

    QPointer<ViewerWindow> viewerWindow;
    QObject::connect(viewerBtn, &QPushButton::clicked, [&](){
        if (viewerWindow) {
            viewerWindow->raise();
            viewerWindow->activateWindow();
            return;
        }
        viewerWindow = new ViewerWindow(nullptr);
        viewerWindow->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(viewerWindow, &QObject::destroyed, [&](){ viewerWindow = nullptr; });
        viewerWindow->show();
    });

    // Save state
    auto saveBuffer = std::make_shared<std::vector<QImage>>();
    auto saveMutex = std::make_shared<QMutex>();
    std::atomic<bool> recording{false};
    std::atomic<bool> saving{false};
    QElapsedTimer recordTimer;
    QDateTime recordStartTime;
    std::atomic<int> recordedFrames{0};
    QTimer saveInfoTimer;
    saveInfoTimer.setInterval(200);

    QObject::connect(saveBrowseBtn, &QPushButton::clicked, [&](){
        QString dir = QFileDialog::getExistingDirectory(&window, "Select save directory", savePathEdit->text());
        if (!dir.isEmpty()) savePathEdit->setText(dir);
    });
    QObject::connect(saveOpenBtn, &QPushButton::clicked, [&](){
        QString dir = savePathEdit->text();
        if (dir.isEmpty()) dir = QCoreApplication::applicationDirPath();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    QObject::connect(captureBtn, &QPushButton::clicked, [&](){
        if (lastFrame.isNull()) {
            statusLabel->setText("No frame to capture");
            return;
        }
        QString baseDir = savePathEdit->text();
        if (baseDir.isEmpty()) baseDir = QCoreApplication::applicationDirPath();
        QDir dir(baseDir);
        dir.mkpath(".");
        QString fname = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz") + ".tiff";
        QString outPath = dir.filePath(fname);
        if (lastFrame.save(outPath, "TIFF")) {
            statusLabel->setText("Captured: " + fname);
            logLine("Captured frame to " + outPath);
        } else {
            statusLabel->setText("Capture failed");
        }
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
        recordStartTime = QDateTime::currentDateTime();
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

        FrameMeta metaCopy = lastMeta;
        double expMsCopy = exposureSpin->value();
        QString recordStartStr = recordStartTime.toString("yyyy-MM-dd hh:mm:ss.zzz");

        std::thread([frames, outDir, logLine, statusLabel, savingDialog, savingProgress, totalFrames, metaCopy, expMsCopy, recordStartStr, &saving](){
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
            // Write metadata file
            QFile infoFile(outDir + "/capture_info.txt");
            if (infoFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&infoFile);
                ts << "Start: " << recordStartStr << "\n";
                ts << "Frames: " << frames->size() << "\n";
                ts << "Resolution: " << metaCopy.width << " x " << metaCopy.height << "\n";
                ts << "Binning: " << metaCopy.binning << "\n";
                ts << "Bits: " << metaCopy.bits << "\n";
                ts << "Exposure(ms): " << expMsCopy << "\n";
                ts << "Internal FPS: " << metaCopy.internalFps << "\n";
                ts << "Readout speed: " << metaCopy.readoutSpeed << "\n";
                ts.flush();
                infoFile.close();
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
        lastFrame = img;
        }
        lastMeta = meta;
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
