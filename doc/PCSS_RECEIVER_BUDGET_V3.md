# PCSS 第三轮优化：模型采样预算、默认关闭 bounds 与深度裁剪

> 日期：2026-09-30。仓库：`PILIPALA030/OrcaSlicer`。
> 分支：`feature/pcss-editor-gcode-shadows`。
> 实施基线：`ada9fc5c4f8e55b53c4f65f3bb7bd3eb02eb0169`。
> 功能提交：`b57531cccdbcc4d50984beb7cc202d14be0e58a6`。
> 本报告是后续文档提交，不改变上述已测功能代码。未合并或改写 main。

## 1. 根据实机反馈调整优先级

用户的五组诊断表明，当前编辑场景的主要额外成本是普通模型表面的 PCSS 查询，其次是阴影深度绘制；局部深度摘要没有显示出足以抵消构建成本的收益。日志中的 model 时间包括普通几何、材质和接收着色，不能全部当作阴影函数独占成本。不同运行的窗口不能当作同一帧严格相减；本轮不把历史统计或软件渲染基准换算成目标机器的帧率承诺。

本轮按三个优先级改动：默认不生成 bounds；只降低普通模型的采样预算；让阴影深度半空间裁剪在光栅化前完成，并补充缓存失效原因及可选图元计数。没有冻结阴影、隔帧更新、取消模型自阴影或用完整 STL 代替预览刀路。

## 2. 默认行为和质量取舍

| 项目 | 上一轮默认 | 本轮默认 |
|---|---:|---:|
| 局部 depth-bounds 摘要 | 开启 | 关闭，可显式开启 |
| 普通模型 blocker / PCF 样本数 | 16 / 32 | 8 / 16 |
| 打印板 blocker / PCF 样本数 | 16 / 32 | 16 / 32，不变 |
| G-code 挤出接收样本数 | 16 / 32 | 16 / 32，不变 |
| 阴影图分辨率 | 2048 | 2048，不变 |
| 完整光角直径、最大半径 | 4°、12 mm | 不变 |
| 物体、相机光方向、可见刀路变化时的更新 | 原有动态更新 | 保留 |

普通模型完整两轮查询的采样预算从 48 降至 24。无 blocker、硬阴影和其他提前返回分支并不一定执行全部预算，因此不能说每个片元恰好执行 24 次，也不能说整个程序耗时减半。

**这是可回退的质量档位，不是无损优化。** 样本较少可能增加半影颗粒、条带，以及漏掉细小 blocker 的概率。光源大小与物理半径公式没有改变，但平均遮挡距离的估计也会随采样变化，因此不能保证阴影边界逐像素相同。本轮没有实现 TAA、时域累积、半分辨率屏幕空间阴影或基于边缘的自适应采样。

为了避免只取旧圆盘前半段而缩小搜索范围，代码根据实际样本数重新生成覆盖完整圆盘的 Vogel 偏移。改变采样预算不要求重画 Shadow Map。

## 3. 配置和接口

新增 `PCSSShadowPolicy.hpp`，提供环境选项、采样预算与失效原因标志。该头文件由已有 renderer 头文件包含，不增加产品编译单元或运行依赖。

`PCSSSettings` 尾部新增：

```cpp
unsigned model_blocker_samples{8};
unsigned model_filter_samples{16};
```

它们按基础 blocker_samples/filter_samples 上限取较小值；合法范围仍为 1–64。原基础设置保留，供板面、G-code 和未识别的接收变体使用。

模型接收分类使用当前普通模型程序中实际存在的 `print_volume.type` uniform，并排除显式标记的板面接收者。实际 gouraud Shader 的分类与上传预算均有原生测试。G-code 的 gouraud_light 不符合模型标志，继续基础预算；未知材质保守回到原预算，而不是取消阴影。`PCSSReceiverScope` 的调用签名保持兼容，不要求修改 GUI 调用链。

