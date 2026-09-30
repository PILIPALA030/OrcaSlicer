# PCSS 帧率回退：原因、修复与验证

> 本次分析基线：`0b53670909f8b5a25992ea85d399f2cb14a7a4cb`。
> 功能修复提交：`a77af90775746fbd715725265078d9ab964fea44`。
> 工作分支：`feature/pcss-editor-gcode-shadows`。没有修改 main。
>
> 结论分为源码事实、受控测试和待实机验证三层。没有用户本机 GPU capture、开关前后帧率或完整场景数据，不能把下面某一个开销宣称为所有机器上唯一的瓶颈。

## 1. 最明确的回退：启用 PCSS 强制绕过颜色 LOD

上一版 `GLVolume::simple_render()` 有以下条件：

```cpp
if (!picking && (shader == nullptr || shader->get_name() != "gouraud_pcss")) {
    // Small / Middle / High selection
} else {
    // Full-resolution model
}
```

因此使用 `gouraud_pcss` 的普通模型不再使用原有 Small/Middle 网格，而是画完整网格。阴影深度 Pass 同时直接绘制 `volume->model`。对于本来依靠 LOD 控制三角形数量的场景，相当于同时增加了一遍高精度深度绘制、又让正常颜色绘制退回高精度。

这是上一版为保持深度与颜色几何一致性引入的过于保守处理，不是 PCSS 必须禁用 LOD。数学与小场景测试通过也不能发现真实大模型上的这一性能回退。

### 本次修复

新增 `GLVolume::render_model(bool allow_lod = true)`，只返回当前实际可用网格，不在取网格时启动简化、切换层级或接管后台数据。

新增 `GLVolumeCollection::prepare_pcss_lod(const Camera&)`，在编辑页生成深度快照前进行原有 LOD 评估和主线程接管。颜色 Pass 使用 `gouraud_pcss` 时不再重新评估或接管，从而保持：

```text
本帧评估 LOD / 接管已经完成的简化网格
                ↓
      确定实际 Small / Middle / High
                ↓
      同一网格用于深度和颜色绘制
```

深度快照保存本帧借用的网格、世界矩阵和索引范围；缓存签名新增实际 LOD 身份及其顶点/索引数量。简化网格晚到或层级切换时重画深度，而不是复用旧层级的阴影图。

不能只删除原来的 `gouraud_pcss` 判断、让颜色画 Small 而深度仍画 High，否则会引入错误的自遮挡。当前实现还保留以下边界：

- picking 仍使用基础高精度网格。
- 部分索引范围属于基础网格，不能直接套到简化网格。
- 已涂色表面仍走原来的分色几何绘制；其投射深度选择基础几何，不擅自改造成低精度分色网格。
- 实际选中网格的包围盒也参与光图覆盖，避免简化容差让深度被裁掉。
- 离屏对象没有简单按观察相机剔除，仍可投射可见阴影。

## 2. 接收 Shader 中的重复计算和无效采样

旧 `pcss.glsl` 的 `pcss_disk(i, count)` 在每次查询的两轮循环中求 `sqrt`、`sin` 和 `cos`。默认情况下，受遮挡的查询最多执行 16 次 blocker 查询和 32 次过滤查询。即使最终完全没有 blocker，也要先完成搜索。实际驱动可能优化部分表达式，因此不能只数源码中的三角函数就推导出确定的 GPU 加速倍数。

本次不改变采样分布，而是在 CPU 端根据样本数预计算相同的 Vogel 圆盘偏移，以 uniform 数组提供给 Shader。只有样本数量改变时重新计算数组，像素循环改为直接读取偏移。

新增光空间 caster UV 包围范围。只有在接收点的**整个搜索范围**与所有可能的 caster texel 完全不相交时，才直接返回可见。边界额外包含两个 texel 的光栅化/最近邻量化保护。

这不是用中心一个深度值、四个随机探针，或“看起来应该没有阴影”来提前结束。用于提前判断的是保守几何范围，所以原本的采样算法在这些位置也不可能找到 blocker。

这项优化对空白板面较多、模型占屏小的场景更有帮助；如果接收区域大部分都在 caster 范围内，则仍要执行真实 PCSS 两轮采样，收益自然更小。

## 3. 先做必要导数，再排除无需阴影查询的片元

将公共函数拆成：

```glsl
PCSSReceiver pcss_prepare_receiver(vec3 world_position);
float pcss_visibility_prepared(PCSSReceiver receiver);
```

第一步只计算坐标和接收面导数，仍在可能分歧的丢弃/分支前执行。第二步才采样阴影纹理。

普通模型先处理几何裁剪，已被丢弃的片元不再跑 blocker 和 PCF。普通模型与 G-code 挤出接收者在主光漫反射、高光贡献均为零时，也不执行无意义的主光阴影查询。环境光、补光、材质颜色和 alpha 的处理保持原来的分工。

打印板继续使用公共 `pcss_visibility()` 包装函数，仍是真实 PCSS，而不是屏幕图像模糊。

## 4. 缓存与 GL 状态

现有深度缓存原本就会比较 revision、光矩阵和深度范围，并非无条件每帧重画。编辑页的主光绑定相机方向，旋转相机时光方向确实改变，需要更新深度；G-code 层范围变化时实际可见几何改变，也需要更新。这些正确的动态更新没有被关闭。

本次只缓存同一 GL 上下文中的纹理尺寸/纹理单元能力查询；销毁或放弃旧上下文时清空能力缓存。没有把状态恢复删掉，也没有在运行路径加入 glFinish、像素回读或阻塞 GPU 计时。

缓存命中时也更新接收用的 caster 范围元数据，避免拟合域不变但包围范围改变时使用过时的快速排除范围。

