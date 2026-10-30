// =============================================================================
//  camerawindow.cpp —— 主窗口的实现
//
//  界面结构在 camerawindow.ui 里，这个文件只放两件事：
//      1) 构造函数 / wireUpUi()：把 .ui 表达不了的东西接上
//         （下拉框的 userData、信号连接、拉伸因子）
//      2) 业务逻辑：采集线程编排、模式与参数、快照、关窗口流程
// =============================================================================

#include "camerawindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVideoWidget>          // .ui 里用到，必须在 ui_camerawindow.h 之前

#include <QCamera>
#include <QCameraDevice>
#include <QMediaCaptureSession>
#include <QMediaDevices>

#include "ui_camerawindow.h"     // 由 camerawindow.ui 生成（qmake 的 FORMS）

#include "processing.h"
#include "videoview.h"

#include <algorithm>
#include <cmath>

// 关窗口时最多等采集线程多久（毫秒）。超时就不等了直接退出 ——
// 正常情况 0.4 秒左右线程就自己收尾完了，这个值只是防驱动卡死的兜底。
static constexpr int kShutdownGraceMs = 1200;

// =============================================================================
//  构造 / 析构
// =============================================================================

CameraWindow::CameraWindow(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::CameraWindow)
{
    ui->setupUi(this);

    // ---- 采集线程：QThread 只跑事件循环，CaptureWorker 被 moveToThread 搬进去 ----
    m_workerThread = new QThread(this);
    m_worker = new CaptureWorker;            // 故意不给父对象：它要 moveToThread
    m_worker->moveToThread(m_workerThread);
    // 线程结束（quit + 事件循环退出）后由 Qt 在正确的线程里删掉 worker
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &CaptureWorker::framesReady, this, &CameraWindow::onFrames);
    connect(m_worker, &CaptureWorker::statsReady, this, &CameraWindow::onStats);
    connect(m_worker, &CaptureWorker::opened, this, [this](const QString &info) {
        m_cvActive = true;
        setOpenCvStatus(QStringLiteral("采集中 —— ") + info, false);
    });
    connect(m_worker, &CaptureWorker::openFailed, this, [this](const QString &message) {
        m_cvActive = false;
        setOpenCvStatus(message, true);
        ui->openBtn->setEnabled(true);
        ui->stopBtn->setEnabled(false);
    });
    connect(m_worker, &CaptureWorker::probeResult, this,
            [this](int index, const QString &backend, int w, int h, bool ok) {
                ui->probeLog->appendPlainText(
                    ok ? QStringLiteral("设备 %1 ： 可用   后端=%2   %3x%4")
                             .arg(index).arg(backend).arg(w).arg(h)
                       : QStringLiteral("设备 %1 ： 打不开").arg(index));
            });
    connect(m_worker, &CaptureWorker::probeFinished, this, [this](int available) {
        m_probeActive = false;
        ui->probeLog->appendPlainText(
            available > 0
                ? QStringLiteral("---- 探测结束：找到 %1 个可用设备 ----").arg(available)
                : QStringLiteral("---- 探测结束：一个都没打开 ----\n"
                                 "若设备管理器里能看到摄像头，检查程序所在目录是否带"
                                 "「低完整性」标签：icacls <目录>"));
        ui->probeBtn->setEnabled(true);
        ui->openBtn->setEnabled(true);
    });

    // 停止是异步的：worker 在采集线程里交还摄像头（~0.3s），完成后发 stopped()
    connect(m_worker, &CaptureWorker::stopped, this, [this] {
        m_cvActive = false;
        ui->statsLabel->clear();

        // 探测期间按钮归探测流程管（probeFinished 收尾），这里别去插手，
        // 否则会把「打开摄像头」错误地重新点亮
        if (!m_probeActive) {
            ui->openBtn->setEnabled(true);
            ui->stopBtn->setEnabled(false);
            setOpenCvStatus(QStringLiteral("已停止。"), false);
        }

        // 之前按了「开始预览」但被我们挡下来的，现在轮到它了
        if (m_pendingQtCamera) {
            m_pendingQtCamera = false;
            startQtCamera();
        }
    });

    // 停机完成 → 让采集线程的事件循环退出（run() 返回后会发 finished）
    connect(m_worker, &CaptureWorker::shutdownDone, m_workerThread, &QThread::quit);

    m_snapshotDir = QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("snapshots"));

    // 界面上"代码才有"的部分（下拉框数据、信号连接）
    wireUpUi();

    refreshQtDevices();
    onModeChanged(0);

    // 界面搭完再启动线程，免得 worker 的信号打在没有控件的窗口上
    m_workerThread->start();
}