| 环境变量 | 行为 |
|---|---|
| 未设置 `ORCA_PCSS_MODEL_QUALITY`，或设为 `balanced` | 模型使用默认 8/16 |
| `ORCA_PCSS_MODEL_QUALITY=reference` | 模型恢复基础预算，默认 16/32；板面和 G-code 不变 |
| 未设置 `ORCA_PCSS_BOUNDS`，或设为 `0` | 默认不建立/查询局部摘要 |
| `ORCA_PCSS_BOUNDS=1` | 显式选择局部摘要，便于后续在不同场景测试 |

BOUNDS=1 提供初始设置默认值，之后显式 set_settings 的 use_depth_bounds 仍有效；现有 BOUNDS=0 诊断会强制禁用摘要。环境值在设置/渲染器初始化时读取，测试时应退出旧进程并重新启动，不依赖运行中修改终端变量。

原有 FILTER=hard、RECEIVERS=plate/none、PROFILE、PROFILE_LOG 保留。性能对比前必须清除之前残留的诊断模式。

## 4. 深度绘制的具体改动

原 `pcss_depth.fs` 对每个片元计算一般裁剪平面和上下 Z 限制，再决定 discard。新版本在顶点 Shader 输出三个 `gl_ClipDistance`，让裁剪在光栅化前进行；片元 Shader 不再 discard，也不自定义深度。

三个保留条件仍为：一般平面点积非负、world.z 不低于下限、不高于上限。完整 world matrix、镜像、已有 caster LOD 与索引范围不变；FLT_MAX 哨兵表示无限制时输出常数合法距离，避免极端值插值。

这个调整为驱动提供更简单的深度-only 路径，但 **早期深度优化的实际程度依赖驱动和几何，不能保证把用户约 9 ms 的深度成本降到某个固定值**。本轮未减少 depth 几何数量，也没有把阴影 LOD 改成与颜色不同的网格。

状态保护新增保存/恢复前八个用户裁剪开关。只有检测到新深度程序的 active uniform `pcss_vertex_clipping` 时才启用三个裁剪输出；旧/测试自定义深度程序不启用这些输出。bounds reduction 和板面绘制前清除本 Pass 不使用的裁剪开关，退出时恢复调用者状态。

缓存签名额外包含深度 program ID，切换深度程序不能复用旧图。原来的 revision、光空间拟合与深度范围检查继续保留。

## 5. 新诊断字段

启用 `ORCA_PCSS_PROFILE=1` 后，每个报告窗口含：

```text
revision=receiver-budget-v3
map=2048 samples=16/32 model_samples=8/16
filter=pcss receivers=all bounds=off depth_clip=vertex
```

`model_samples` 是当前模型预算；`samples` 是板面、G-code 等使用的基础预算，不是漏改参数。

| 字段 | 含义 |
|---|---|
| `model_budget_calls` | 当前窗口识别并启用模型预算的接收作用域次数，不是像素数或早退率 |
| `cache_hits` | 复用深度图的次数 |
| `redraw_invalid` | 没有可复用的有效图，包括首次初始化、重建和显式失效 |
| `redraw_revision` | 场景 revision 改变，可包含几何、变换、裁剪、所选 LOD；不进一步猜测原因 |
| `redraw_light` | 已有效图的世界光方向发生变化 |
| `redraw_projection` | 光空间拟合矩阵或最近 caster 深度变化 |
| `redraw_program` | 深度 Shader program ID 变化 |
| `depth_primitives` / `primitive_samples` | 可选的图元生成阶段平均计数及样本数 |

一次重画可能同时命中几个原因，它们不能直接相加当重画次数。各项 CPU/GPU 均值仍是作用域执行均值，不是整帧时间。GPU 样本延迟读回，与 CPU 统计窗口可能错位。

设置 `ORCA_PCSS_PROFILE_GEOMETRY=1` 可同时采集 GL_PRIMITIVES_GENERATED；必须也开启 PROFILE。它是该深度绘制作用域的生成图元计数，不是原始模型三角形数量、像素数量或独立 LOD 标签。查询目标已被宿主占用时不打断宿主，跳过本次计数。只在结果就绪后异步读取，有界查询池不等待 GPU；不支持计时/尚未就绪时显示 NA，而不是零成本。