## 5. 没有降低的设置，以及仍保留的功能

| 项目 | 修复前 | 修复后 |
|---|---:|---:|
| Shadow Map 默认分辨率 | 2048 | 2048 |
| Blocker samples | 16 | 16 |
| Filter samples | 32 | 32 |
| 完整光角直径 | 4° | 4° |
| 最大滤波半径 | 12 mm | 12 mm |
| 编辑模型/板面接收 | 启用时工作 | 保留 |
| 可见 G-code 范围与端面 | 真实几何 | 保留 |
| 持久化偏好开关 | enable_shadow_map | 保留 |

LOD 修复意味着不再强制使用 High，而是恢复相机原有层级策略。因此精细几何的阴影轮廓会随实际显示层级变化；深度与显示表面保持一致。这是有意恢复原有几何预算，不是承诺画面每个像素与强制 High 版本完全相同。

## 6. 首次发布前已完成的验证

远程验证运行：

https://github.com/PILIPALA030/OrcaSlicer/actions/runs/36667898710

| 检查 | 结果 |
|---|---|
| `git diff --check`、改动区域格式化 | 通过 |
| 实际 `PCSSShadowRenderer.cpp`，C++17，warnings-as-errors | 通过 |
| 数学检查 | 2279 个断言通过，含 1–64 样本圆盘及 caster 范围 |
| OpenGL 3.1 / GLSL 140 | 104 个断言通过 |
| OpenGL 3.3 Core | 104 个断言通过 |
| 源码接入契约 | 14 个测试通过，新增双 Pass 共享 LOD 和热循环检查 |
| 新旧公共 Shader 的可见度 A/B | 所测五组读回 mean/max 差值均为 0 |

A/B 使用仓库中固定的旧 Shader 测试样本 `tests/pcss/reference/pcss_before_performance.glsl`。它只用于测试，不加入产品 Shader 注册。覆盖远近 blocker、接触、不同样本数和倾斜接收面等情况，但有限场景的零差异不等于对所有几何/驱动的数学证明。

源码契约测试确认函数接法和更新关系，不能代替完整 GUI 的实际操作测试。**本次没有重新完成整个 Orca GUI 的编译、链接、运行，也没有在用户本机显卡测量 FPS。** 上一轮完整工程配置曾被 OpenVDB/IlmBase 依赖问题阻塞；本轮没有为此改动依赖目录。

## 7. 受控接收阶段基准：不是整机 FPS

同一远程 runner：Mesa 25.2.8 llvmpipe 软件渲染；输出 1024×768 RGBA32F；同一张 2048 深度图、默认 16/32 样本。3 次预热；每组批量画 3 次，共 5 组，报告每次绘制耗时的中位数。

| 场景 | 旧公共 Shader | 新公共 Shader | 此次测量的比值 |
|---|---:|---:|---:|
| 小遮挡物、大接收面，300×220 mm 查询区域 | 23.0887 ms | 1.3235 ms | 17.45× |
| 密集阴影区域，10×4 mm 查询区域 | 66.8787 ms | 49.7841 ms | 1.34× |

这仅比较公共阴影接收查询，**不包括 Orca 大模型的颜色/深度几何提交、完整材质、UI 和 G-code 范围重建成本**。软件渲染、共享 runner 的调度与缓存都会影响数据，不能推导用户显卡提高 17 倍，也不能拿该 ms 的倒数当作 Orca 帧率。LOD 修复的收益需要在用户的大模型场景另外验证。

`glFinish()` 只存在于显式启用的测试基准中，用于避免把异步提交时间当完成时间；没有放进产品渲染器或 GUI。

复现 Linux 独立基准：

```bash
cmake -S tests/pcss -B build-pcss-perf -DPCSS_TEST_OPENGL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-pcss-perf --parallel
ORCA_PCSS_BENCHMARK=1 ./build-pcss-perf/pcss_gl_tests --core
```

无显示环境可加 `xvfb-run -a`。只在明确要比较软件渲染时设置 `LIBGL_ALWAYS_SOFTWARE=1`。这个目标使用 GLFW/GLEW 作为测试宿主，并不替代 Orca 正式构建。

## 8. 本机验收建议

```bash
git fetch origin
git switch feature/pcss-editor-gcode-shadows
git pull --ff-only
```

使用原本可编译 Orca 的依赖和配置重新编译，确保新的 Shader 资源同时复制到运行目录；只换二进制、继续读取旧的 pcss.glsl 会让验证失真。

在同一个模型、相机、窗口大小、显示缩放和构建类型下对比：关闭阴影、旧版本开启、新版本开启。分别观察持续旋转视角、平移/缩放物体，以及 G-code 上/下层与横向顺序滑块。静止视图可能采用按需重绘，不适合仅看空闲 FPS。

同时检查模型缩放造成 LOD 切换时是否有错误自遮挡、晚到的简化网格是否留下旧阴影、涂色/镜像/部分范围和偏好开关是否仍正常。记录显卡、视口像素尺寸、模型/刀路规模和上述操作下的帧时间。

## 9. 分支操作说明

修复只落到既有 feature 分支，未修改 main。此次需要给大体积 GUI 源文件应用经过校验的局部改动，因此使用一个限定分支/起点的一次性验证入口，在测试通过后进行 fast-forward 提交；该入口在报告提交中删除。常规 `pcss-validation.yml` 仍只有 contents:read，不负责修改源码。

常规 CI 的 PR 路径过滤同步覆盖 GUI 和全部 140 Shader，避免只改 3DScene、GLCanvas3D 或 gouraud 而漏掉 PCSS 回归检查。计时基准不设置易受共享 runner 波动影响的自动失败阈值。
