# camera_qt —— Qt6 + OpenCV 摄像头采集与处理

在原来「只用 Qt 自带库看摄像头」的基础上，加上了 **用 OpenCV 抓帧并做图像处理** 的完整链路。

界面是两个标签页：

| 标签页 | 干什么 | 用到的库 |
|---|---|---|
| **Qt Multimedia 预览** | 原来的那套，纯看画面 | `QCamera` + `QVideoWidget` |
| **OpenCV 采集 + 处理** | OpenCV 抓帧 → 处理 → 显示 / 存图 | `cv::VideoCapture` + `cv::imgproc` |

---

## 1. 本机环境

| 组件 | 路径 / 版本 |
|---|---|
| Qt | `D:\qt6\6.11.1\msvc2022_64`（6.11.1，含 QtMultimedia） |
| 编译器 | MSVC 2022，`D:\vs2022`，工具集 14.44.35207 |
| OpenCV | `D:\opencv\build`（官方预编译包 5.0.0，`x64\vc16`，world 单库） |

> 也保留了 MinGW 那条路：`build-mingw.bat` + `D:\opencv\install-mingw`（自己编的 MinGW 版，模块是分开的）。
> `camera_qt.pro` 会根据套件自动选库：`-lopencv_world500` 还是 `-lopencv_core500 -lopencv_imgproc500 ...`。

---

## 2. 构建

双击 `build.bat`，或者命令行：

```
build.bat                :: release
build.bat debug          :: debug
build.bat release nopause :: 不暂停（给脚本调用）
```

脚本做三件事：`vcvars64` → `qmake` → 编译，然后 `windeployqt` 部署 Qt 运行时、把 OpenCV 的 DLL 拷到 exe 旁边。
**影子构建**：所有中间产物都在 `build-msvc\`，工程根目录只留源码。

产物：`build-msvc\release\camera_qt.exe`

> ⚠ 两个 `.bat` 是 **UTF-8 + CRLF** 编码，开头有 `chcp 65001`。
> 用别的编辑器改的时候别把编码换成 GBK，也别把行尾改成 LF —— 否则 `cmd` 会把中文当命令执行。

### 关于 jom

`build.bat` 优先用 jom 并行编译；但这个工程比 `D:\qt6` 深，
qmake 会生成 `..\..\..\..\..\qt6\...` 这种越过盘符根目录的相对依赖，jom 解析不了会报
`dependent ... does not exist`。脚本会自动退回 `nmake`，不用管。

---

## 3. 运行

### 图形界面

```
build-msvc\release\camera_qt.exe
```

右边控制面板分三组：

1. **摄像头** —— 设备序号（OpenCV 只认序号，不认设备名）、采集后端（自动 / MSMF / DSHOW）、
   请求分辨率、目标帧率，还有「探测设备」按钮（挨个序号硬试，因为 OpenCV 没有枚举摄像头的 API）。
2. **OpenCV 处理** —— 16 种处理模式，每种模式有自己的 1~2 个参数滑块，拖动即时生效。
3. **显示 / 快照** —— 可以看处理结果、原始画面、或左右对比；「保存快照」会把**原图和处理后**各存一张 PNG。

状态栏实时显示 FPS、分辨率和这一帧的处理耗时。

> 两个标签页共用同一个摄像头，所以打开一边会自动停掉另一边。

### 命令行

| 参数 | 作用 |
|---|---|
| （无） | 打开界面 |
| `--autostart` | 打开界面后自动切到 OpenCV 页并开摄像头（省一次点击） |
| `--mode N` | 配合 `--autostart`，把初始处理模式设成第 N 种（0~15） |
| `--list` | 列出 Qt 能看到的摄像头设备 |
| `--opencv-info` | 打印 Qt / OpenCV 版本和 OpenCV 构建信息 |
| `--selftest` | **不用摄像头**，用合成图跑一遍全部处理模式并出图 |
| `--grab [序号]` | **真抓一帧摄像头画面**，再拿这一帧跑一遍全部处理模式并出图 |
| `--backend auto\|dshow\|msmf` | 配合 `--grab` 指定后端，用来对比打开速度 |

因为是 GUI 子系统程序，命令行输出会同时写到 exe 旁边的 `cli_output.txt`
（`--selftest` / `--grab` 写各自目录下的 `selftest.log` / `grab.log`），
所以就算控制台没接上也拿得到结果。

### 打开摄像头：0.5 秒 —— 以及为什么以前要十几秒

同一个摄像头、同一台机器，只换后端（用 `--grab --backend xxx` 实测）：

| 后端 | 打开耗时 | 实际帧率 |
|---|---|---|
| **DSHOW + MJPG**（现在的默认） | **0.46 ~ 0.62 s** | **30 fps** |
| DSHOW，不指定 fourcc | 0.6 s | **10 fps** |
| MSMF | **9.8 ~ 10.9 s** | 30 fps |

两个坑叠在一起，所以"打开很慢"和"帧率很低"看着像两回事，其实是同一段代码：

1. **MSMF 打开就是慢。** 它要花 10 秒才 `open()` 成功。我最初的默认顺序是
   `MSMF → DSHOW → ANY`，于是每次都先撞上这 10 秒。**改成先试 DSHOW**
   （实测设备不存在时三条路都只要 ~0.2s，所以多留回退路并不贵）。
   另外请求具体分辨率会让 MSMF 更慢（13.3s vs 不指定时的 4.1s），因为它要重新协商 media type。

2. **DSHOW 在 720p 默认会协商成 YUY2（无压缩），这个摄像头只能跑 ~10fps。**
   显式点名 **MJPG** 才能到 30fps，而且打开还更快。
   注意 **MSMF 不能加 FOURCC** —— 它自己会协商 MJPG，参数里带 `CAP_PROP_FOURCC`
   会直接失败（`can't set property 6`）。所以代码里 FOURCC 只给 DSHOW 加。