// -----------------------------------------------------------------------------
//  把 .ui 表达不了的东西接上
//      - 下拉框的条目：后端/分辨率要带 int / QSize 的 userData，
//        处理模式是从 processing 的模式表生成的 —— 这些只能在代码里填
//      - 信号连接：Designer 的 <connections> 只能连控件之间的槽，
//        我们的槽都在 CameraWindow 上，写在代码里更清楚
// -----------------------------------------------------------------------------
void CameraWindow::wireUpUi()
{
    // 分割器：uic 不认识 QSplitter 的 QList<int> sizes 属性，
    // 所以初始列宽只能在代码里给（Designer 里那两条线也只能在运行时生效）
    ui->splitter->setSizes({ 900, 380 });
    ui->splitter->setStretchFactor(0, 1);
    ui->splitter->setStretchFactor(1, 0);

    // 参数滑块统一 0..1000，再线性映射到各模式自己的取值范围
    ui->p1Slider->setRange(0, 1000);
    ui->p2Slider->setRange(0, 1000);

    // ---- 摄像头 ----
    ui->backendBox->addItem(QStringLiteral("自动（先 DSHOW，失败再 MSMF）"),
                            int(CaptureWorker::Backend::Auto));
    ui->backendBox->addItem(QStringLiteral("DSHOW（快，推荐）"),
                            int(CaptureWorker::Backend::DSHOW));
    ui->backendBox->addItem(QStringLiteral("MSMF（Windows 新框架，但打开慢）"),
                            int(CaptureWorker::Backend::MSMF));

    ui->resolutionBox->addItem(QStringLiteral("640 x 480"), QSize(640, 480));
    ui->resolutionBox->addItem(QStringLiteral("1280 x 720"), QSize(1280, 720));
    ui->resolutionBox->addItem(QStringLiteral("1920 x 1080"), QSize(1920, 1080));
    ui->resolutionBox->setCurrentIndex(1);

    // ---- 处理模式：直接从模式表生成，加模式不用动界面 ----
    for (int i = 0; i < processModeCount(); ++i)
        ui->modeBox->addItem(QString::fromUtf8(processModeTable()[i].name));

    // ---- 显示 ----
    ui->displayBox->addItem(QStringLiteral("处理结果"));
    ui->displayBox->addItem(QStringLiteral("原始画面"));
    ui->displayBox->addItem(QStringLiteral("左右对比（原图 | 处理后）"));

    // ---- 信号连接 ----
    connect(ui->qtRefreshBtn, &QPushButton::clicked, this, &CameraWindow::refreshQtDevices);
    connect(ui->qtStartBtn, &QPushButton::clicked, this, &CameraWindow::startQtCamera);
    connect(ui->qtStopBtn, &QPushButton::clicked, this, &CameraWindow::stopQtCamera);

    connect(ui->openBtn, &QPushButton::clicked, this, &CameraWindow::startOpenCv);
    connect(ui->stopBtn, &QPushButton::clicked, this, &CameraWindow::stopOpenCv);
    connect(ui->probeBtn, &QPushButton::clicked, this, &CameraWindow::startProbe);

    connect(ui->modeBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CameraWindow::onModeChanged);
    connect(ui->p1Slider, &QSlider::valueChanged, this, &CameraWindow::pushParams);
    connect(ui->p2Slider, &QSlider::valueChanged, this, &CameraWindow::pushParams);
    connect(ui->mirrorBox, &QCheckBox::toggled, this, &CameraWindow::pushParams);

    connect(ui->displayBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFrames(QImage(), QImage()); });

    connect(ui->snapshotBtn, &QPushButton::clicked, this, &CameraWindow::saveSnapshot);
    connect(ui->openDirBtn, &QPushButton::clicked, this, &CameraWindow::openSnapshotDir);
}

