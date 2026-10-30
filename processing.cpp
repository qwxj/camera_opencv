// =============================================================================
//  processing.cpp —— OpenCV 图像处理的实现
//
//  只用 core / imgproc 两个模块 + imwrite，全部是 OpenCV 里最稳定的 API，
//  在 OpenCV 4.x 和 5.x 上表现一致。
//
//  约定：processFrame() 永远吐一张 8UC3 的 BGR 图。
//        单通道算法（Canny / 阈值）在内部转成 BGR 再返回，
//        这样上层的显示和保存代码不用分支。
// =============================================================================

#include "processing.h"

#include <opencv2/imgproc.hpp>

#include <algorithm>

// -----------------------------------------------------------------------------
//  模式表
// -----------------------------------------------------------------------------
static const ProcessModeInfo kModes[] = {
    { ProcessMode::Original, "原图（不做处理）", true,
      { nullptr, 0, 0, 0 }, { nullptr, 0, 0, 0 },
      "直接把摄像头这一帧送显示，用来和后面几种效果做对比" },

    { ProcessMode::Grayscale, "灰度", false,
      { nullptr, 0, 0, 0 }, { nullptr, 0, 0, 0 },
      "cvtColor(BGR2GRAY) —— 去色，后面的边缘/阈值都先走这一步" },

    { ProcessMode::GaussianBlur, "高斯模糊", false,
      { "核大小", 1, 51, 7 }, { nullptr, 0, 0, 0 },
      "GaussianBlur —— 平滑去噪，核越大越糊（自动取奇数）" },

    { ProcessMode::MedianBlur, "中值滤波", false,
      { "核大小", 3, 51, 7 }, { nullptr, 0, 0, 0 },
      "medianBlur —— 对椒盐噪声特别有效，也比高斯更保边" },

    { ProcessMode::BilateralFilter, "双边滤波（磨皮）", false,
      { "邻域直径", 1, 25, 9 }, { "颜色sigma", 5, 200, 75 },
      "bilateralFilter —— 磨平皮肤纹理同时保住轮廓，代价是慢" },

    { ProcessMode::Canny, "Canny 边缘检测", false,
      { "低阈值", 0, 400, 80 }, { "高阈值", 0, 800, 180 },
      "Canny —— 只留下轮廓线，两个阈值控制哪些边缘算“真边缘”" },

    { ProcessMode::Threshold, "阈值二值化", false,
      { "阈值", 0, 255, 128 }, { nullptr, 0, 0, 0 },
      "threshold(THRESH_BINARY) —— 比阈值亮的变白，其余变黑" },

    { ProcessMode::AdaptiveThreshold, "自适应阈值", false,
      { "邻域大小", 3, 99, 21 }, { "常数 C", -20, 20, 5 },
      "adaptiveThreshold —— 每个像素跟自己的邻域比，光照不均时比固定阈值好" },

    { ProcessMode::Inverse, "反色（负片）", false,
      { nullptr, 0, 0, 0 }, { nullptr, 0, 0, 0 },
      "bitwise_not —— 255 减去每个通道" },

    { ProcessMode::EqualizeHist, "直方图均衡", false,
      { nullptr, 0, 0, 0 }, { nullptr, 0, 0, 0 },
      "equalizeHist —— 把灰阶拉开，暗画面一下子亮起来" },

    { ProcessMode::BrightContrast, "亮度 / 对比度", true,
      { "亮度", -150, 150, 0 }, { "对比度(%)", 0, 300, 100 },
      "convertTo —— 线性变换 out = (in-128)*对比度/100 + 128 + 亮度；默认值等于不变" },

    { ProcessMode::Erode, "腐蚀", false,
      { "核大小", 1, 31, 5 }, { "迭代次数", 1, 10, 1 },
      "erode —— 亮区变瘦，用来断开细连接、去小亮点" },

    { ProcessMode::Dilate, "膨胀", false,
      { "核大小", 1, 31, 5 }, { "迭代次数", 1, 10, 1 },
      "dilate —— 亮区变胖，用来填补小孔、连接断裂" },

    { ProcessMode::Sharpen, "锐化", false,
      { "强度", 0, 100, 40 }, { nullptr, 0, 0, 0 },
      "filter2D —— 3x3 拉普拉斯锐化核，强度越高边缘越硬" },

    { ProcessMode::Sketch, "铅笔素描", false,
      { "模糊核", 1, 51, 21 }, { nullptr, 0, 0, 0 },
      "灰度反相 + 高斯模糊 + divide —— 经典的“颜色减淡”素描效果" },

    { ProcessMode::PseudoColor, "伪彩色", false,
      { "色图编号", 0, 21, 2 }, { nullptr, 0, 0, 0 },
      "applyColorMap —— 先把灰度映射成颜色，0=Autumn 2=Jet 等" },

    // 哨兵：只用于表长校验
    { ProcessMode::Count, "", true,
      { nullptr, 0, 0, 0 }, { nullptr, 0, 0, 0 }, "" },
};