这些都写在 `CaptureWorker::openCapture()` 的注释里，界面状态栏也会显示
`后端 DSHOW | MJPG | 分辨率 1280x720 | 打开耗时 0.5 s`，方便换机器时一眼看出
是哪个后端慢、以及 MJPG 有没有协商成功。

> **关于帧率，别被单次数字骗了。** 上面的 30fps 是光线充足时测的；
> 同一天傍晚再测，MJPG 只有 15fps、YUY2 只有 10fps —— 因为**摄像头自动曝光在暗环境里
> 会拉长曝光时间，帧率自然掉下来**（两次测得曝光值都是 -6.0）。这是摄像头的行为，不是代码问题。
> 判断依据是**背靠背对比**：同一时刻 YUY2 9.9fps vs MJPG 14.9fps，MJPG 始终快 ~50%。
> 所以看帧率要**同一时刻两种配置对比着看**，不能拿两次不同时间的绝对值比。

> 权衡：MJPG 是有损压缩，对边缘检测这类处理会引入极轻微的压缩噪声。
> 换来的是帧率高出 50% 以上。真想要无压缩，可以在界面上把后端选成 MSMF
> （它自己也用 MJPG，但要等 ~10 秒打开）。

### 控制台那些 WARN / ERROR 是什么？

从终端启动时可能看到这样几行：

```
qt.multimedia.ffmpeg: Using Qt multimedia with FFmpeg version 7.1.3 ...
[ WARN] VIDEOIO(DSHOW): backend is generally available but can't be used to capture by index
[ WARN] VIDEOIO/FFMPEG: OpenCV should be configured with libavdevice to open a camera device
[ERROR] obsensor_uvc_stream_channel.cpp: Camera index out of range
```

**这些和图像处理没有任何关系** —— `cv::Mat` / `imgproc` 那一套从来不打印东西。
它们全部来自 **videoio（打开摄像头）**：

OpenCV 没有"枚举摄像头"的 API，找设备只能挨个后端试（GSTREAMER / MSMF / DSHOW /
FFMPEG / UEYE / OBSENSOR），每一条走不通的路它都会抱怨一行。上面第 2、4 行就是
DSHOW 和 OBSENSOR 走不通，第 3 行是说这个 OpenCV 编译时没带 libavdevice，
所以 FFMPEG 打不开摄像头。属于**我们主动试探产生的噪音，不是故障**。