CameraWindow::~CameraWindow()
{
    if (m_workerThread && m_workerThread->isRunning() && !m_abandonThread) {
        // 正常情况下 closeEvent 已经把线程停干净了，这里只是兜底。
        // 交给事件循环退出，最多等 300ms；还不停就放弃等待 ——
        // QThread 在运行中被析构会让 Qt 直接 abort 进程，所以只能
        // 把它（和里面的 worker）从父子树里摘出来故意泄漏，由进程退出时回收。
        m_workerThread->quit();
        if (!m_workerThread->wait(300)) {
            m_workerThread->setParent(nullptr);
            m_abandonThread = true;
        }
    }
    if (m_camera)
        m_camera->stop();

    delete ui;
}

// -----------------------------------------------------------------------------
//  关窗口
//
//  如果把界面线程 wait() 在采集线程上，窗口就完全不能重绘 —— 用户看到的是
//  "点了关闭没反应"。而采集线程收尾有几处躲不掉的耗时：
//      grabOneFrame() 里的 cap.read()   最坏 ~0.4s（DSHOW）
//      shutdown() 里的 cap.release()    实测 ~0.3s（MSMF / DSHOW 都要）
//
//  现在改成全异步：
//      1) 往采集线程排一个 shutdown()，它停掉定时器、交还摄像头，然后发 shutdownDone()；
//      2) shutdownDone() → QThread::quit()（在构造函数里连好），事件循环退出、线程结束；
//      3) 线程的 finished() → 真正 close()。
//      全程界面线程不 wait()，窗口继续响应事件并显示"正在关闭摄像头……"。
//      驱动要是真卡死了，超过宽限期就直接 quit() 走人，不再等它收尾。
// -----------------------------------------------------------------------------
void CameraWindow::closeEvent(QCloseEvent *event)
{
    if (!m_closing && m_workerThread && m_workerThread->isRunning()) {
        m_closing = true;
        event->ignore();                     // 先别关，等线程收尾

        setEnabled(false);
        setOpenCvStatus(QStringLiteral("正在关闭摄像头……"));

        if (m_worker)
            QMetaObject::invokeMethod(m_worker.data(), &CaptureWorker::shutdown,
                                      Qt::QueuedConnection);

        connect(m_workerThread, &QThread::finished, this, [this] { close(); });

        // 宽限期到了还没停，就不等了：驱动卡死不该拖着整个程序陪葬
        QTimer::singleShot(kShutdownGraceMs, this, [this] {
            if (m_workerThread && m_workerThread->isRunning()) {
                m_workerThread->quit();
                m_abandonThread = true;
            }
            if (m_closing)
                close();
        });
        return;
    }

    stopQtCamera();
    event->accept();
}

void CameraWindow::autoStartCapture(int modeIndex)
{
    ui->tabs->setCurrentIndex(1);              // 切到「OpenCV 采集 + 处理」页

    if (modeIndex > 0 && modeIndex < processModeCount())
        ui->modeBox->setCurrentIndex(modeIndex);   // 触发 onModeChanged → 参数排队投递

    startOpenCv();
}

