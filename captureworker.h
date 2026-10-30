// =============================================================================
//  captureworker.h —— 采集 worker（worker 对象 + moveToThread 模型）
//
//  线程模型：
//      QThread 只管跑事件循环，CaptureWorker 是一个普通 QObject，
//      被 moveToThread() 搬到那个线程里。所有对外接口都是 **槽**，
//      由 Qt 队列连接投递到采集线程执行。
//
//  为什么用这个模型，而不是继承 QThread 重写 run()：
//      1) 队列连接天然把调用串行化，"先停止再打开"这种顺序不用自己保证；
//      2) 参数、启停都不用锁 —— 它们全在采集线程里顺序执行；
//      3) 停机就是 quit() + wait()，生命周期干净。
//      代价是采集循环必须变成事件驱动（QTimer），见 grabOneFrame()。
//
//  ⚠ 注意：这个类的方法**必须**通过队列连接调用（invokeMethod / connect）。
//     谁要是直接在界面线程里 m_worker->startCapture(...)，
//     就会在错误的线程里碰 cv::VideoCapture 和 QTimer，而且没有任何报错。
// =============================================================================

#ifndef CAPTUREWORKER_H
#define CAPTUREWORKER_H

#include <QElapsedTimer>
#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QString>

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>

#include "processing.h"

class QTimer;

// ProcessParams 要跨线程排队传递，得让 Qt 的元类型系统认识它
Q_DECLARE_METATYPE(ProcessParams)

class CaptureWorker : public QObject
{
    Q_OBJECT

public:
    // 采集后端：Windows 上 MSMF 和 DSHOW 是两条不同的路，某些摄像头只有一条能开
    enum class Backend { Auto = 0, MSMF = 1, DSHOW = 2 };

    explicit CaptureWorker(QObject *parent = nullptr);
    ~CaptureWorker() override;

    // 打开设备的逻辑是纯函数，命令行 --grab 也复用它，所以做成静态的。
    // openMs 回传整个打开过程的耗时（含校验用的第一帧），用于在界面上显示。
    static bool openCapture(cv::VideoCapture &cap, int index, Backend backend,
                            int width = 0, int height = 0,
                            QString *usedBackend = nullptr,
                            double *openMs = nullptr);
    static int  backendFlag(Backend backend);

    // 把 CAP_PROP_FOURCC 解成 "MJPG" / "YUY2" 这样的四字符码。
    // 显示出来有意义：万一 MJPG 协商失败退回 YUY2，帧率会掉一半以上，
    // 但 open() 依然返回成功 —— 不打出来根本看不出来。
    static QString fourccString(const cv::VideoCapture &cap);

public slots:
    // ---- 下面这些槽都在采集线程里执行，彼此之间不需要任何锁 ----

    // backend 用 int 传是为了跨线程排队时不用额外注册枚举类型，
    // 内部再转回 Backend
    void startCapture(int deviceIndex, int backend, int width, int height, int targetFps);
    void stopCapture();          // 停采集（线程继续活着，可以再开）
    void shutdown();             // 停机：停采集 + 通知外面可以退事件循环了
    void setParams(ProcessParams params);

    void startProbe(int maxIndex);
    void stopProbe();

signals:
    void opened(const QString &info);           // 摄像头打开成功
    void openFailed(const QString &message);    // 打开失败 / 读帧中断
    void framesReady(const QImage &raw, const QImage &processed);
    void statsReady(const QString &stats);
    void stopped();                             // 停止完成（界面靠它恢复按钮）
    void shutdownDone();                        // shutdown() 做完了
    void probeResult(int index, const QString &backend, int width, int height, bool ok);
    void probeFinished(int availableCount);

private slots:
    void grabOneFrame();     // QTimer 每 1/fps 秒叫一次
    void probeNext();        // 探测链：一次探一个序号，中间把控制权还给事件循环

private:
    void   stopCaptureInternal();
    void   emitStats(double fps, const cv::Size &size, double processMs);
    static QImage matToQImage(const cv::Mat &mat);

    cv::VideoCapture m_cap;
    QTimer          *m_timer = nullptr;   // new QTimer(this)：moveToThread 会把子对象一起带走

    // 下面这些只在采集线程里读写，所以不用加锁
    ProcessParams m_params;
    int           m_targetFps = 30;
    bool          m_capturing = false;
    int           m_failCount = 0;
    cv::Mat       m_frame;
    cv::Mat       m_processed;

    QElapsedTimer m_statsTimer;
    int           m_frameCount  = 0;
    double        m_lastProcMs  = 0.0;
    bool          m_firstFrame  = true;

    // 设备探测
    bool m_probing          = false;
    int  m_probeIndex       = 0;
    int  m_probeMax         = 3;
    int  m_probeAvailable   = 0;
};

#endif // CAPTUREWORKER_H