现在这些噪音在试探期间被压掉了（真出问题该有的日志一条不少）。
想看原始输出：设环境变量 `OPENCV_LOG_LEVEL=INFO`（或 `DEBUG`），程序会自动让路。

### 线程模型：worker 对象 + moveToThread

`QThread` 只管跑事件循环，`CaptureWorker` 是一个普通 `QObject`，被 `moveToThread()` 搬进去：

```
界面线程                                 采集线程
  │                                        │
  ├─ invokeMethod(startCapture, Queued) ──▶ startCapture()  ← 槽，在采集线程执行
  │                                        │  openCapture()
  │                                        │  QTimer::start(1000/fps)
  │                                        │
  │◀── framesReady(QImage, QImage) ────────┤  grabOneFrame() 每次超时跑一次
  │◀── statsReady(QString) ────────────────┤
  │◀── stopped() / shutdownDone() ─────────┤
  │                                        │
  ├─ invokeMethod(shutdown, Queued) ──────▶ shutdown() → emit shutdownDone()
  │                                        │
  └─ shutdownDone() → QThread::quit() ────▶ 事件循环退出，run() 返回 → finished()
```

**为什么是这个模型，而不是继承 `QThread` 重写 `run()`：**

1. **队列连接天然串行化**。"先停止再打开"这种顺序不用自己保证 —— 先排 `stopCapture`
   再排 `startCapture`，worker 一定按这个顺序执行；
2. **参数和启停全都不用锁**。它们和取帧都在采集线程里顺序执行，
   所以 `m_params` / `m_capturing` 这些成员一个 `QMutex` 都不需要；
3. **停机就是 `quit() + wait()`**，生命周期干净。

代价是采集循环必须变成**事件驱动**：`grabOneFrame()` 由 `QTimer` 每 `1000/fps`
毫秒叫一次，不再是 `while` 紧循环。顺带一个好处：`QTimer` 在处理完上一次超时之前
不会再排新的，所以处理慢了会**自然降帧、不会堆积**。

> ⚠ `CaptureWorker` 的方法**必须**通过队列连接调用。
> 谁要是直接在界面线程里 `m_worker->startCapture(...)`，就会在错误的线程里
> 碰 `cv::VideoCapture` 和 `QTimer`，而且**不会有任何报错**。
> 类里的 `public slots:` 就是为了让这件事在代码上一眼可见。

> ⚠ `m_timer` 必须是 `m_worker` 的**子对象**（`new QTimer(this)`），
> 因为 `moveToThread()` 只会把对象**和它的子对象**一起搬走。
> 写成值成员或者无父对象的话，定时器会留在界面线程，回调就跑错线程了。

### 关窗口很快，不会假死

采集线程收尾有几处躲不掉的耗时（都是实测值）：

| 环节 | 耗时 |
|---|---|
| `grabOneFrame()` 里的 `cap.read()` | 平均 33 ms，DSHOW 最坏 ~400 ms |
| `shutdown()` 里的 `cap.release()` | **MSMF / DSHOW 都要 ~300 ms** |

如果让界面线程 `wait()` 去等这些，窗口就完全不能重绘 —— 看起来就是"点了关闭没反应"。
所以全程异步：

1. `closeEvent` 往采集线程排一个 `shutdown()`（立刻返回），窗口继续响应事件并显示
   "正在关闭摄像头……"；
2. `shutdown()` 停掉定时器、交还摄像头，然后发 `shutdownDone()`；
3. 构造函数里连好的 `shutdownDone() → QThread::quit()` 让事件循环退出，线程结束；
4. 线程的 `finished()` → 真正 `close()`。

万一驱动卡死，超过 **1200 ms** 宽限期就直接 `quit()` 走人；真要还停不下来，
就把 `QThread` 从父子树里摘出来故意不 `delete` —— 因为 `~QThread` 遇到还在跑的线程
会让 Qt 直接 `abort` 整个进程。

实测（采集中用 `CloseMainWindow()` 关窗口）：**396 / 406 / 430 ms**，
未采集时 **46 ms**。其中 ~300ms 就是摄像头自己释放句柄的固有开销。