// -----------------------------------------------------------------------------
//  界面自己的启动开关
//      --autostart   启动后自动切到 OpenCV 页并打开摄像头
//      --mode N      顺便把初始处理模式设成第 N 种
//
//  解析放在这里而不是 main.cpp：这些开关是界面自己的知识，
//  入口不需要知道 --mode 的取值怎么校验、也不需要知道要延迟 200ms 再切页。
// -----------------------------------------------------------------------------
void CameraWindow::applyStartupOptions(const QStringList &args)
{
    if (!args.contains(QStringLiteral("--autostart")))
        return;

    int modeIndex = 0;
    const int at = args.indexOf(QStringLiteral("--mode"));
    if (at >= 0 && at + 1 < args.size()) {
        bool      ok = false;
        const int n  = args.at(at + 1).toInt(&ok);
        if (ok)
            modeIndex = n;
    }

    // 等窗口显示出来、布局算好之后再切页 / 开摄像头
    QTimer::singleShot(200, this, [this, modeIndex] { autoStartCapture(modeIndex); });
}

void CameraWindow::refreshQtDevices()
{
    ui->qtDeviceBox->clear();

    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    for (const QCameraDevice &device : devices)
        ui->qtDeviceBox->addItem(device.description());

    if (devices.isEmpty()) {
        ui->qtDeviceBox->addItem(QStringLiteral("（未检测到摄像头）"));
        ui->qtStatus->setStyleSheet(QStringLiteral("color:#ff7a7a;padding:6px;"));
        ui->qtStatus->setText(QStringLiteral(
            "QMediaDevices::videoInputs() 返回 0 个设备。\n\n"
            "如果设备管理器里能看到摄像头，多半是程序所在目录带「低完整性」标签，"
            "进程被降级成低完整性后 Windows 会禁止它访问摄像头和麦克风。\n"
            "检查命令：icacls <程序所在目录>   （出现 Mandatory Label\\Low 就是它）"));
    } else {
        ui->qtStatus->setStyleSheet(QStringLiteral("color:#93a0b4;padding:6px;"));
        ui->qtStatus->setText(QStringLiteral("检测到 %1 个摄像头，选一个点「开始预览」。")
                                .arg(devices.size()));
    }
}

void CameraWindow::startQtCamera()
{
    const QList<QCameraDevice> devices = QMediaDevices::videoInputs();
    if (devices.isEmpty()) {
        refreshQtDevices();
        return;
    }

    // 摄像头不能被两条链路同时打开。OpenCV 那边是异步停的，
    // 所以这一轮先排队停它，等它发 stopped() 再由信号回调把自己叫回来。
    // （这里不能 wait()，否则又变成关窗口那种假死了。）
    if (m_cvActive) {
        m_pendingQtCamera = true;
        ui->qtStatus->setStyleSheet(QStringLiteral("color:#93a0b4;padding:6px;"));
        ui->qtStatus->setText(QStringLiteral("正在等待 OpenCV 释放摄像头……"));
        stopOpenCv();
        return;
    }

    stopQtCamera();

    int index = ui->qtDeviceBox->currentIndex();
    index = std::max(0, std::min(index, int(devices.size()) - 1));
    const QCameraDevice device = devices.at(index);

    m_camera  = new QCamera(device, this);
    m_session = new QMediaCaptureSession(this);
    m_session->setCamera(m_camera);
    m_session->setVideoOutput(ui->qtView);

    connect(m_camera, &QCamera::errorOccurred, this,
            [this](QCamera::Error code, const QString &text) {
                ui->qtStatus->setStyleSheet(QStringLiteral("color:#ff7a7a;padding:6px;"));
                ui->qtStatus->setText(QStringLiteral("摄像头错误 [%1]：%2").arg(int(code)).arg(text));
            });
    connect(m_camera, &QCamera::activeChanged, this, [this, device](bool active) {
        if (!active)
            return;
        ui->qtStatus->setStyleSheet(QStringLiteral("color:#8ee08e;padding:6px;"));
        ui->qtStatus->setText(QStringLiteral("预览中 —— %1").arg(device.description()));
    });

    m_camera->start();

    ui->qtStartBtn->setEnabled(false);
    ui->qtStopBtn->setEnabled(true);
    ui->qtStatus->setStyleSheet(QStringLiteral("color:#93a0b4;padding:6px;"));
    ui->qtStatus->setText(QStringLiteral("正在打开摄像头：%1 ……").arg(device.description()));
}

