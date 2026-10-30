// =============================================================================
//  cli.cpp —— 命令行诊断工具的实现
//
//  抽出来的原因：这些都是"没有界面也能跑"的逻辑，
//  和窗口 / 界面代码零耦合，放在一起只会让主文件越来越长。
// =============================================================================

#include "cli.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QThread>          // grab() 里等自动曝光稳定时用 QThread::msleep

#include <QCameraDevice>
#include <QMediaDevices>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/videoio/registry.hpp>

#include "processing.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace cli {

    // -----------------------------------------------------------------------------
    //  把 stdout 接回父进程的控制台
    //      qmake 给 GUI 程序加的是 /SUBSYSTEM:WINDOWS，本身没有控制台，
    //      所以 --list / --selftest 之类想打印东西时必须自己接一下；
    //      接不上（比如被 IDE 或脚本用管道捕获）就自己开一个。
    // -----------------------------------------------------------------------------
    void ensureConsole()
    {
    #if defined(_WIN32)
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            AllocConsole();
    #  if defined(_MSC_VER)
        FILE *fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
    #  else
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    #  endif
    #endif
    }

    // 命令行输出顺便落一份文件：
    // 控制台万一没接上，用户在 exe 目录下也能找到一模一样的内容。
    static void saveCliText(const QString &fileName, const QString &text)
    {
        QFile file(QDir(QCoreApplication::applicationDirPath()).filePath(fileName));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << text << Qt::endl;
        }
    }
    // =============================================================================
    //  命令行工具：--selftest（合成图）/ --grab（真摄像头）
    // =============================================================================

    // 平均差异：三个通道均值之和，用来判断“画面到底有没有变”
    static double meanDiffTo(const cv::Mat &a, const cv::Mat &b)
    {
        cv::Mat diff;
        cv::absdiff(a, b, diff);
        const cv::Scalar d = cv::mean(diff);
        return d[0] + d[1] + d[2];
    }

    // 每个模式额外的不变量检查：不光看“变没变”，还要看“变得对不对”
    static QString invariantError(const cv::Mat &input, const ProcessModeInfo &info,
                                  const cv::Mat &out)
    {
        switch (info.mode) {
        case ProcessMode::Grayscale:
        case ProcessMode::EqualizeHist:
        case ProcessMode::Sketch: {
            cv::Mat ch[3];
            cv::split(out, ch);
            cv::Mat d1, d2;
            cv::absdiff(ch[0], ch[1], d1);
            cv::absdiff(ch[1], ch[2], d2);
            if (cv::countNonZero(d1.reshape(1)) || cv::countNonZero(d2.reshape(1)))
                return QStringLiteral("灰度模式却输出了彩色像素");
            return {};
        }
        case ProcessMode::Canny:
        case ProcessMode::Threshold:
        case ProcessMode::AdaptiveThreshold: {
            cv::Mat gray;
            cv::cvtColor(out, gray, cv::COLOR_BGR2GRAY);
            cv::Mat notZero, notFull, bad;
            cv::compare(gray, 0, notZero, cv::CMP_NE);
            cv::compare(gray, 255, notFull, cv::CMP_NE);
            cv::bitwise_and(notZero, notFull, bad);
            if (cv::countNonZero(bad))
                return QStringLiteral("二值化结果出现 0 / 255 以外的灰度");
            return {};
        }
        case ProcessMode::Inverse: {
            cv::Mat expect, d;
            cv::bitwise_not(input, expect);
            cv::absdiff(out, expect, d);
            if (cv::countNonZero(d.reshape(1)))
                return QStringLiteral("反色结果不等于 255 - 输入");
            return {};
        }
        default:
            return {};
        }
    }

    // 把一张图跑一遍所有处理模式：校验 + 存盘。返回失败个数。
    static int runModesOverImage(const cv::Mat &input, const QString &outDir,
                                 const QString &prefix, QStringList &log)
    {
        auto add = [&log](const QString &line) {
            log << line;
            QTextStream(stdout) << line << Qt::endl;
        };

        const ProcessModeInfo *table = processModeTable();
        const int              count = processModeCount();
        int                    failures = 0;

        for (int i = 0; i < count; ++i) {
            const ProcessModeInfo &info = table[i];
            const QString          name = QString::fromUtf8(info.name);

            ProcessParams params;
            params.mode   = info.mode;
            params.p1     = info.p1.label ? info.p1.def : 0.0;
            params.p2     = info.p2.label ? info.p2.def : (info.p2.def > 0.0 ? info.p2.def : 1.0);
            params.mirror = false;                     // 自检不镜像，方便和输入比对

            double  ms  = 0.0;
            cv::Mat out = processFrame(input, params, &ms);

            // (1) 输出必须合法
            if (out.empty() || out.type() != CV_8UC3 || out.size() != input.size()) {
                ++failures;
                add(QStringLiteral("[FAIL] %1 —— 输出不合法（空 / 尺寸 / 类型不对）").arg(name));
                continue;
            }

            // (2) 默认参数下：该变的必须真的变，声明「默认不变」的必须真的不变
            const double meanDiff = meanDiffTo(input, out);
            const bool   changed  = meanDiff > 0.2;
            if (info.identityByDefault ? changed : !changed) {
                ++failures;
                add(QStringLiteral("[FAIL] %1 —— %2  平均差异=%3")
                        .arg(name)
                        .arg(info.identityByDefault ? QStringLiteral("默认参数下画面本不该变")
                                                    : QStringLiteral("画面没有变化"))
                        .arg(meanDiff, 0, 'f', 3));
                continue;
            }

            // (3) 结构不变量
            const QString why = invariantError(input, info, out);
            if (!why.isEmpty()) {
                ++failures;
                add(QStringLiteral("[FAIL] %1 —— %2").arg(name, why));
                continue;
            }

            // (4) 默认参数是恒等变换的模式（亮度/对比度），把参数推一把再证明它有效
            QString extra;
            if (info.identityByDefault && (info.p1.label || info.p2.label)) {
                ProcessParams pushed = params;
                if (info.p1.label)
                    pushed.p1 = info.p1.min + (info.p1.max - info.p1.min) * 0.75;
                else
                    pushed.p2 = info.p2.min + (info.p2.max - info.p2.min) * 0.75;

                double  ms2  = 0.0;
                cv::Mat out2 = processFrame(input, pushed, &ms2);
                const double d2 = out2.empty() ? 0.0 : meanDiffTo(input, out2);
                if (d2 <= 0.2) {
                    ++failures;
                    add(QStringLiteral("[FAIL] %1 —— 参数推到 75% 位置画面仍然没变（差异=%2）")
                            .arg(name)
                            .arg(d2, 0, 'f', 3));
                    continue;
                }
                extra = QStringLiteral("   参数推到 75% 后差异 %1").arg(d2, 0, 'f', 1);
            }

            const cv::Scalar m = cv::mean(out);
            const QString file = QDir(outDir).filePath(
                QStringLiteral("%1_%2.png").arg(prefix).arg(i, 2, 10, QLatin1Char('0')));
            if (!cv::imwrite(file.toStdString(), out)) {
                ++failures;
                add(QStringLiteral("[FAIL] %1 —— 写 %2 失败").arg(name, file));
                continue;
            }

            add(QStringLiteral("[ OK ] %1  %2 ms   均值(%3,%4,%5)   与输入平均差异 %6%7")
                    .arg(name, -16)
                    .arg(ms, 6, 'f', 2)
                    .arg(m[0], 6, 'f', 1)
                    .arg(m[1], 6, 'f', 1)
                    .arg(m[2], 6, 'f', 1)
                    .arg(meanDiff, 7, 'f', 2)
                    .arg(extra));
        }

        add(QString());
        add(QStringLiteral("合计 %1 个处理模式，失败 %2 个。").arg(count).arg(failures));
        return failures;
    }

    // 把日志同时写进文件
    static void saveLogFile(const QString &dir, const QString &fileName, const QStringList &log)
    {
        QFile file(QDir(dir).filePath(fileName));
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << log.join(QLatin1Char('\n')) << Qt::endl;
        }
    }

    // 打印 OpenCV 认得的采集后端（排查摄像头问题时最有用的一行）
    static QString backendSummary()
    {
        // 注意：OpenCV 5 把后端枚举从 VideoCapture 静态成员搬到了 videoio_registry
        QStringList names;
        for (cv::VideoCaptureAPIs api : cv::videoio_registry::getCameraBackends()) {
            try {
                names << QString::fromStdString(cv::videoio_registry::getBackendName(api));
            } catch (...) {
                names << QStringLiteral("?");
            }
        }
        return names.isEmpty() ? QStringLiteral("(无)") : names.join(QStringLiteral(", "));
    }

    // -----------------------------------------------------------------------------
    //  --selftest：不碰摄像头，用一张合成图验证整条 OpenCV 链路
    // -----------------------------------------------------------------------------
    int selfTest()
    {
        ensureConsole();

        const QString outDir = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("selftest_out"));
        QDir().mkpath(outDir);

        QStringList log;
        auto add = [&log](const QString &line) {
            log << line;
            QTextStream(stdout) << line << Qt::endl;
        };

        add(QStringLiteral("================ camera_qt 自检（不用摄像头）================"));
        add(QStringLiteral("Qt 版本      : %1").arg(QString::fromLatin1(qVersion())));
        add(QStringLiteral("OpenCV 版本  : %1").arg(QString::fromStdString(cv::getVersionString())));
        add(QStringLiteral("采集后端     : %1").arg(backendSummary()));
        add(QStringLiteral("输出目录     : %1").arg(outDir));
        add(QString());

        const cv::Mat test = makeTestFrame(1280, 720);
        if (test.empty()) {
            add(QStringLiteral("[FAIL] 合成测试帧创建失败"));
            return 3;
        }
        add(QStringLiteral("合成测试帧   : %1x%2, %3 通道")
                .arg(test.cols).arg(test.rows).arg(test.channels()));

        const QString testPath = QDir(outDir).filePath(QStringLiteral("mode_input.png"));
        add(QStringLiteral("输入已保存   : %1（%2）")
                .arg(testPath, cv::imwrite(testPath.toStdString(), test) ? QStringLiteral("OK")
                                                                         : QStringLiteral("失败")));
        add(QString());

        const int failures = runModesOverImage(test, outDir, QStringLiteral("mode"), log);

        const QString logPath = QDir(outDir).filePath(QStringLiteral("selftest.log"));
        saveLogFile(outDir, QStringLiteral("selftest.log"), log);
        QTextStream(stdout) << QStringLiteral("日志已写入   : ") << logPath << Qt::endl;

        return failures == 0 ? 0 : 3;
    }

    // -----------------------------------------------------------------------------
    //  --grab [设备号]：真的打开摄像头抓一帧，然后拿这一帧跑一遍所有处理模式
    //      既是端到端验证，也是排查摄像头问题的命令行工具。
    // -----------------------------------------------------------------------------
    int grab(int index, CaptureWorker::Backend backend)
    {
        ensureConsole();

        const QString outDir = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("grab_out"));
        QDir().mkpath(outDir);

        QStringList log;
        auto add = [&log](const QString &line) {
            log << line;
            QTextStream(stdout) << line << Qt::endl;
        };

        add(QStringLiteral("================ camera_qt 抓帧 ================"));
        add(QStringLiteral("OpenCV 版本  : %1").arg(QString::fromStdString(cv::getVersionString())));
        add(QStringLiteral("采集后端     : %1").arg(backendSummary()));
        add(QStringLiteral("输出目录     : %1").arg(outDir));
        add(QString());

        cv::VideoCapture cap;
        QString          backendName;
        double           openMs = 0.0;
        if (!CaptureWorker::openCapture(cap, index, backend,
                                        1280, 720, &backendName, &openMs)) {
            add(QStringLiteral("[FAIL] 打不开设备 %1（DSHOW / MSMF 都试过了，耗时 %2 s）")
                    .arg(index)
                    .arg(openMs / 1000.0, 0, 'f', 1));
            add(QStringLiteral("排查顺序：① 设备号对不对（先跑 --probe）"
                               "② 摄像头是否被别的程序占用"
                               "③ 程序所在目录是否带「低完整性」标签"));
            saveLogFile(outDir, QStringLiteral("grab.log"), log);
            return 4;
        }

        // 分辨率在 openCapture 里就谈好了 —— open 之后再 set 宽高会让 MSMF 的流直接死掉
        add(QStringLiteral("设备 %1 已打开   后端=%2   %3   实际分辨率 %4x%5   打开耗时 %6 s")
                .arg(index)
                .arg(backendName)
                .arg(CaptureWorker::fourccString(cap))
                .arg(static_cast<int>(cap.get(cv::CAP_PROP_FRAME_WIDTH)))
                .arg(static_cast<int>(cap.get(cv::CAP_PROP_FRAME_HEIGHT)))
                .arg(openMs / 1000.0, 0, 'f', 2));

        // 连读若干帧再取最后一帧：让自动曝光 / 自动白平衡先稳定下来
        cv::Mat frame, tmp;
        for (int i = 0; i < 12; ++i) {
            if (cap.read(tmp) && !tmp.empty())
                frame = tmp.clone();
            QThread::msleep(40);
        }
        cap.release();

        if (frame.empty()) {
            add(QStringLiteral("[FAIL] 打开了设备但一帧都读不到（驱动问题或被别的程序抢占）"));
            saveLogFile(outDir, QStringLiteral("grab.log"), log);
            return 5;
        }

        const QString rawPath = QDir(outDir).filePath(QStringLiteral("raw.png"));
        cv::imwrite(rawPath.toStdString(), frame);
        add(QStringLiteral("抓到一帧     : %1x%2, %3 通道 → %4")
                .arg(frame.cols).arg(frame.rows).arg(frame.channels()).arg(rawPath));
        add(QString());

        const int failures = runModesOverImage(frame, outDir, QStringLiteral("grab_mode"), log);

        const QString logPath = QDir(outDir).filePath(QStringLiteral("grab.log"));
        saveLogFile(outDir, QStringLiteral("grab.log"), log);
        QTextStream(stdout) << QStringLiteral("日志已写入   : ") << logPath << Qt::endl;

        return failures == 0 ? 0 : 3;
    }
    // -----------------------------------------------------------------------------
    //  --list：列出 Qt 能看到的摄像头设备
    // -----------------------------------------------------------------------------
    int listDevices()
    {
        ensureConsole();

        const QList<QCameraDevice> devices = QMediaDevices::videoInputs();

        QString text;
        {
            QTextStream out(&text);
            out << QStringLiteral("Qt %1\n").arg(qVersion());
            out << QStringLiteral("OpenCV %1\n").arg(QString::fromStdString(cv::getVersionString()));
            out << QStringLiteral("摄像头数量: %1\n").arg(devices.size());
            for (const QCameraDevice &device : devices)
                out << QStringLiteral("  - %1\n").arg(device.description());
        }

        saveCliText(QStringLiteral("cli_output.txt"), text);
        QTextStream(stdout) << text << Qt::endl;

        return devices.isEmpty() ? 2 : 0;
    }

    // -----------------------------------------------------------------------------
    //  --opencv-info：打印版本，排查「到底链接到哪个 OpenCV」时很有用
    // -----------------------------------------------------------------------------
    int printOpenCvInfo()
    {
        ensureConsole();

        QString text;
        {
            QTextStream out(&text);
            out << "Qt     : " << qVersion() << Qt::endl;
            out << "OpenCV : " << QString::fromStdString(cv::getVersionString()) << Qt::endl;
            out << "OpenCV 构建信息：" << Qt::endl;
            out << QString::fromStdString(cv::getBuildInformation()) << Qt::endl;
        }

        saveCliText(QStringLiteral("cli_output.txt"), text);
        QTextStream(stdout) << text << Qt::endl;

        return 0;
    }