`--selftest` 和 `--grab` 不只是"跑一遍不崩"，每种模式都会检查：

- 输出尺寸 / 类型合法，8UC3；
- 默认参数下该变的必须真的变，「原图」「亮度对比度(0,100%)」这种声明恒等的必须真的不变；
- 结构不变量：灰度类输出不能有彩色像素、二值化输出只能有 0/255、反色必须精确等于 `255 - 输入`；
- 恒等模式的参数推到 75% 位置必须产生变化。

全部通过才返回 0。

---

## 4. 代码结构

按职责分成六个模块，`main.cpp` 只做入口和分发：

```
main.cpp              程序入口：建 QApplication、解析命令行、分发、进事件循环
cli.h/.cpp            命令行诊断工具（--list / --opencv-info / --selftest / --grab）
camerawindow.ui       主窗口界面（Qt Designer 文件，qmake 调 uic 生成 ui_camerawindow.h）
camerawindow.h/.cpp   主窗口逻辑：下拉框数据、信号连接、子线程编排、快照
videoview.h/.cpp      等比缩放显示 QImage 的显示控件（在 .ui 里被提升使用）
captureworker.h/.cpp  采集 worker（OpenCV 抓帧 → 处理 → 信号发 QImage 回界面线程）
processing.h/.cpp     纯 OpenCV 图像处理（不依赖 Qt，可单独复用）
camera_qt.pro         qmake 工程，含 OpenCV 路径探测
```

依赖方向是单向的，没有循环：

```
main ──▶ camerawindow ──▶ videoview
  │           │
  └──▶ cli    └──▶ captureworker ──▶ processing
```

- **main.cpp** 只有 ~45 行，**除了 `main()` 一个自定义函数都没有**：
  建 QApplication、设全局开关、把命令行交给该管的人、进事件循环。
  参数解析也不在这里 —— 见下面「开关归谁管」。
- **cli.cpp** 是"没有界面也能跑"的诊断逻辑，和窗口零耦合，
  所以 `--selftest` / `--grab` 在纯命令行环境下也能独立验证整条处理链路。
- **processing** 这层不碰 Qt：输入一张 BGR `cv::Mat`，输出一张 BGR `cv::Mat`。
  处理模式表 `kModes` 是数据驱动的 —— 界面上的下拉框、参数滑块的名称和取值范围都从表里生成，
  加一种处理只要在 `enum` 和表里各加一行。
- **captureworker** 是 worker 对象（`QObject`）跑在独立的 `QThread` 里，
  因为 `cv::VideoCapture::read()` 是阻塞的，在界面线程里读帧会让窗口卡死。
  线程模型见上面「worker 对象 + moveToThread」那节。
- **camerawindow** 只管界面和编排，不做算法、也不直接碰 OpenCV 的抓帧。
- Qt 的 `QVideoFrame` 可能是 NV12/YUV 甚至 GPU 纹理，转成 `cv::Mat` 又慢又容易错，
  所以 OpenCV 这一路直接自己抓帧，链路最短。

### 开关归谁管

命令行参数**不在入口解析**。"有哪些开关、长什么样、怎么校验"是各自功能自己的知识，
所以解析跟着功能走：

| 开关 | 归谁 | 入口怎么用 |
|---|---|---|
| `--list` `--opencv-info` `--selftest` `--grab` `--index` `--backend` | `cli` | `cli::runFromArgs(args)` 命中就返回退出码，没命中返回 `std::nullopt` |
| `--autostart` `--mode` | `CameraWindow` | `window.applyStartupOptions(args)` |

于是 `main.cpp` 长这样，而且**一个自定义函数都没有**：

```cpp
if (const std::optional<int> exitCode = cli::runFromArgs(args))
    return *exitCode;          // 命令行模式，跑完就退

CameraWindow window;
window.show();
window.applyStartupOptions(args);   // 界面自己的开关
return app.exec();
```

好处很直接：**加一个新开关只要改对应模块，不用动入口**。
反过来说，如果哪天发现 `main.cpp` 里又多出一个 helper 函数，
基本就是"某个开关的解析被放错地方了"的信号。