默认 profiling 和几何计数都关闭。几何计数本身可能扰动性能，所以日常帧率对比可以关闭，定位深度几何成本时再开启。产品路径没有新增 glFinish 或像素回读。

## 6. 已完成的验证与限制

功能提交的 GitHub Actions：

https://github.com/PILIPALA030/OrcaSlicer/actions/runs/36703173408

四个任务全部成功：Linux、Windows、macOS 的 CPU/源码契约检查，Linux 的原生 OpenGL 检查。Linux 的六个 CTest 目标均通过，包含新增 policy 和 receiver_budget；GL 3.3 Core 额外运行原 GPU、bounds 和新预算测试；ASan/UBSan 通过，未启用 LeakSanitizer。

| 测试 | 最终提交结果 |
|---|---|
| 已有 PCSS 数学 | 2,279 个断言通过 |
| 新预算与环境 policy | 8,197 个检查通过 |
| 已有实际 OpenGL 回归 | 104 个断言通过 |
| 新接收预算与深度裁剪 | 8,331 个检查通过 |
| 已有源码接入契约 | 14 项通过 |
| 可选 bounds 对比与包围性 | 原有测试继续通过，不因默认关闭而跳过其验证 |

计数包含参数组合和逐像素检查，不代表同样数量的独立模型或场景。实际编译的是提交版本的 PCSSShadowRenderer.cpp 以及对应 GLSL。新测试检查真实模型和 G-code 程序的预算、完整采样圆盘、缓存复用、七组深度裁剪对比、镜像、哨兵值、剪裁状态恢复、bounds 显式开启、宿主查询不被打断和异步计数。

七组深度对比在本次软件 GL 中差异像素数均为 0；几何裁剪和片元丢弃仍可能在轮廓光栅化边界产生有限差异，测试对此保留边界容差。

**模型预算的画面并不与 16/32 等价。** 单个合成接收条带测试的 8/16 对 16/32 可见度平均绝对差为 0.0127563，最大差为 0.125。这只是该测试的数据，不是所有模型的误差上限，也不是颜色变化百分比。接触硬化、间距增大时半影变宽、内部遮挡及外部可见的检查通过。

最终 CI 使用 Mesa llvmpipe 软件 OpenGL，没有完成整个 Orca GUI 的编译、交互运行或用户 AMD 显卡上的性能复验。本轮不把模块检查说成全应用验收，不承诺恢复到 50 FPS。旧测试保留，CI 仍只读；本次源码通过 GitHub 接口直接提交，没有新增用 CI 写源码的临时工作流。

## 7. 拉取、运行和最小 A/B 对照

保存本地改动后：

```bash
git fetch origin
git switch feature/pcss-editor-gcode-shadows
git pull --ff-only origin feature/pcss-editor-gcode-shadows
```

使用原来的 Orca 构建配置重新编译，**同步运行目录的 pcss_depth.vs 和 pcss_depth.fs；整个 resources/shaders/140 目录一起更新更不容易漏文件**。本轮公共 pcss.glsl 没有改变，但运行目录仍应来自同一个分支版本。退出旧进程后重新启动。

### A. 本轮默认：bounds 关闭、模型 8/16

先完全退出 Orca，在启动程序的 PowerShell 中完整执行：

```powershell
Remove-Item Env:ORCA_PCSS_BOUNDS, Env:ORCA_PCSS_FILTER, Env:ORCA_PCSS_RECEIVERS, Env:ORCA_PCSS_MODEL_QUALITY, Env:ORCA_PCSS_PROFILE_GEOMETRY -ErrorAction SilentlyContinue
$env:ORCA_PCSS_PROFILE = "1"
$env:ORCA_PCSS_PROFILE_LOG = Join-Path $env:TEMP ("orca-pcss-v3-balanced-{0}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
Write-Host "日志：$env:ORCA_PCSS_PROFILE_LOG"
```