const ProcessModeInfo *processModeTable()
{
    return kModes;
}

int processModeCount()
{
    return static_cast<int>(sizeof(kModes) / sizeof(kModes[0])) - 1;   // 去掉哨兵
}

// -----------------------------------------------------------------------------
//  小工具
// -----------------------------------------------------------------------------

// 把任意值夹到 [lo, hi] 的奇数上（OpenCV 的核大小 / 邻域大小都要求奇数）
static int oddClamp(double v, int lo, int hi)
{
    int n = static_cast<int>(v + 0.5);
    n = std::max(lo, std::min(hi, n));
    if (n % 2 == 0)
        ++n;
    return std::min(hi, std::max(lo, n));
}

// 保证输入是 8UC3
static cv::Mat toBgr8u(const cv::Mat &in)
{
    if (in.empty())
        return {};

    cv::Mat tmp;
    if (in.depth() != CV_8U) {
        in.convertTo(tmp, CV_8U);
    } else {
        tmp = in;
    }

    if (tmp.channels() == 1) {
        cv::Mat bgr;
        cv::cvtColor(tmp, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }
    if (tmp.channels() == 4) {
        cv::Mat bgr;
        cv::cvtColor(tmp, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    }
    return tmp;
}

// -----------------------------------------------------------------------------
//  处理一帧
// -----------------------------------------------------------------------------
cv::Mat processFrame(const cv::Mat &bgrIn, const ProcessParams &params,
                     double *elapsedMs)
{
    if (bgrIn.empty())
        return {};

    const int64 t0 = cv::getTickCount();

    cv::Mat src = toBgr8u(bgrIn);

    // 镜像（自拍时左右不反看着别扭）
    if (params.mirror) {
        cv::Mat flipped;
        cv::flip(src, flipped, 1);
        src = flipped;
    }

    cv::Mat out;

    switch (params.mode) {

    case ProcessMode::Original:
        out = src.clone();
        break;

    case ProcessMode::Grayscale: {
        cv::Mat gray;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);
        break;
    }

    case ProcessMode::GaussianBlur: {
        const int k = oddClamp(params.p1, 1, 51);
        cv::GaussianBlur(src, out, cv::Size(k, k), 0, 0);
        break;
    }

    case ProcessMode::MedianBlur: {
        const int k = oddClamp(params.p1, 3, 51);
        cv::medianBlur(src, out, k);
        break;
    }

    case ProcessMode::BilateralFilter: {
        const int    d  = std::max(1, static_cast<int>(params.p1 + 0.5));
        const double sc = std::max(1.0, params.p2);
        cv::bilateralFilter(src, out, d, sc, sc);
        break;
    }

    case ProcessMode::Canny: {
        cv::Mat gray;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        cv::GaussianBlur(gray, gray, cv::Size(3, 3), 0, 0);   // 先轻模糊，边缘更干净

        double lo = std::max(0.0, params.p1);
        double hi = std::max(0.0, params.p2);
        if (hi < lo)                    // 高阈值必须不小于低阈值，否则交换
            std::swap(lo, hi);
        if (hi < 1.0)
            hi = 1.0;

        cv::Mat edges;
        cv::Canny(gray, edges, lo, hi);
        cv::cvtColor(edges, out, cv::COLOR_GRAY2BGR);
        break;
    }

    case ProcessMode::Threshold: {
        cv::Mat gray;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        const double th = std::max(0.0, std::min(255.0, params.p1));
        cv::Mat bin;
        cv::threshold(gray, bin, th, 255, cv::THRESH_BINARY);
        cv::cvtColor(bin, out, cv::COLOR_GRAY2BGR);
        break;
    }

    case ProcessMode::AdaptiveThreshold: {
        cv::Mat gray;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        const int block = oddClamp(params.p1, 3, 99);
        const double c  = params.p2;
        cv::Mat bin;
        cv::adaptiveThreshold(gray, bin, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                              cv::THRESH_BINARY, block, c);
        cv::cvtColor(bin, out, cv::COLOR_GRAY2BGR);
        break;
    }

    case ProcessMode::Inverse: {
        cv::bitwise_not(src, out);
        break;
    }

    case ProcessMode::EqualizeHist: {
        cv::Mat gray;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        cv::Mat eq;
        cv::equalizeHist(gray, eq);
        cv::cvtColor(eq, out, cv::COLOR_GRAY2BGR);
        break;
    }

    case ProcessMode::BrightContrast: {
        const double brightness = params.p1;          // -150 .. 150
        const double contrast   = params.p2 / 100.0;  // 0 .. 3
        // out = (in - 128) * contrast + 128 + brightness
        const double alpha = contrast;
        const double beta  = 128.0 * (1.0 - contrast) + brightness;
        src.convertTo(out, -1, alpha, beta);
        break;
    }

    case ProcessMode::Erode:
    case ProcessMode::Dilate: {
        const int    k    = oddClamp(params.p1, 1, 31);
        const int    iter = std::max(1, std::min(10, static_cast<int>(params.p2 + 0.5)));
        const cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
        if (params.mode == ProcessMode::Erode)
            cv::erode(src, out, kernel, cv::Point(-1, -1), iter);
        else
            cv::dilate(src, out, kernel, cv::Point(-1, -1), iter);
        break;
    }

    case ProcessMode::Sharpen: {
        const double s = std::max(0.0, std::min(100.0, params.p1)) / 100.0;
        // 单位核 (1-s)*I + s*锐化核
        // （不用 Mat_ 的逗号初始化，那个 operator<< 在 OpenCV 5 里已标记弃用）
        cv::Mat kernel = cv::Mat::zeros(3, 3, CV_32F);
        float  *k = kernel.ptr<float>();
        k[0] = 0.f;           k[1] = float(-s);             k[2] = 0.f;
        k[3] = float(-s);     k[4] = float(1.0 + 4.0 * s);  k[5] = float(-s);
        k[6] = 0.f;           k[7] = float(-s);             k[8] = 0.f;
        cv::filter2D(src, out, -1, kernel);
        break;
    }

    case ProcessMode::Sketch: {
        const int k = oddClamp(params.p1, 1, 51);
        cv::Mat gray, inv, blur, inv255, sketch;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        cv::bitwise_not(gray, inv);
        cv::GaussianBlur(inv, blur, cv::Size(k, k), 0, 0);
        // 颜色减淡：out = gray / (255 - blur) * 255
        // 用 convertTo(alpha=-1, beta=255) 算 255-blur，避免依赖 Scalar-Mat 运算符
        blur.convertTo(inv255, -1, -1.0, 255.0);
        cv::divide(gray, inv255, sketch, 255.0);
        cv::cvtColor(sketch, out, cv::COLOR_GRAY2BGR);
        break;
    }

    case ProcessMode::PseudoColor: {
        cv::Mat gray;
        cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
        const int cmap = std::max(0, std::min(21, static_cast<int>(params.p1 + 0.5)));
        cv::applyColorMap(gray, out, cmap);
        break;
    }

    case ProcessMode::Count:
    default:
        out = src.clone();
        break;
    }

    if (out.empty())
        out = src.clone();

    if (elapsedMs) {
        const int64 t1 = cv::getTickCount();
        *elapsedMs = 1000.0 * static_cast<double>(t1 - t0) / cv::getTickFrequency();
    }
    return out;
}

// -----------------------------------------------------------------------------
//  合成测试图：没有摄像头也能验证整条 OpenCV 链路
// -----------------------------------------------------------------------------
cv::Mat makeTestFrame(int width, int height)
{
    cv::Mat img(height, width, CV_8UC3, cv::Scalar(30, 30, 35));

    // 彩色渐变背景（BGR）
    for (int y = 0; y < height; ++y) {
        auto *row = img.ptr<cv::Vec3b>(y);
        for (int x = 0; x < width; ++x) {
            row[x][0] = static_cast<uchar>(120 + 120.0 * x / std::max(1, width - 1));   // B
            row[x][1] = static_cast<uchar>(60  + 150.0 * y / std::max(1, height - 1));  // G
            row[x][2] = static_cast<uchar>(200 - 150.0 * x / std::max(1, width - 1));   // R
        }
    }

    // 几何图形：给边缘检测 / 形态学一点活儿干
    cv::rectangle(img, cv::Rect(width / 12, height / 8, width / 4, height / 3),
                  cv::Scalar(255, 255, 255), 4, cv::LINE_AA);
    cv::circle(img, cv::Point(width / 2, height / 2), height / 5,
               cv::Scalar(60, 220, 255), -1, cv::LINE_AA);
    cv::line(img, cv::Point(width / 8, height * 7 / 8),
             cv::Point(width * 5 / 8, height / 4),
             cv::Scalar(0, 255, 0), 5, cv::LINE_AA);

    // 棋盘格：测模糊 / 锐化
    const int cell = std::max(8, height / 24);
    for (int y = height * 3 / 5; y < height * 3 / 5 + cell * 4 && y < height; ++y) {
        for (int x = width * 2 / 3; x < width * 2 / 3 + cell * 4 && x < width; ++x) {
            const bool on = (((x - width * 2 / 3) / cell) + ((y - height * 3 / 5) / cell)) % 2 == 0;
            img.at<cv::Vec3b>(y, x) = on ? cv::Vec3b(240, 240, 240) : cv::Vec3b(10, 10, 10);
        }
    }

    // 文字：putText 只支持 ASCII，别写中文
    cv::putText(img, "OpenCV self-test frame", cv::Point(width / 20, height / 20 + 10),
                cv::FONT_HERSHEY_SIMPLEX, 0.9, cv::Scalar(255, 255, 255), 2, cv::LINE_AA);
    cv::putText(img, cv::getVersionString(), cv::Point(width / 20, height - height / 20),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);

    return img;
}
