// =============================================================================
//  captureworker.cpp
//
//  所有 public slot 都跑在采集线程里（由队列连接投递），
//  所以它们之间共享的成员变量一律不加锁 —— 这是 moveToThread 模型的主要收益。
// =============================================================================

#include "captureworker.h"

#include <QList>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <vector>

#include <opencv2/core/utils/logger.hpp>
#include <opencv2/imgproc.hpp>

namespace {

// -----------------------------------------------------------------------------
//  试探摄像头时临时关掉 OpenCV 自己的日志
//
//  找摄像头这件事 OpenCV 没有任何枚举 API，只能"挨个后端试、试到能开为止"，
//  而每一条走不通的路它都会往 stderr 打一行：
//
//      VIDEOIO(DSHOW): backend is generally available but can't be used to capture by index
//      VIDEOIO/FFMPEG: OpenCV should be configured with libavdevice to open a camera device
//      obsensor_uvc_stream_channel.cpp: Camera index out of range
//
//  这些是**我们主动试探**造成的噪音，不是故障，更和后面的图像处理毫无关系
//  （cv::Mat / imgproc 那一套从来不打日志）。所以试探期间把日志压到静默，
//  试完立刻恢复，真出问题时该有的日志一条不少。
//
//  想看到原始输出：设环境变量 OPENCV_LOG_LEVEL（比如 INFO / DEBUG），
//  这时本类自动让路，不再压制。
// -----------------------------------------------------------------------------
class ScopedSilenceOpenCvLog
{
public:
    ScopedSilenceOpenCvLog()
    {
        if (qgetenv("OPENCV_LOG_LEVEL").isEmpty()) {
            m_previous = cv::utils::logging::getLogLevel();
            cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);
            m_active = true;
        }
    }

    ~ScopedSilenceOpenCvLog()
    {
        if (m_active)
            cv::utils::logging::setLogLevel(m_previous);
    }

    ScopedSilenceOpenCvLog(const ScopedSilenceOpenCvLog &) = delete;
    ScopedSilenceOpenCvLog &operator=(const ScopedSilenceOpenCvLog &) = delete;

private:
    bool                          m_active = false;
    cv::utils::logging::LogLevel  m_previous = cv::utils::logging::LOG_LEVEL_WARNING;
};

}   // namespace

// 把 CAP_PROP_FOURCC 的整数解成 "MJPG" / "YUY2" 这样的四字符码。
// 显示出来是有意义的：万一 MJPG 协商失败退回 YUY2，帧率会直接掉一半以上，
// 而 `open()` 依然返回成功 —— 不打出来根本看不出来。
QString CaptureWorker::fourccString(const cv::VideoCapture &cap)
{
    const int fcc = static_cast<int>(cap.get(cv::CAP_PROP_FOURCC));
    if (fcc <= 0)
        return QStringLiteral("?");

    QString s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((fcc >> (8 * i)) & 0xFF);
        s.append((c >= 32 && c < 127) ? QLatin1Char(c) : QLatin1Char('?'));
    }
    return s;
}

// -----------------------------------------------------------------------------
//  构造 / 析构
// -----------------------------------------------------------------------------

CaptureWorker::CaptureWorker(QObject *parent)
    : QObject(parent)
{
    // 定时器必须在这里创建、并让它成为自己的子对象：
    // moveToThread() 会把对象和它的所有子对象一起搬过去，
    // 否则 timer 会留在界面线程，超时回调就跑错线程了。
    m_timer = new QTimer(this);
    m_timer->setTimerType(Qt::PreciseTimer);      // 预览这种场合，精度比省电重要
    connect(m_timer, &QTimer::timeout, this, &CaptureWorker::grabOneFrame);
}

CaptureWorker::~CaptureWorker()
{
    // 正常情况下调用方已经 quit() + wait() 过，线程早停了；
    // 这里再收一次尾，保证摄像头句柄一定被还回去。
    stopCaptureInternal();
}

// -----------------------------------------------------------------------------
//  工具
// -----------------------------------------------------------------------------