void CameraWindow::stopQtCamera()
{
    if (m_camera) {
        m_camera->stop();
        m_camera->deleteLater();
        m_camera = nullptr;
    }
    if (m_session) {
        m_session->deleteLater();
        m_session = nullptr;
    }
    ui->qtStartBtn->setEnabled(true);
    ui->qtStopBtn->setEnabled(false);
}

// -----------------------------------------------------------------------------
//  OpenCV 页：打开 / 停止 / 探测
//
//  全部是"排一个队列调用就返回"，界面线程不做任何等待。
//  顺序由 Qt 队列保证：先排 stopCapture 再排 startCapture，worker 一定按这个顺序执行，
//  所以不需要任何标志位去保证"先关再开"。
// -----------------------------------------------------------------------------
void CameraWindow::startOpenCv()
{
    if (!m_worker)
        return;

    // 摄像头不能被两条链路同时打开：Qt 那边直接停（它不阻塞）
    stopQtCamera();

    const QSize res     = ui->resolutionBox->currentData().toSize();
    const int   backend = ui->backendBox->currentData().toInt();
    const int   index   = ui->deviceIndex->value();
    const int   fps     = ui->fpsBox->value();

    pushParams();

    ui->openBtn->setEnabled(false);
    ui->stopBtn->setEnabled(true);
    setOpenCvStatus(QStringLiteral("正在打开设备 %1 ……（先试 DSHOW，失败自动换 MSMF）").arg(index),
                    false);

    QMetaObject::invokeMethod(m_worker.data(), &CaptureWorker::startCapture,
                              Qt::QueuedConnection,
                              index, backend, res.width(), res.height(), fps);
}

void CameraWindow::stopOpenCv()
{
    if (!m_worker)
        return;

    if (!m_cvActive) {
        // 本来就没在采集，不用往线程里排东西
        ui->openBtn->setEnabled(true);
        ui->stopBtn->setEnabled(false);
        return;
    }

    // 异步停：worker 交还摄像头（~0.3s）后发 stopped()，那时再恢复按钮
    ui->openBtn->setEnabled(false);
    ui->stopBtn->setEnabled(false);
    setOpenCvStatus(QStringLiteral("正在停止……"), false);

    QMetaObject::invokeMethod(m_worker.data(), &CaptureWorker::stopCapture,
                              Qt::QueuedConnection);
}

void CameraWindow::startProbe()
{
    if (!m_worker)
        return;

    // 不用先调 stopOpenCv()：worker 的 startProbe() 自己会把采集停掉并发 stopped()。
    // 而且停止是异步的，这里再插一脚反而会把按钮状态搅乱。
    m_probeActive = true;

    ui->probeLog->clear();
    ui->probeBtn->setEnabled(false);
    ui->openBtn->setEnabled(false);
    ui->stopBtn->setEnabled(false);
    ui->probeLog->appendPlainText(QStringLiteral(
        "OpenCV 没有「枚举摄像头」的 API，只能按序号硬试，每个序号最多等几秒……"));

    QMetaObject::invokeMethod(m_worker.data(), &CaptureWorker::startProbe,
                              Qt::QueuedConnection, 3);
}