### 改界面：直接用 Qt Designer 打开 `camerawindow.ui`

界面全部在 `camerawindow.ui` 里，`camerawindow.cpp` **一行布局代码都没有**
（没有 `addWidget` / `addRow` / `new QXxx(控件)`）。改界面不需要碰 C++。

工程里已经配好 `FORMS += camerawindow.ui`，qmake 会自动调 `uic` 生成
`ui_camerawindow.h`，所以**改完 .ui 直接重新构建就行**。

用 Designer 打开时注意三件事：

| 事项 | 说明 |
|---|---|
| **`VideoView` 是提升控件** | 它是个自定义 `QWidget` 子类。在 .ui 里用 `<customwidget>` 声明，Designer 里会显示成 `VideoView`（不是普通 QWidget），能正常拖放和设布局，只是没有可视化预览 |
| **控件名就是代码里的名字** | 代码统一用 `ui->xxx` 访问，没有另存一份 `m_xxx` 指针。所以在 Designer 里**改名 = 改代码里的引用**，编译期就会报错，不会出现"改了没生效" |
| **有些东西故意留在代码里** | 见下 |

留在 `wireUpUi()` 里、不在 .ui 里的东西，都是 .ui 表达不了的：

- **下拉框条目**：`backendBox` / `resolutionBox` 的条目要带 `int` / `QSize` 的 userData，
  `modeBox` 的条目是从 `processing` 的模式表生成的（加一种处理不用动 .ui）；
- **信号连接**：Designer 的 `<connections>` 只能连控件之间的槽，
  我们的槽都在 `CameraWindow` 上，写在代码里更直观；
- **`displayBox` 的条目**和 `splitter` 的初始列宽。

> 关于 `splitter` 的 `sizes`：`uic` 不认识 `QSplitter` 的 `QList<int> sizes` 属性
> （写进 .ui 会报 `Unexpected element list`），所以列宽只能在 `wireUpUi()` 里给。
> 在 Designer 里拖动分割线调出来的比例**不会**被保存进 .ui。

### 处理模式

| # | 模式 | 参数 |
|---|---|---|
| 00 | 原图（不做处理） | — |
| 01 | 灰度 | — |
| 02 | 高斯模糊 | 核大小 |
| 03 | 中值滤波 | 核大小 |
| 04 | 双边滤波（磨皮） | 邻域直径、颜色 sigma |
| 05 | Canny 边缘检测 | 低阈值、高阈值 |
| 06 | 阈值二值化 | 阈值 |
| 07 | 自适应阈值 | 邻域大小、常数 C |
| 08 | 反色（负片） | — |
| 09 | 直方图均衡 | — |
| 10 | 亮度 / 对比度 | 亮度、对比度(%) |
| 11 | 腐蚀 | 核大小、迭代次数 |
| 12 | 膨胀 | 核大小、迭代次数 |
| 13 | 锐化 | 强度 |
| 14 | 铅笔素描 | 模糊核 |
| 15 | 伪彩色 | 色图编号 |

![处理效果预览](docs/effect_preview.png)

> 这张拼图是 `camera_qt.exe --grab` 用真实摄像头抓的一帧（1280x720），
> 再分别跑一遍 16 种模式得到的，可以用同一条命令随时重新生成。
> 14 号「铅笔素描」用的是经典的颜色减淡（color dodge）算法：平坦区域会被算成白纸，
> 只有边缘留下灰色笔触 —— 所以对着窗户这种大片高亮又没什么细节的画面，
> 结果就会偏白，把「模糊核」调小可以让笔触更明显。

---

## 5. 踩过的坑（改代码前先看这里）

1. **目录不能带「低完整性」标签。**
   `Mandatory Label\Low Mandatory Level` 会让从该目录启动的进程以低完整性运行，
   而 Windows 禁止低完整性进程访问摄像头 —— 表现是 `QMediaDevices::videoInputs()` 返回 0 个，
   或者 OpenCV 读帧失败，报错还很难懂。检查：`icacls <目录> | findstr /i Mandatory`。