// -----------------------------------------------------------------------------
//  参数解析 + 分发
//
//  放在这里而不是 main.cpp：这几条开关是命令行模式自己的知识。
//  入口只要问一句"命中了吗"，加一个新开关不用动 main.cpp。
// -----------------------------------------------------------------------------

// 读 "--flag <数字>" 形式的值；没给或者不是数字就返回 fallback
static int numberAfter(const QStringList &args, const QString &flag, int fallback)
{
    const int at = args.indexOf(flag);
    if (at < 0 || at + 1 >= args.size())
        return fallback;

    bool      ok = false;
    const int n  = args.at(at + 1).toInt(&ok);
    return ok ? n : fallback;
}

// --grab [序号] 或者 --index N，后者优先
static int grabDeviceIndex(const QStringList &args)
{
    return numberAfter(args, QStringLiteral("--index"),
                       numberAfter(args, QStringLiteral("--grab"), 0));
}

// --backend msmf|dshow|auto：用来对比不同后端在这台机器上的打开速度
static CaptureWorker::Backend grabBackend(const QStringList &args)
{
    const int at = args.indexOf(QStringLiteral("--backend"));
    if (at < 0 || at + 1 >= args.size())
        return CaptureWorker::Backend::Auto;

    const QString want = args.at(at + 1).toLower();
    if (want == QStringLiteral("msmf"))
        return CaptureWorker::Backend::MSMF;
    if (want == QStringLiteral("dshow"))
        return CaptureWorker::Backend::DSHOW;
    return CaptureWorker::Backend::Auto;
}

std::optional<int> runFromArgs(const QStringList &args)
{
    if (args.contains(QStringLiteral("--opencv-info")))
        return printOpenCvInfo();

    if (args.contains(QStringLiteral("--selftest")))
        return selfTest();

    if (args.contains(QStringLiteral("--grab")))
        return grab(grabDeviceIndex(args), grabBackend(args));

    if (args.contains(QStringLiteral("--list")))
        return listDevices();

    return std::nullopt;        // 没有命令行开关 -> 让调用方去开界面
}

}   // namespace cli
