// =============================================================================
//  cli.h —— 命令行诊断工具（全都不需要打开界面）
//
//      --list           列出 Qt 能看到的摄像头设备
//      --opencv-info    打印 Qt / OpenCV 版本与 OpenCV 构建信息
//      --selftest       用合成图跑一遍全部处理模式并出图
//      --grab [序号]    真抓一帧摄像头画面，再跑一遍全部处理模式并出图
//          --index N    指定设备序号
//          --backend X  指定后端（auto / dshow / msmf）
//
//  **参数怎么解析也在这个模块里** —— "有哪些开关、长什么样"是这条命令自己的知识，
//  入口不需要知道 --grab 还能带序号、--backend 有哪几个取值。
// =============================================================================

#ifndef CLI_H
#define CLI_H

#include <QStringList>
#include <optional>

#include "captureworker.h"      // CaptureWorker::Backend

namespace cli {

// 命令行入口：按 args 里的开关执行对应功能。
//      命中了  -> 返回它的退出码
//      没命中  -> 返回 std::nullopt，表示"没有命令行开关，该开界面了"
//
// 退出码约定：
//      0  成功
//      2  没找到摄像头（--list）
//      3  有处理模式自检失败（--selftest / --grab）
//      4  打不开摄像头（--grab）
//      5  打开了但读不到帧（--grab）
std::optional<int> runFromArgs(const QStringList &args);

// 下面是具体功能。一般不用直接调（runFromArgs 会分发过来），
// 保持公开是为了能被单独调用 / 测试。
void ensureConsole();
int listDevices();
int printOpenCvInfo();
int selfTest();
int grab(int deviceIndex, CaptureWorker::Backend backend);

}   // namespace cli

#endif // CLI_H