2. **分辨率必须在 `open()` 时就谈好，不能在打开成功后再 `set()`。**
   OpenCV 5 + MSMF 下，`cap.set(CAP_PROP_FRAME_WIDTH/HEIGHT, ...)` 会打印
   `initStream Failed to select stream 0` 并且**把流弄死**，之后 `read()` 再也拿不到帧，
   而 `set()` 只是返回 `false`，看上去人畜无害。正确做法是
   `cap.open(index, api, {CAP_PROP_FRAME_WIDTH, w, CAP_PROP_FRAME_HEIGHT, h})`。

3. **`cv::VideoCapture` 没有移动赋值。**
   写 `cap = std::move(probe)` 会退化成隐式拷贝，两个对象共用同一个底层句柄，
   析构时互相 `release()`。要在调用者的对象上直接 `open()/release()`。

4. **OpenCV 5 的 API 变化。**
   后端枚举从 `cv::VideoCapture::getBackends()` 搬到了
   `cv::videoio_registry::getCameraBackends()` / `getBackendName()`；
   `Mat_` 的逗号初始化 `operator<<` 已标记弃用，改用 `Mat::zeros` + `ptr<float>()`。

5. **`cv::countNonZero` 只接受单通道**，多通道矩阵要先 `.reshape(1)`。

6. **界面线程里 `wait()` 等采集线程 = 窗口假死。**
   线程收尾要过 `read()`（最坏 0.4s）+ `release()`（~0.3s），
   合起来半秒左右，全部转嫁到界面上。正确姿势是"只排队请求，窗口继续跑事件循环"，
   详见上面「关窗口很快，不会假死」那节。

7. **`QThread` 对象还在跑时被 `delete`，Qt 会直接 `abort` 进程。**
   所以"放弃等待"不能只是不管它 —— 必须 `setParent(nullptr)` 把它从父子树里摘出来，
   否则 `~QObject` 递归析构子对象时就会炸。

8. **`moveToThread()` 只搬对象和它的子对象。**
   `QTimer` 必须写成 `new QTimer(this)`（子对象），否则它会留在界面线程，
   超时回调跑错线程 —— 而且**不会报任何错**，只是行为诡异。

9. **跨线程队列传自定义类型要先注册元类型。**
   `ProcessParams` 走队列连接，需要 `Q_DECLARE_METATYPE`（在 `captureworker.h`）
   **加上**运行时的 `qRegisterMetaType<ProcessParams>()`。
   漏了的话 `invokeMethod` 返回 `false`，参数静默丢失、画面不跟着变
   —— `pushParams()` 里对返回值做了检查，失败会在状态栏红字提示。

10. **别在 QThread 子类里放槽然后指望它跑在新线程。**
    这是"不要继承 QThread"那句教条的真正含义：`QThread` 对象属于**创建它的线程**，
    它的槽和队列调用都在那个线程执行。本项目用的是 worker 对象 + `moveToThread`，
    所以队列调用确实落在采集线程里；但这也意味着 `CaptureWorker` 的方法
    **必须**通过队列连接调，直接调用就是在界面线程里跑。

11. **DSHOW 在 720p 会默认选 YUY2，帧率只有 10fps。**
    要在 `open()` 的参数里显式点名 `CAP_PROP_FOURCC = MJPG` 才是 30fps。
    反过来 **MSMF 不能带 FOURCC**，否则 `open()` 直接失败。

12. **回退顺序要按"快"排，不是按"新"排。**
    MSMF 是更新的框架，但它在这个摄像头上要 10 秒才打开，
    而 DSHOW 只要 0.5 秒 —— 默认先试 MSMF 就白等 10 秒。
    这类事情**必须实测**，别凭"哪个更现代"下判断。

---

## 6. 生成的目录

| 目录 | 内容 |
|---|---|
| `build-msvc\release\snapshots\` | 界面上点「保存快照」存的 PNG（`raw_*.png` / `proc_*.png`） |
| `build-msvc\release\selftest_out\` | `--selftest` 的输入图、每种模式的输出、日志 |
| `build-msvc\release\grab_out\` | `--grab` 真实抓帧的结果，同上 |
| `releaseCamera\` | 旧的 MinGW 影子构建目录（保留） |
