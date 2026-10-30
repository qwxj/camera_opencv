// =============================================================================
//  camerawindow.h —— 主窗口
//
//  界面本身在 camerawindow.ui 里（可以用 Qt Designer 直接打开改），
//  这个类只放"界面表达不了的东西"：
//      - 下拉框的数据（后端 / 分辨率 / 处理模式都是从代码里带 userData 填的）
//      - 信号连接
//      - 采集线程的编排、快照落盘等业务逻辑
//
//  ⚠ 控件全部通过 ui->xxx 访问，不要另外再加 m_xxx 指针存控件，
//    否则在 Designer 里改了名字两边就对不上了。
// =============================================================================

#ifndef CAMERAWINDOW_H
#define CAMERAWINDOW_H

#include <QImage>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "captureworker.h"      // QPointer<CaptureWorker> 需要完整类型

QT_BEGIN_NAMESPACE
namespace Ui { class CameraWindow; }
QT_END_NAMESPACE

class QCamera;
class QCloseEvent;
class QMediaCaptureSession;
class QThread;

class CameraWindow : public QWidget
{
    Q_OBJECT

public:
    explicit CameraWindow(QWidget *parent = nullptr);
    ~CameraWindow() override;

    // 命令行 --autostart：启动后自动切到 OpenCV 页并打开摄像头
    // modeIndex > 0 时顺便先把处理模式选好（用来验证参数确实投递到了采集线程）
    void autoStartCapture(int modeIndex = 0);

    // 界面自己的启动开关（--autostart / --mode N）。
    // 和 cli 一个道理："有哪些开关"是界面自己的知识，不该塞进 main.cpp。
    void applyStartupOptions(const QStringList &args);

protected:
    // 关窗口时不能傻等采集线程（它可能正卡在 cv::VideoCapture::release() 上），
    // 否则窗口会假死好几百毫秒 —— 见 .cpp 里的实现注释
    void closeEvent(QCloseEvent *event) override;

private:
    // 把 .ui 表达不了的东西接上：下拉框条目、信号连接
    void wireUpUi();

    // ---- Qt Multimedia 页 ----
    void refreshQtDevices();
    void startQtCamera();
    void stopQtCamera();

    // ---- OpenCV 页 ----
    void startOpenCv();
    void stopOpenCv();
    void startProbe();
    void onFrames(const QImage &raw, const QImage &processed);
    void onStats(const QString &text);
    void onModeChanged(int comboIndex);
    void pushParams();
    void updateParamUi();
    void saveSnapshot();
    void openSnapshotDir();

    void   setOpenCvStatus(const QString &text, bool error = false);
    QImage composeDisplay(const QImage &raw, const QImage &processed) const;

    // ---- 界面（.ui 里的控件都在这里）----
    Ui::CameraWindow *ui = nullptr;

    // ---- Qt Multimedia 的对象（不是控件，所以不放 .ui）----
    QCamera              *m_camera  = nullptr;
    QMediaCaptureSession *m_session = nullptr;

    // ---- 采集线程：QThread 只跑事件循环，CaptureWorker 被 moveToThread 搬进去 ----
    QThread                *m_workerThread = nullptr;
    QPointer<CaptureWorker> m_worker;        // 线程结束会 deleteLater，QPointer 自动置空

    // ---- 最近一帧 ----
    QImage  m_lastRaw;
    QImage  m_lastProcessed;
    QString m_snapshotDir;

    // ---- 状态位 ----
    bool m_closing         = false;   // 已经在走"先停线程再关窗"的流程
    bool m_abandonThread   = false;   // 放弃等待采集线程了（不再 delete 它的 QThread）
    bool m_cvActive        = false;   // OpenCV 那边正在采集中（由 worker 的信号维护）
    bool m_probeActive     = false;   // 探测进行中：这期间按钮归探测流程管
    bool m_pendingQtCamera = false;   // 等 OpenCV 把摄像头交出来后再开 Qt 预览
};

#endif // CAMERAWINDOW_H
