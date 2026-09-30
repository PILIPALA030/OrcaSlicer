# PCSS 第二轮性能优化：局部深度界与分阶段计时

> 日期：2026-09-30。基线：`b2f1fa8b221c40411ab3b72b94f7f221f8d27cf0`。
> 功能提交：`1ca5bfd7855fa4fae1ccdce2ec1fc91a58c2d915`。
> 分支：`feature/pcss-editor-gcode-shadows`。只更新该分支，不改 main。
> 用户反馈：关闭阴影约 50 多 FPS，开启后最多约 20 FPS。没有用户显卡的逐阶段 GPU 数据，不能据此确定唯一瓶颈，也不能保证本轮恢复到 50 FPS。

## 1. 为什么上一轮仍不足

50–55 FPS 对应约 20–18.2 ms/帧，20 FPS 对应约 50 ms/帧。差值约 30–32 ms；这是根据用户观测换算的应用帧时间差，不是 PCSS 独占 GPU 耗时的直接测量。

上一轮修复了强制 High LOD、像素循环重复生成采样位置和整体 caster 包围框以外的无效查询，但在整体范围内部，局部全亮的表面仍可能执行完整 16 次 blocker 搜索，完全处于阴影内部的表面仍可能再执行 32 次 PCF。相机固定光随相机旋转而变化、模型变换和 G-code 范围变化所需的深度更新也仍有成本。

本轮同时提供保守局部区域优化和实际应用的诊断计时。不能继续把简单场景的 Shader 加速倍数等同于完整 Orca FPS 提升。

## 2. 保守局部深度界

### 2.1 层级摘要，不是四个点猜测阴影

新增一个深度摘要纹理。最细一级每个节点覆盖原始深度图的 4×4 texel，之后逐级合并四个子节点。查询时选取合适层级，最多四个节点覆盖整个待查矩形。

这些节点保存的是各自整个区域的保证范围，不是只读取四个位置就猜测可见度。只有证明整个搜索/过滤范围完全亮或完全暗时，才跳过原 PCSS；不确定区域保持原来的完整搜索、平均 blocker 距离和 PCF。

### 2.2 为什么保存深度平面和残差

单纯保存 raw depth 的 min/max，在倾斜接收面上会产生很宽的范围，即使没有自遮挡也难以提前结束。这里的 RGBA32F 节点保存中心深度、每深度 texel 的 X/Y 斜率和最大残差界：

```text
abs(depth(x,y) - [center_depth + slope_x*(x-center_x) + slope_y*(y-center_y)]) <= residual
```

细层用真实 16 个源深度值拟合后取最大残差；粗层通过子层中心偏差、斜率差和子层残差传播界，并逐层加入向外的浮点安全量。查询时减去 receiver plane 梯度，同时计入最近邻量化的半 texel 偏移。

这个平面不是替代真实深度图：它只负责证明可以提前结束，模糊边缘仍比较原始 depth texture。

### 2.3 提前结束的逻辑

- 全亮：整个 blocker 搜索范围内不可能存在满足 blocker 判定的深度，返回可见度 1。
- 全暗：先由接收深度上界和 blocker 深度下界推导最终滤波半径上界，再证明搜索范围及所有可能的滤波范围均被遮挡，返回 0。
- 不确定：执行原有 16/32 默认采样，算法与样本分布不变。

全暗验证覆盖的不能只是初始搜索盘，因为 receiver 梯度可能使最后的滤波盘更大。纹理边界、孔洞、薄片和大的残差界会保守回到完整 PCSS，不通过简单的随机探针早退。

## 3. 资源、缓存和兼容

`PCSSShadowAcceleration.hpp` 只由 `PCSSShadowRenderer.cpp` 包含，内部管理 reduction Shader、摘要纹理、FBO 和可选计时查询。reduction Shader 绘制无顶点属性的三角形，不调用 GLModel，故没有增加宿主 Shader 管理器的注册入口。

默认 2048 深度图使用 512×512 的摘要第零层及 mip 链，RGBA32F 约增加 5.33 MiB 纹理存储。非二次幂尺寸向上补齐，补齐区域视为深度 1。没有复制整份模型或 G-code 几何。

摘要仅随深度 generation 或分辨率变化更新，静态 cache 命中时复用。没有隔帧更新、冻结阴影或忽略 G-code 滑块。摘要不可用时回到原完整 PCSS，不关闭全部阴影。

构建 mip 时把源纹理的 BASE_LEVEL/MAX_LEVEL 限制到输入层，使正在写入的输出层不属于采样可访问范围；结束后恢复完整范围。这比仅指定显式 LOD 更严格，针对 GL 3.1 反馈规则设计。

新增临时占用纹理单元 6，原始深度仍在 7。保存并恢复这两个单元的纹理、采样器和调用者活动纹理单元，保留 FBO、viewport 等状态恢复。已持有外层状态保护的深度更新路径不再重复采集一套完整状态。

