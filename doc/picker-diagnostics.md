# feature_picker_rebuild 卡顿诊断分支

基线：`8578d18ae1d09f2099e3de25a1ddf7d9e9e06120`。
分支：`feature_picker_rebuild_diagnostics`。

这是一份**诊断实现，不是卡顿修复**。不改变 LOD 选择、视锥剔除、拾取结果、同步读回方式、场景缓存策略或现有失败返回逻辑。

## 编译与运行

在现有能够编译原分支的开发环境中：

```sh
git fetch origin
git switch --track origin/feature_picker_rebuild_diagnostics
# 已有同名本地分支时，使用 git switch feature_picker_rebuild_diagnostics。
# 重新执行你现有的 CMake configure / generate 步骤，然后正常编译。
```

CMake 的 `ORCA_PICKER_DIAGNOSTICS` 默认 `ON`。运行时日志也默认开启；不必设置原应用的 Boost 日志级别。

Windows PowerShell 示例（最后一行替换为你编译出的程序实际路径）：

```powershell
$env:ORCA_PICKER_DIAG = "1"
$env:ORCA_PICKER_DIAG_DIR = "C:\temp\orca-picker-logs"
& "D:\your-build\snapmaker-orca.exe"
```

Windows CMD：

```bat
set ORCA_PICKER_DIAG=1
set ORCA_PICKER_DIAG_DIR=C:\temp\orca-picker-logs
"D:\your-build\snapmaker-orca.exe"
```

Linux 示例：

```sh
ORCA_PICKER_DIAG=1 ORCA_PICKER_DIAG_DIR="$HOME/orca-picker-logs" /path/to/snapmaker-orca
```

关闭已经运行的实例后，从设置好环境变量的同一个终端启动新进程。通过转发给已有实例的方式打开模型，不会更新旧进程读取过的诊断配置。运行时开关在进程中只读取一次。

未指定目录时使用系统临时目录：Windows 通常为 `%TEMP%`，Linux 通常为 `/tmp`。文件名为 `orca-picker-<pid>-<unix_ms>.log`。目录自动创建；不可写时不会阻止应用启动，但不能产生日志。

请保留 `.log` 和存在时的 `.log.1`。每个文件约 128 MiB 后滚动，最多保留当前及上一份。异步写入每批 flush，但不承诺断电/强杀时最后几条日志绝对落盘。

## 两种关闭方式

```powershell
$env:ORCA_PICKER_DIAG = "0"  # 同一个诊断程序，运行时关闭日志和新增 GL 查询
```

或者在已有完整 CMake 参数中加 `-DORCA_PICKER_DIAGNOSTICS=OFF` 并重新生成、编译，直接使用未插入埋点的原始 `.cpp`。

## 诊断级别

| 环境变量 | 默认 | 用途 |
|---|---:|---|
| `ORCA_PICKER_DIAG` | `1` | 总开关；`0` 关闭。 |
| `ORCA_PICKER_DIAG_DIR` | 系统临时目录 | 独立日志目录。 |
| `ORCA_PICKER_DIAG_DETAIL` | `0` | 每次 draw、详细 LOD/绑定/pack 状态等。 |
| `ORCA_PICKER_DIAG_GPU` | `0` | 支持时启用异步 GPU timestamp 对。 |
| `ORCA_PICKER_DIAG_GL_ERRORS` | `0` | 在上传、读回、附件分配等调用之后额外读取 GL 错误。 |

**第一轮使用默认级别。** 保持窗口尺寸、驱动和模型一致，分别运行低内存压力/高内存压力的原策略，再用你已有的 Small 优先改动运行高内存压力对照。此提交不内置强制 Small 或关闭读回等策略开关。

有需要时再单独开启 GPU 或 GL 错误检查。错误探针会消费 GL error，可能改变原有 `glsafe` 的断言行为；错误日志标为“调用之后，包括可能存在的旧错误”，不伪称精确确定错误来源。默认模式只旁路记录原有错误消费者已经取出的错误，不额外清空错误状态。

