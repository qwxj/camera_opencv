// =============================================================================
//  processing.h —— 用 OpenCV 对摄像头帧做处理
//
//  这一层故意不依赖 Qt：输入一张 BGR 的 cv::Mat，输出一张 BGR 的 cv::Mat，
//  方便单独测试、也方便以后搬到别的界面框架里。
//
//  模式表 kModes 是「数据驱动」的：
//      界面上的下拉框、两个参数滑块的取值范围和名称，全部从这张表生成，
//      加一种处理只要在 enum 和表里各加一行，界面自动就有了。
// =============================================================================

#ifndef PROCESSING_H
#define PROCESSING_H

#include <opencv2/core.hpp>

// -----------------------------------------------------------------------------
//  处理模式
// -----------------------------------------------------------------------------
enum class ProcessMode {
    Original = 0,        // 原图
    Grayscale,           // 灰度
    GaussianBlur,        // 高斯模糊
    MedianBlur,          // 中值滤波（去椒盐噪声）
    BilateralFilter,     // 双边滤波（磨皮保边）
    Canny,               // Canny 边缘检测
    Threshold,           // 固定阈值二值化
    AdaptiveThreshold,   // 自适应阈值二值化
    Inverse,             // 反色（负片）
    EqualizeHist,        // 直方图均衡（提对比度）
    BrightContrast,      // 亮度 / 对比度
    Erode,               // 腐蚀
    Dilate,              // 膨胀
    Sharpen,             // 锐化
    Sketch,              // 铅笔素描
    PseudoColor,         // 伪彩色（色图映射）
    Count                // 哨兵：模式总数
};

// -----------------------------------------------------------------------------
//  一个参数滑块的描述
//      label 为空 → 这个模式不用该参数
// -----------------------------------------------------------------------------
struct ParamSpec {
    const char *label;   // 中文显示名（UTF-8）
    double      min;     // 最小值
    double      max;     // 最大值
    double      def;     // 默认值
};

// -----------------------------------------------------------------------------
//  一个处理模式的描述
// -----------------------------------------------------------------------------
struct ProcessModeInfo {
    ProcessMode mode;
    const char *name;               // 下拉框里显示的名字（UTF-8）
    bool        identityByDefault;  // 用默认参数时画面不变（原图、亮度对比度默认值）
    ParamSpec   p1;                 // 参数 1
    ParamSpec   p2;                 // 参数 2
    const char *hint;               // 一行说明
};

// 模式表（下标 == int(ProcessMode)，最后一项是 Count 哨兵）
const ProcessModeInfo *processModeTable();
int                    processModeCount();

// -----------------------------------------------------------------------------
//  当前选中的处理参数
// -----------------------------------------------------------------------------
struct ProcessParams {
    ProcessMode mode = ProcessMode::Original;
    double      p1   = 0.0;
    double      p2   = 0.0;

    // 公共开关
    bool mirror = true;   // 左右镜像（自拍习惯）
};

// -----------------------------------------------------------------------------
//  处理一帧
//      bgrIn      : 输入，8UC3 / 8UC1 都行
//      params     : 模式 + 两个参数
//      elapsedMs  : 可选，回传这一帧的处理耗时（毫秒）
//      返回       : 8UC3 BGR 图像；输入为空时返回空 Mat
// -----------------------------------------------------------------------------
cv::Mat processFrame(const cv::Mat &bgrIn, const ProcessParams &params,
                     double *elapsedMs = nullptr);

// -----------------------------------------------------------------------------
//  自检用：造一张不依赖摄像头的合成图
// -----------------------------------------------------------------------------
cv::Mat makeTestFrame(int width = 1280, int height = 720);

#endif // PROCESSING_H