int CaptureWorker::backendFlag(Backend backend)
{
    switch (backend) {
    case Backend::MSMF:  return cv::CAP_MSMF;
    case Backend::DSHOW: return cv::CAP_DSHOW;
    case Backend::Auto:
    default:             return cv::CAP_ANY;
    }
}

bool CaptureWorker::openCapture(cv::VideoCapture &cap, int index, Backend backend,
                                int width, int height, QString *usedBackend,
                                double *openMs)
{
    // 下面是一串"故意试错"的打开动作，先把 OpenCV 的日志压掉（详见文件头的说明）
    ScopedSilenceOpenCvLog silence;

    QElapsedTimer timer;
    timer.start();

    // 回退顺序：先试**最快**的，不是"最新"的。
    //
    // 实测（本机 Integrated Webcam，请求 1280x720；别的机器可能不一样）：
    //     DSHOW   0.3 ~ 0.7 s     ← 先试它
    //     MSMF    3 ~ 13 s        ← 之前默认先试它，这就是"打开摄像头很慢"的元凶
    //     ANY     2.8 ~ 9 s       ← 它的优先级列表里也是 MSMF 先中，所以一样慢
    //
    // 另外，请求具体分辨率会让 MSMF 明显更慢（13.3s vs 不指定时的 4.1s），
    // 因为它要重新协商 media type；DSHOW 则基本不受影响。
    //
    // 设备不存在时三条路都只要 ~0.2 s，所以"多留几条回退路"本身并不贵。
    QList<Backend> order;
    if (backend != Backend::Auto)
        order << backend;                       // 用户明确指定了就先用它
    order << Backend::DSHOW << Backend::MSMF << Backend::Auto;

    // ⚠ 分辨率必须在 open 的时候就谈好。
    //   千万不要在 open 成功以后再 set(CAP_PROP_FRAME_WIDTH/HEIGHT)：
    //   OpenCV 5 + MSMF 下这会打印 "initStream Failed to select stream 0"，
    //   流直接死掉，之后的 read() 再也拿不到帧（而且 set 的返回值只是 false，
    //   看起来人畜无害，非常难查 —— 实测就是这样）。
    //   open(index, apiPreference, params) 两个后端都能正确协商。
    QSet<int> tried;
    for (Backend b : order) {
        const int flag = backendFlag(b);
        if (tried.contains(flag))
            continue;
        tried.insert(flag);

        // 每个后端按优先级试几组参数，失败就换下一组，最后一组是"驱动默认"
        QList<std::vector<int>> paramSets;

        // 关键的一条：DSHOW 在 1280x720 下默认协商成 YUY2（无压缩），
        // 而这个摄像头 YUY2@720p 只能跑 ~10fps；换成 MJPG 才有 ~30fps，
        // 而且打开还更快（0.25s vs 0.6s）。所以 DSHOW 要显式点名 MJPG。
        // MSMF 则相反：它自己会协商 MJPG，参数里带上 FOURCC 反而会失败
        //（"can't set property 6"），所以不给它加。
        if (b == Backend::DSHOW && width > 0 && height > 0) {
            const int mjpg = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
            paramSets << std::vector<int>{ cv::CAP_PROP_FOURCC, mjpg,
                                           cv::CAP_PROP_FRAME_WIDTH, width,
                                           cv::CAP_PROP_FRAME_HEIGHT, height };
        }
        if (width > 0 && height > 0) {
            paramSets << std::vector<int>{ cv::CAP_PROP_FRAME_WIDTH, width,
                                           cv::CAP_PROP_FRAME_HEIGHT, height };
        }
        paramSets << std::vector<int>{};        // 退回"用驱动默认"

        for (const std::vector<int> &params : paramSets) {
            // 不能拿临时对象赋值回来：cv::VideoCapture 没有移动赋值，
            // 隐式拷贝会让两个对象共用同一个底层句柄，析构时互相 release。
            // 所以直接在调用者的 cap 上 open / release。
            bool ok = false;
            try {
                ok = params.empty() ? cap.open(index, flag)
                                    : cap.open(index, flag, params);
            } catch (const cv::Exception &) {
                ok = false;                     // 有些后端不认 params 形式
            }
            if (!ok) {
                cap.release();
                continue;
            }

            // 能开还不够，得真的读出一帧才算数（有些驱动 open 成功但永远没帧）
            cv::Mat first;
            if (!cap.read(first) || first.empty()) {
                cap.release();
                continue;
            }

            if (usedBackend) {
                QString name;
                try {
                    name = QString::fromStdString(cap.getBackendName());
                } catch (...) {
                    name = QStringLiteral("未知");
                }
                *usedBackend = name;
            }
            if (openMs)
                *openMs = timer.elapsed();      // 含校验用的第一帧，就是用户实际等的时间
            return true;
        }
    }

    if (openMs)
        *openMs = timer.elapsed();
    return false;
}