`DETAIL` 会新增只读 GL 状态查询；GPU timer 会提交 query 命令。它们都可能改变时序。没有新增 `glFinish`、阻塞查询结果、额外像素读回或忙等。

## 已实现的观察链路

- `RENDER_ATTEMPT`、`FRAME_ACCEPT`、`FRAME_ABORT/END`，整帧 draw/三角形/上传申请/读回汇总；`SwapBuffers` 单独 BEGIN/END。
- 可见性、context current、初始化、相机准备、主场景、透明绘制、高亮、gizmo、overlay 等阶段耗时。
- Picking FBO 是否重建的决策、创建/复用/销毁、状态恢复、纹理和深度附件申请、FBO complete 状态。
- 每个拾取 volume 的模型/实例/网格身份；**目标 LOD 和真正进入分支的 Small/Middle/High**，以及 High 是主动选择还是 fallback。
- `GLModel` 几何生命周期 UID；实际 VBO/IBO 首次上传的来源 pass、申请字节、索引类型、CPU 数组释放；每条原有 `glBufferData` 及 draw 调用的包装。
- 颜色/深度 `glReadPixels` 各自 BEGIN/END，矩形、格式、数据量；DETAIL 下读 FBO 与 pixel-pack 状态。覆盖所处理四个 `.cpp` 中的原有读回，包括框选 fallback 和缩略图读回。
- GPU 查询返回状态、像素校验、命中信息，以及 CPU All fallback、NonVolume、Bed、选中对象重叠查询的独立计时。
- 主场景重画/缓存呈现的决策、失效次数和调用阶段；捕获帧与相机指纹对照；blit 独立计时。
- LOD 输入复制、QEM、输出面数、共享关系、任务启动/结束、ready 发布、主线程 promote；活跃与峰值 LOD 任务数。
- 独立监视线程每秒采样系统/进程内存，并对持续超过 2 秒的**活跃阶段**输出 `STALL`。正常 idle 不视为卡死。Windows 同时标记调试器连接状态。
- 可选异步 GPU timestamp：最多每个 context 64 对，只在结果 available 后读取；池满则跳过。作用域内有上传或 GPU 空闲时，计时也包含它们，不等同于纯几何计算时间。

## 如何读日志

公共字段有 `seq`、单调时间 `t_us`、进程内线程槽 `tid`、frame/scope/query、canvas/volume/geometry 地址和 geometry UID。`tid` 是诊断线程序号，不是操作系统线程 ID。地址只在本次运行中作为辅助身份；GLModel 通过 reset 退休 UID，再次使用会得到新 UID。

重点搜索：

```text
SESSION
FRAME_ABORT
FRAME_END
PICK_DECISION
PICK_VOLUME
LOD_USE
UPLOAD_MODEL
UPLOAD_REQUEST
READ_REQUEST
CPU_RAYCAST_ALL_FALLBACK
CACHE_STATE
LOD_TASK_END
MEMORY
GL_ERROR
STALL
LOG_DROPPED
```

BEGIN 与 END 使用同一个 scope ID。`STALL` 的 `watched_stage` 和 `watched_scope` 是监视线程看到的渲染/worker 当前阶段。Scope 退出发生在后声明的 GL 状态恢复 guard 退出之后；可定位状态恢复本身的长等待。

- 上传 BEGIN 没有 END：优先调查分配/上传/驱动等待。
- 上传和提交较快、`glReadPixels_color` 很长：读回是等待出口，不能仅凭它认定传输几个像素很慢。
- 读回较快而 CPU fallback 很长：查 raycast 路径。
- `FRAME_END swap_returned=1` 持续快速出现：再看 cache 的相机指纹、捕获帧和重画决策；不代表显示器已完成扫描输出。
- `LOD_USE requested=1 effective=High reason=requested_lod_unavailable`：目标 Middle 不可用，实际用了原模型。枚举 High=0、Middle=1、Small=2。
- `LOD_TASK_END ready_published=0`：任务结束不等于模型已经交接成功；查看之前的输出和原有 LOD 拒绝日志。