资源在拥有它们的 current GL 上下文中释放。丢失上下文的 abandon/destructor 不调用 glDelete，避免误删新上下文重用的名字。

## 4. 接口与默认行为

| 新增/修改接口 | 用途 |
|---|---|
| `PCSSSettings::use_depth_bounds` | 追加在结构尾部，默认 true；false 回到完整 PCSS 查询 |
| `PCSSShadowRenderer::update_depth_bounds(bool)` | 私有；按深度 generation 构建或复用摘要 |
| `PCSSReceiverScope(..., bool plate_receiver = false)` | 保持原调用兼容；板面显式标记便于统计 |
| `PCSSShadowAcceleration` | 私有资源、区域界、异步计时与诊断选项 |
| `pcss_query_depth_range()` | GLSL 保守局部范围查询 |
| `pcss_depth_bounds_tests` | 原生 GPU 对比、包围性、状态和诊断测试 |

默认分辨率 2048、blocker 16、PCF 32、完整角直径 4°、最大半径 12 mm 均不变。上一轮共享 LOD、编辑页/预览页调用链、G-code 可见范围、持久化开关不变。不修改切片参数或制造行为。

本轮直接使用 GitHub 写接口提交文件。CI 只有原来的只读验证工作流，没有新增用 CI 写源码的临时工作流。

## 5. 分阶段 CPU/GPU 诊断

默认不开启 profiling。`ORCA_PCSS_PROFILE=1` 启用以下统计：

| 字段 | 范围 |
|---|---|
| `depth` | 深度 Pass 设置、清除和实际几何提交 |
| `bounds` | 摘要构建；首次分配和编译也在内，先预热再分析 |
| `model` | 普通模型的 receiver 绘制作用域 |
| `gcode` | 挤出路径及端面的 receiver 绘制作用域 |
| `plate` | 打印板 receiver 绘制作用域 |

`cpu_ms` 是作用域 CPU 提交时间的均值；`gpu_ms` 是 GPU timestamp 对应区间的均值。model/gcode/plate 包含该作用域的正常几何和材质工作，不是 PCSS 函数独占耗时。深度前的场景收集/LOD 评估、其它界面绘制和呈现等待不包括在这些计时内。

计时使用 32 个有界 ticket 槽，至少延迟两帧再检查 QUERY_RESULT_AVAILABLE，未就绪不等待、不覆盖；槽满就丢弃计时样本。生产路径没有 glFinish、像素回读或等待 GPU 完成的轮询。无 timer_query 时只输出 CPU，GPU=NA 不等于 GPU 耗时为零。

每两秒在实际阴影 update 时输出一次，释放时输出余下统计；不会为日志启动永久绘制循环。CPU 和 GPU 样本来自稍有错位的窗口，应同时看 calls/gpu_samples，不能把各均值直接相加当整帧时间。

日志带有 `revision=depth-bounds-v2`、实际 map/samples、GL renderer、filter、receivers、bounds、各阶段耗时。`bounds_receiver_calls` 表示诊断期间绑定了新层级 uniform 的接收次数，不是像素早退率。如果该值一直为 0，应核查是否有相关接收者及运行目录是否仍在使用旧 pcss.glsl。

### Windows PowerShell

在同一个启动终端先执行：

```powershell
$env:ORCA_PCSS_PROFILE = "1"
$env:ORCA_PCSS_PROFILE_LOG = "$env:TEMP\orca-pcss-profile.log"
```

然后从该终端启动本次编译出的实际 Orca 可执行文件，开启偏好的软阴影。不要改从另一个已存在的进程/快捷方式启动，否则可能不继承环境变量。

预热后，在导致降帧的场景持续旋转视角或拖动层滑块约 10 秒，记下操作，再退出读取日志。日志追加写入，可为不同实验使用不同文件名。

### Linux

同一终端设置后启动实际程序：

```bash
export ORCA_PCSS_PROFILE=1
export ORCA_PCSS_PROFILE_LOG=/tmp/orca-pcss-profile.log
```

不指定日志文件时写 stderr；指定路径不可写时提示并退回 stderr。

### 控制实验：每次只改变一项，重新启动

| 环境变量 | 行为 | 用途 |
|---|---|---|
| `ORCA_PCSS_BOUNDS=0` | 不生成/使用新增摘要，仍完整 PCSS | 直接比较本轮优化的净收益 |
| `ORCA_PCSS_FILTER=hard` | 保留同样深度几何，改一次硬比较，不构建摘要 | 判断过滤成本是否占主要份额 |
| `ORCA_PCSS_RECEIVERS=plate` | 保留深度，只让板面接收阴影 | 对比模型/刀路接收的贡献 |
| `ORCA_PCSS_RECEIVERS=none` | 保留深度生成，不构建摘要，不让表面接收 | 与偏好关闭对比，观察深度生成和集成成本 |