// -----------------------------------------------------------------------------
//  每一帧：缓存 + 按显示模式合成 + 送显示
// -----------------------------------------------------------------------------
void CameraWindow::onFrames(const QImage &raw, const QImage &processed)
{
    if (!raw.isNull())
        m_lastRaw = raw;
    if (!processed.isNull())
        m_lastProcessed = processed;

    if (m_lastRaw.isNull() && m_lastProcessed.isNull())
        return;

    ui->view->setImage(composeDisplay(m_lastRaw, m_lastProcessed));
}

QImage CameraWindow::composeDisplay(const QImage &raw, const QImage &processed) const
{
    const int mode = ui->displayBox ? ui->displayBox->currentIndex() : 0;

    if (mode == 1)
        return raw;
    if (mode != 2 || raw.isNull() || processed.isNull())
        return processed;

    // 左右对比：中间留缝，左上角标一下哪边是哪个
    const int    gap = 8;
    const QSize  size(raw.width() * 2 + gap, std::max(raw.height(), processed.height()));
    QImage       out(size, QImage::Format_RGB32);
    out.fill(QColor(13, 16, 21));

    QPainter painter(&out);
    painter.drawImage(0, 0, raw);
    painter.drawImage(raw.width() + gap, 0, processed);

    const QRect leftTag(0, 0, 100, 28);
    const QRect rightTag(raw.width() + gap, 0, 120, 28);
    painter.fillRect(leftTag, QColor(0, 0, 0, 150));
    painter.fillRect(rightTag, QColor(0, 0, 0, 150));
    painter.setPen(QColor(235, 240, 250));
    painter.drawText(leftTag.adjusted(8, 0, 0, 0), Qt::AlignVCenter,
                     QStringLiteral("原始画面"));
    painter.drawText(rightTag.adjusted(8, 0, 0, 0), Qt::AlignVCenter,
                     QStringLiteral("处理后"));
    return out;
}

void CameraWindow::onStats(const QString &text)
{
    ui->statsLabel->setText(text);
}

// -----------------------------------------------------------------------------
//  处理模式与参数
// -----------------------------------------------------------------------------
void CameraWindow::onModeChanged(int comboIndex)
{
    if (comboIndex < 0 || comboIndex >= processModeCount())
        return;

    const ProcessModeInfo &info = processModeTable()[comboIndex];

    ui->modeHint->setText(QString::fromUtf8(info.hint));

    // 参数滑块统一 0..1000，再线性映射到各模式自己的取值范围
    auto toSlider = [](const ParamSpec &spec) {
        if (!spec.label || spec.max <= spec.min)
            return 0;
        const double t = (spec.def - spec.min) / (spec.max - spec.min);
        return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * 1000.0));
    };

    ui->p1Slider->blockSignals(true);
    ui->p2Slider->blockSignals(true);

    if (info.p1.label) {
        ui->p1Label->setText(QString::fromUtf8(info.p1.label));
        ui->p1Label->setEnabled(true);
        ui->p1Slider->setEnabled(true);
        ui->p1Slider->setValue(toSlider(info.p1));
    } else {
        ui->p1Label->setText(QStringLiteral("参数 1（本模式不用）"));
        ui->p1Label->setEnabled(false);
        ui->p1Slider->setEnabled(false);
        ui->p1Slider->setValue(0);
    }

    if (info.p2.label) {
        ui->p2Label->setText(QString::fromUtf8(info.p2.label));
        ui->p2Label->setEnabled(true);
        ui->p2Slider->setEnabled(true);
        ui->p2Slider->setValue(toSlider(info.p2));
    } else {
        ui->p2Label->setText(QStringLiteral("参数 2（本模式不用）"));
        ui->p2Label->setEnabled(false);
        ui->p2Slider->setEnabled(false);
        ui->p2Slider->setValue(0);
    }

    ui->p1Slider->blockSignals(false);
    ui->p2Slider->blockSignals(false);

    updateParamUi();
    pushParams();
}