QImage CaptureWorker::matToQImage(const cv::Mat &mat)
{
    if (mat.empty())
        return QImage();

    if (mat.depth() != CV_8U) {
        cv::Mat tmp;
        mat.convertTo(tmp, CV_8U);
        return matToQImage(tmp);
    }

    switch (mat.channels()) {
    case 1:
        return QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_Grayscale8).copy();
    case 3:
        // OpenCV 是 BGR，Qt6 正好有 Format_BGR888，不用再 cvtColor 一次
        return QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_BGR888).copy();
    case 4:
        return QImage(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step),
                      QImage::Format_ARGB32).copy();
    default:
        return QImage();
    }
}

void CaptureWorker::emitStats(double fps, const cv::Size &size, double processMs)
{
    emit statsReady(QStringLiteral("FPS %1  |  分辨率 %2x%3  |  处理耗时 %4 ms")
                        .arg(fps, 0, 'f', 1)
                        .arg(size.width)
                        .arg(size.height)
                        .arg(processMs, 0, 'f', 2));
}

// -----------------------------------------------------------------------------
//  采集
// -----------------------------------------------------------------------------

void CaptureWorker::startCapture(int deviceIndex, int backend, int width, int height,
                                 int targetFps)
{
    // 同一个线程里先收上一次，安全且必要
    stopCaptureInternal();

    QString backendName;
    double  openMs = 0.0;
    if (!openCapture(m_cap, deviceIndex, static_cast<Backend>(backend),
                     width, height, &backendName, &openMs)) {
        emit openFailed(QStringLiteral(
            "打不开设备 %1（DSHOW / MSMF 都试过了，耗时 %2 s）。\n"
            "检查：① 设备号是否正确，可以点「探测设备」；"
            "② 摄像头是否被其它程序占用（腾讯会议 / 相机 / 之前的进程）；"
            "③ 程序所在目录是否带「低完整性」标签，那会让 Windows 禁止访问摄像头。")
                            .arg(deviceIndex)
                            .arg(openMs / 1000.0, 0, 'f', 1));
        return;
    }

    m_targetFps  = (targetFps < 1) ? 1 : ((targetFps > 120) ? 120 : targetFps);
    m_failCount  = 0;
    m_frameCount = 0;
    m_firstFrame = true;
    m_capturing  = true;
    m_statsTimer.start();

    emit opened(QStringLiteral("设备 %1 已打开  |  后端 %2  |  %3  |  分辨率 %4x%5  |  打开耗时 %6 s")
                    .arg(deviceIndex)
                    .arg(backendName.isEmpty() ? QStringLiteral("未知") : backendName)
                    .arg(fourccString(m_cap))
                    .arg(static_cast<int>(m_cap.get(cv::CAP_PROP_FRAME_WIDTH)))
                    .arg(static_cast<int>(m_cap.get(cv::CAP_PROP_FRAME_HEIGHT)))
                    .arg(openMs / 1000.0, 0, 'f', 2));

    // 帧率交给事件循环：QTimer 在处理完上一次 timeout 之前不会再排新的，
    // 所以处理慢了会自然降帧，不会堆积 —— 这是定时器方案的一个额外好处
    m_timer->start(1000 / m_targetFps);
}

void CaptureWorker::stopCaptureInternal()
{
    if (m_timer)
        m_timer->stop();
    m_capturing = false;

    if (m_cap.isOpened()) {
        // 实测 MSMF / DSHOW 都要 ~0.3s，所以调用方必须是非阻塞的：
        // 界面线程只发请求，绝不 wait() 在这里
        m_cap.release();
    }
}