Windows `process_commit` 是 PrivateUsage/commit charge，不是磁盘页面文件实际使用量。系统 commit 页数已换算为字节。Linux 的 `VmSize` 是虚拟地址空间大小，不能当成物理占用。

`live_vertex_requested/live_index_requested` 是按唯一 GLModel 去重的**逻辑申请账本**，不是驱动实际驻留量，也不是已验证分配成功量。它不包含所有纹理、FBO、CPU/QEM 中间数据或其他模块分配。`triangles` 按 draw 实际提交次数累加，共享几何重复绘制仍重复计数。

## 边界和未包含项

这次实现侧重可直接运行的主要定位链路，不宣称把原 37 点清单全部实现。

尚未包括 DXGI/WDDM GPU 预算与驻留采样、系统 TDR 事件收集、GPU debug callback、最终屏幕动态标记、完整场景几何版本模型、全部 early-return 的枚举原因、每个失效调用点的精确源码原因，以及独立 CPU 缺页性能跟踪。`gpu_budget=not_sampled` 会在内存日志中明确注明。

默认帧退出日志能区分“未接受的 render 调用”与“完整执行”，但某些初始化早退需要结合相邻阶段返回值进一步判断。相机缓存指纹不涵盖模型变换/材质/可见性本身，不能代替完整场景版本号。

日志使用有界队列，生产线程只在短锁内入队，不持锁执行 GL 或文件 IO；队列满时计数并输出 `LOG_DROPPED`。监视槽最多 128 个曾使用诊断的线程，超过后仍可写日志，但这些线程不再有 watchdog 槽。日志本身有开销，应与 runtime OFF/编译 OFF 对照，不能据此承诺完全不改变复现概率。

## 构建接入方式

为了保留一份完全不改动的基线，原 `src/slic3r/CMakeLists.txt` 原样保存在同目录的 `CMakeLists.picker-base.cmake`。新的短入口先包含基线，再包含诊断模块。

CMake 在 configure 时，根据 `GUI/PickerDiagnostics-*.cmake` 中可审阅的精确锚点，生成四个带埋点的 `.cpp` 到构建目录的 `src/slic3r/picker-diagnostics/`，替换 GUI target 的相应编译输入。**无需手工运行补丁，也不依赖 Python。** 仓库中的四个原始 `.cpp` 不被覆盖。

锚点缺失或有歧义时配置会明确失败，避免在不同代码版本中静默插入错误位置。修改埋点请编辑 recipes，不要编辑 build 目录生成文件。切换原分支后重新 configure，就不再链接诊断模块。

标准库日志模块单独构建，避免混入应用的旧 PCH 设置；生成的应用源文件保留原有源文件编译属性。

## 自测与验证范围

自测只依赖 C++17、CMake 和线程库，不依赖 wxWidgets 或真实显卡：

```sh
cmake -S tests/picker_diagnostics -B build-picker-diag-tests
cmake --build build-picker-diag-tests --config Release
ctest --test-dir build-picker-diag-tests -C Release --output-on-failure
```

测试使用独立 Mock GL（不会进入应用 target）：检查 GL 调用仍执行一次、宏返回引用/移动值及异常传播、作用域恢复、关闭模式无额外 GL 查询、几何 UID 退休、GPU pending 不阻塞读取、query 池上限、缓存指纹变化、并发日志及慢阶段心跳。

提交前已在 Linux 用 GCC 构建并通过 enabled/disabled 两项 CTest；Clang 的严格警告编译亦通过。**没有在此环境完成整个 Orca 的编译，也没有在 Windows/780M 上运行验证。** 这些自测不等价于真实驱动测试。