后三个是明确改变显示结果的诊断模式，不是默认性能方案，不能用这些模式的高帧率宣称完整 PCSS 提升。环境变量在渲染器初始化时读取，改变后应重新启动应用。

恢复正常：

```powershell
Remove-Item Env:ORCA_PCSS_BOUNDS, Env:ORCA_PCSS_FILTER, Env:ORCA_PCSS_RECEIVERS -ErrorAction SilentlyContinue
```

```bash
unset ORCA_PCSS_BOUNDS ORCA_PCSS_FILTER ORCA_PCSS_RECEIVERS
```

排查结束可移除 PROFILE 和 PROFILE_LOG，消除计时本身的小量开销。

## 6. 验证范围

本地原型直接编译实际 PCSSShadowRenderer，执行过 OpenGL 3.1/GLSL140、3.3 Core 和 ASan/UBSan（未启用 LeakSanitizer）。对固定上一版 Shader 的所测场景，最大可见度读回差为 0。

最终提交的 CI 保留原数学、旧 GPU 回归和源码契约检查，并新增以下测试：257/512/1024 分辨率、1/8/16/64 搜索样本、接触、倾斜面、分离 blocker、薄片、边界、隐藏几何、禁用/重开摘要、纹理和 sampler 恢复；逐层检查摘要包围每个真实源深度 texel；另有 513 分辨率的 16 个固定随机场景，以及有界异步计时查询池。

百万级检查计数来自逐像素/逐 texel 比较，不是百万个独立场景。有限测试不能保证所有几何/驱动；完整 Orca GUI 编译、实际界面操作及用户显卡 FPS 未在本次本地环境运行。

最终功能提交的 GitHub Actions 运行 `36684088585` 已成功：Linux、Windows、macOS 的可移植检查及 Linux 原生 GL 任务全部通过。Linux 的 4 个 CTest 目标（数学、原 GPU 回归、新深度界、源码契约）及 ASan/UBSan 全部通过；3.3 Core 额外执行了诊断查询池测试。最终版本的所测新旧 Shader 最大可见度差仍为 0。

原始日志保存在该运行的 `pcss-opengl-results` artifact。日志包括 GL3.1、GL3.3 Core、新深度界对比、计时压力测试和 sanitizer。它们不是完整 Orca GUI 或目标显卡性能测试。

## 7. 本地受控基准：收益和额外成本

Linux Mesa25.0.7-2 llvmpipe（LLVM19.1.7），1024×768 RGBA32F 接收输出、2048 深度图、16/32 样本。3 次预热，7 组，每组 3 次绘制并等待完成，报告单次中位数。完成等待仅在显式测试基准使用。

| 接收查询场景 | 上一轮版本 | 本轮原型 |
|---|---:|---:|
| 完全阴影内部 | 30.43 ms | 3.39 ms |
| 整体 caster 范围内但局部全亮 | 10.75 ms | 3.40 ms |
| 无自遮挡的倾斜平面 | 10.69 ms | 3.31 ms |
| 几乎全是混合半影的窄条场景 | 31.81 ms | 35.54 ms |

摘要更新不是免费：简单几何且每次强制重画深度的实验中，深度更新约 1.56 ms，深度+摘要约 8.74 ms，增加约 7.18 ms。静态缓存时不重建摘要。以上是软件渲染该次数据，不是目标硬件 GPU 的开销。

因此本轮主要帮助局部全亮、全暗和光滑表面；混合半影多、深度频繁更新或几何提交本就占大头时，净收益可能较小甚至变负。提供 BOUNDS=0 与阶段计时就是为了在真实场景验证，而不是用合成加速比承诺 20 FPS 必然升到 50 FPS。

复现：

```bash
cmake -S tests/pcss -B build-pcss -DPCSS_TEST_OPENGL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-pcss --config Release --parallel
./build-pcss/pcss_depth_bounds_tests --core --benchmark
```

需具备 GLFW/GLEW/OpenGL 测试依赖，无显示环境可加 xvfb-run。只在有意测试软件渲染时设置 LIBGL_ALWAYS_SOFTWARE=1。CI 不设置受共享 runner 波动影响的固定性能失败阈值。

## 8. 拉取与真实场景验收

```bash
git fetch origin
git switch feature/pcss-editor-gcode-shadows
git pull --ff-only origin feature/pcss-editor-gcode-shadows
```

重新编译并同步运行目录的 `resources/shaders/140/pcss.glsl`，退出旧进程再启动。C++ 和 Shader 必须配套，只更新 exe 会让验证失真。

固定同一个模型、相机、窗口与显示缩放、构建类型，比较正常 PCSS 和 BOUNDS=0，记录实际 viewport 像素、显卡和引发降帧的操作。日志说明 depth、bounds、model/gcode/plate 各阶段份额后，才能确定下一步应该改变阴影几何预算、板面缓存还是接收计算分辨率。