void CaptureWorker::stopCapture()
{
    stopCaptureInternal();
    emit stopped();
}

void CaptureWorker::shutdown()
{
    m_probing = false;          // 探测链看到这个标志会自己停
    stopCaptureInternal();
    emit shutdownDone();
}

void CaptureWorker::setParams(ProcessParams params)
{
    m_params = params;          // 队列投递，天然和 grabOneFrame 串行，不用锁
}

void CaptureWorker::grabOneFrame()
{
    if (!m_cap.isOpened())
        return;

    if (!m_cap.read(m_frame) || m_frame.empty()) {
        if (++m_failCount > 30) {
            m_timer->stop();
            m_capturing = false;
            emit openFailed(QStringLiteral("连续 30 次读不到帧，采集已停止。"));
        }
        return;
    }
    m_failCount = 0;

    m_processed = processFrame(m_frame, m_params, &m_lastProcMs);
    if (m_processed.empty())
        m_processed = m_frame;

    emit framesReady(matToQImage(m_frame), matToQImage(m_processed));

    if (m_firstFrame) {
        m_firstFrame = false;
        emitStats(0.0, m_frame.size(), m_lastProcMs);
    }

    ++m_frameCount;
    if (m_statsTimer.elapsed() >= 500) {
        const double secs = m_statsTimer.elapsed() / 1000.0;
        emitStats(secs > 0.0 ? m_frameCount / secs : 0.0, m_frame.size(), m_lastProcMs);
        m_statsTimer.restart();
        m_frameCount = 0;
    }
}

// -----------------------------------------------------------------------------
//  设备探测：没有枚举摄像头的 API，只能按序号硬试
//
//  这里特意做成"排一次队列 → 探一个序号 → 再排下一次"的链式结构：
//  每探一个就把控制权还给事件循环，队列里的 stopProbe() / shutdown() 才插得进来。
//  否则一次探测要连着卡好几秒，关窗口得干等它跑完。
// -----------------------------------------------------------------------------

void CaptureWorker::startProbe(int maxIndex)
{
    // 探测要用摄像头，所以先把采集停掉。
    // 如果本来在采集，得告诉界面一声 —— 否则界面还以为在采集中，
    // 按钮状态就乱了。
    const bool wasCapturing = m_capturing || m_cap.isOpened();
    stopCaptureInternal();
    if (wasCapturing)
        emit stopped();

    m_probeIndex     = 0;
    m_probeMax       = maxIndex;
    m_probeAvailable = 0;
    m_probing        = true;

    QMetaObject::invokeMethod(this, &CaptureWorker::probeNext, Qt::QueuedConnection);
}

void CaptureWorker::stopProbe()
{
    m_probing = false;
}

void CaptureWorker::probeNext()
{
    if (!m_probing)
        return;

    if (m_probeIndex > m_probeMax) {
        m_probing = false;
        emit probeFinished(m_probeAvailable);
        return;
    }

    // 探测是噪音最大的地方（序号 × 后端，几乎全是失败尝试），先静音
    ScopedSilenceOpenCvLog silence;

    const int index = m_probeIndex;
    bool      found = false;

    for (Backend b : { Backend::MSMF, Backend::DSHOW, Backend::Auto }) {
        cv::VideoCapture cap(index, backendFlag(b));
        if (!cap.isOpened())
            continue;

        cv::Mat probe;
        const bool gotFrame = cap.read(probe) && !probe.empty();
        const int  w = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        const int  h = static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

        QString name;
        try {
            name = QString::fromStdString(cap.getBackendName());
        } catch (...) {
            name = QStringLiteral("未知");
        }
        cap.release();

        if (gotFrame) {
            emit probeResult(index, name, w, h, true);
            ++m_probeAvailable;
            found = true;
            break;
        }
    }

    if (!found)
        emit probeResult(index, QString(), 0, 0, false);

    ++m_probeIndex;
    QMetaObject::invokeMethod(this, &CaptureWorker::probeNext, Qt::QueuedConnection);
}