void CameraWindow::updateParamUi()
{
    const int idx = ui->modeBox->currentIndex();
    if (idx < 0 || idx >= processModeCount())
        return;

    const ProcessModeInfo &info = processModeTable()[idx];

    auto text = [](const ParamSpec &spec, QSlider *slider) {
        if (!spec.label)
            return QStringLiteral("--");
        const double v = spec.min + (spec.max - spec.min) * slider->value() / 1000.0;
        const bool integral = (spec.min == std::floor(spec.min)
                               && spec.max == std::floor(spec.max));
        return integral ? QString::number(static_cast<long long>(std::llround(v)))
                        : QString::number(v, 'f', 2);
    };

    ui->p1Value->setText(text(info.p1, ui->p1Slider));
    ui->p2Value->setText(text(info.p2, ui->p2Slider));
}

void CameraWindow::pushParams()
{
    if (!ui->modeBox || !m_worker)
        return;

    const int idx = ui->modeBox->currentIndex();
    if (idx < 0 || idx >= processModeCount())
        return;

    const ProcessModeInfo &info = processModeTable()[idx];

    ProcessParams params;
    params.mode   = info.mode;
    params.mirror = ui->mirrorBox->isChecked();

    params.p1 = info.p1.label
                    ? info.p1.min + (info.p1.max - info.p1.min) * ui->p1Slider->value() / 1000.0
                    : 0.0;

    // p2 即使界面不给控件，也要给个合理默认（形态学的迭代次数就走这条路）
    params.p2 = info.p2.label
                    ? info.p2.min + (info.p2.max - info.p2.min) * ui->p2Slider->value() / 1000.0
                    : (info.p2.def > 0.0 ? info.p2.def : 1.0);

    updateParamUi();

    // 队列投递到采集线程：worker 那边不需要锁，也不会和取帧抢。
    // 自定义类型要走队列连接，必须让元类型系统认识它 ——
    // Q_DECLARE_METATYPE 在 captureworker.h 里，运行时注册在 main()。
    const bool queued = QMetaObject::invokeMethod(
        m_worker.data(), &CaptureWorker::setParams, Qt::QueuedConnection, params);
    if (!queued)
        setOpenCvStatus(QStringLiteral("参数没能投递到采集线程（元类型没注册？），画面不会跟着变。"),
                        true);
}

// -----------------------------------------------------------------------------
//  快照
// -----------------------------------------------------------------------------
void CameraWindow::saveSnapshot()
{
    if (m_lastRaw.isNull() && m_lastProcessed.isNull()) {
        ui->snapshotInfo->setText(QStringLiteral("还没有画面可存。"));
        return;
    }

    if (!QDir().mkpath(m_snapshotDir)) {
        ui->snapshotInfo->setText(QStringLiteral("建不了目录：%1").arg(m_snapshotDir));
        return;
    }

    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMdd_hhmmss_zzz"));
    QStringList saved;

    if (!m_lastRaw.isNull()) {
        const QString path = QDir(m_snapshotDir).filePath(QStringLiteral("raw_%1.png").arg(stamp));
        if (m_lastRaw.save(path))
            saved << path;
    }
    if (!m_lastProcessed.isNull()) {
        const QString path = QDir(m_snapshotDir).filePath(QStringLiteral("proc_%1.png").arg(stamp));
        if (m_lastProcessed.save(path))
            saved << path;
    }

    ui->snapshotInfo->setText(saved.isEmpty()
                                ? QStringLiteral("保存失败。")
                                : QStringLiteral("已保存：\n") + saved.join(QStringLiteral("\n")));
}

void CameraWindow::openSnapshotDir()
{
    QDir().mkpath(m_snapshotDir);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_snapshotDir));
}

void CameraWindow::setOpenCvStatus(const QString &text, bool error)
{
    ui->statusLabel->setStyleSheet(error ? QStringLiteral("color:#ff7a7a;padding:6px;")
                                       : QStringLiteral("color:#93a0b4;padding:6px;"));
    ui->statusLabel->setText(text);
}