然后从这个终端启动新编译的实际 Orca exe，保持偏好中的阴影开关开启。用同一个导致降帧的场景预热后旋转或移动约 10 秒，记录 FPS。退出后查看：

```powershell
notepad $env:ORCA_PCSS_PROFILE_LOG
```

预期正常新记录为 `receiver-budget-v3`、`model_samples=8/16`、`bounds=off`、`depth_clip=vertex`。普通模型确实接收时 `model_budget_calls` 应增加；没有可接收模型或落在特殊未适配路径时不能要求它为正。

### B. 对照：bounds 仍关闭、模型恢复 16/32

先完全退出程序，再在同一个终端执行完整设置：

```powershell
Remove-Item Env:ORCA_PCSS_BOUNDS, Env:ORCA_PCSS_FILTER, Env:ORCA_PCSS_RECEIVERS, Env:ORCA_PCSS_PROFILE_GEOMETRY -ErrorAction SilentlyContinue
$env:ORCA_PCSS_MODEL_QUALITY = "reference"
$env:ORCA_PCSS_PROFILE = "1"
$env:ORCA_PCSS_PROFILE_LOG = Join-Path $env:TEMP ("orca-pcss-v3-reference-{0}.log" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
Write-Host "日志：$env:ORCA_PCSS_PROFILE_LOG"
```

从该终端重新启动，重复相同操作。这组的 `model_samples=16/32`。两组采用同一个新深度路径，主要隔离模型采样预算的收益，不是完整复现旧版所有设置。

第一轮只需要这两组，不必重复之前五组。同时观察半影噪点、细小遮挡物、接触处阴影和物体变换是否可以接受；不要只比较 FPS。主光方向、窗口尺寸、显示缩放、场景和构建类型保持一致。

### 可选：针对深度成本追加计数

在单独一次启动前：

```powershell
$env:ORCA_PCSS_PROFILE_GEOMETRY = "1"
```

用于查看生成图元量和重画原因；这组不与无几何计数的结果直接当作严格性能对照。设置另一日志文件避免混入前两组。恢复默认模型质量和关闭计数：

```powershell
Remove-Item Env:ORCA_PCSS_MODEL_QUALITY, Env:ORCA_PCSS_PROFILE_GEOMETRY -ErrorAction SilentlyContinue
```

Linux 对应使用 `export ORCA_PCSS_PROFILE=1`、`export ORCA_PCSS_PROFILE_LOG=/tmp/orca-pcss-v3-balanced.log`，以及按需要 `export ORCA_PCSS_MODEL_QUALITY=reference`；用 unset 清除其他测试变量，然后从同一个终端启动。

## 8. 后续性能判断

先看本轮默认是否降低 model GPU 时间和提高同操作下的帧率，再看画面是否可接受。若 model_samples 已是 8/16 且 model_budget_calls 有效，但模型阶段仍明显偏高，再考虑接收计算分辨率或更复杂的重建路径，不能将未实现方案描述为已完成。

depth 仍重时，用失效原因区分必要动态更新与异常重画；图元计数可帮助判断几何规模，但还需与宿主选择的 LOD/对象集合结合，不能单凭计数推断具体对象。保持阴影实时更新正确性优先，不为帧率默默冻结或丢弃可见遮挡物。

## 9. 代码入口

- [默认策略与环境选项](../src/slic3r/GUI/PCSSShadowPolicy.hpp)
- [设置与接收接口](../src/slic3r/GUI/PCSSShadowRenderer.hpp)
- [预算上传、深度状态及缓存](../src/slic3r/GUI/PCSSShadowRenderer.cpp)
- [可选 bounds 与诊断](../src/slic3r/GUI/PCSSShadowAcceleration.hpp)
- [新深度顶点程序](../resources/shaders/140/pcss_depth.vs)
- [原生预算/裁剪回归](../tests/pcss/test_receiver_budget.cpp)
- [CPU policy 测试](../tests/pcss/test_policy.cpp)
