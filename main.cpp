// =============================================================================
//  main.cpp —— 程序入口
//
//  只做三件事，没有任何功能实现，也没有任何自定义函数：
//      1) 建 QApplication，设好全局开关（日志过滤、跨线程元类型注册）
//      2) 把命令行交给该管的人
//      3) 进事件循环
//
//  参数解析也不在这里 —— "有哪些开关、长什么样"是各自模块自己的知识：
//      --list / --opencv-info / --selftest / --grab   -> cli::runFromArgs()
//      --autostart / --mode                           -> CameraWindow::applyStartupOptions()
//  所以加一个新开关只要改对应模块，不用动入口。
// =============================================================================

#include <QApplication>
#include <QLoggingCategory>

#include <optional>

#include "camerawindow.h"
#include "cli.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("camera_qt"));
    app.setApplicationVersion(QStringLiteral("2.0"));

    // Qt 多媒体插件每次启动都会用 info 级打一行 ffmpeg 版本号，控制台里纯噪音。
    // 只关这一条 info，warning / error 仍然照常显示。
    QLoggingCategory::setFilterRules(QStringLiteral("qt.multimedia.ffmpeg.info=false"));

    // 处理参数要跨线程排队传给采集线程，自定义类型得先注册
    qRegisterMetaType<ProcessParams>("ProcessParams");

    const QStringList args = app.arguments();

    // 命中了命令行模式就执行完直接退出；没命中说明该开界面了
    if (const std::optional<int> exitCode = cli::runFromArgs(args))
        return *exitCode;

    CameraWindow window;
    window.show();
    window.applyStartupOptions(args);

    return app.exec();
}
