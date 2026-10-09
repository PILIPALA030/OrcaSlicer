# PCSS / 全帧渲染性能诊断

本次修改只增加可选的测量工具，不继续改变正式 PCSS 的采样预算、简化预算和画质。
基线为 `0511b96f1f172ae2071f23261bc7f05a8eeb71fc`。

用户报告 upstream main / 本分支关闭 PCSS / 开启 PCSS 分别约为 45 / 29 / 23 FPS。
倒数约为 22.22 / 34.48 / 43.48 ms，但最高 FPS 不能当作配对的稳态测量。
首先要区分关闭时的回退和开启后的额外开销，并确认 45 FPS 二进制对应的准确源码提交。
当前 upstream main 与本分支未必是只相差阴影开关的同一套渲染器。

## 1. 编译

在现有、能正常编译的构建目录中保留原来的依赖、编译器和工具链配置，增加：

```sh
cmake -S . -B build -DSLIC3R_RENDER_PROFILE=ON
cmake --build build --config Release --parallel
```

把 `build` 换成实际构建目录。单配置生成器还需 `-DCMAKE_BUILD_TYPE=Release`。
配置阶段需要 Python 3.8+，运行编译后的程序不需要 Python。
必须重新编译 C++；仅替换着色器资源不会产生这些日志。

默认 `SLIC3R_RENDER_PROFILE=OFF`：完全保留原有编译单元，不引入运行时诊断依赖。
开启后，CMake 在构建目录的 `src/slic3r/render-profile` 中生成 GLCanvas3D.cpp、
GLModel.cpp、3DScene.cpp、GLShader.cpp 的诊断副本，以及 ShadowMeshProxy.hpp 的副本。
原始源码和资源着色器不被覆盖。仅替换该目标的四个编译输入。

生成器对必需插入位置使用精确匹配；源码变化导致位置缺失或重复时，配置明确失败，
不会静默丢掉测量点，也不会部分修改源码。生成的 `manifest.json` 包含原始源码
SHA-256、源码版本、已安装的插桩点和跳过的可选插桩点。
重新配置 OFF 即可恢复原始编译输入。

## 2. 运行：先正常开关，再做两个对照

运行时还必须设置 `ORCA_RENDER_PROFILE=1`，否则诊断默认不启用。
环境变量每个进程只读取一次；每换一种模式，先关闭所有现有 Orca 实例。
单实例转发到旧进程不会让旧进程获得新的环境变量。

Windows PowerShell，在仓库根目录运行，Exe 指向本次构建的实际可执行文件：

```powershell
.\tools\render_profile\capture.ps1 -Exe 'D:\your-build\actual-orca.exe' -Mode normal
.\tools\render_profile\capture.ps1 -Exe 'D:\your-build\actual-orca.exe' -Mode depth_only
.\tools\render_profile\capture.ps1 -Exe 'D:\your-build\actual-orca.exe' -Mode static_off
```

逐条运行，每次退出程序后再执行下一条。输出目录默认 `%TEMP%\orca-render-profile`，
每次生成独立的时间戳文件。脚本不自动修改偏好设置、不关闭现有程序、不导入模型，
只在当前进程中设置环境变量、启动程序、等待退出，然后恢复调用者原来的环境变量。
如果未产生日志，脚本会提示检查实际运行的二进制、编译开关及旧实例。
不要为了脚本而修改机器级 PowerShell 安全策略；也可以手动设置下面的进程环境变量。

Linux / macOS 示例（macOS 使用 app bundle 内的实际二进制）：

```sh
ORCA_RENDER_PROFILE=1 ORCA_RENDER_PROFILE_MODE=normal \
ORCA_RENDER_PROFILE_OUT=/tmp/orca-normal.log /path/to/the/actual/executable
```

换模式时更换日志文件名。没有设置 OUT 时，日志写入应用数据目录的 `pcss-profile.log`，
常规应用日志也会打印其路径。文件以追加方式打开；不能打开时，打印错误并退到 stderr。

| 模式 | 偏好设置阴影开关 | 目的 |
| --- | --- | --- |
| normal | 关闭 → 开启 → 再关闭 | 同一进程内测实际开关；不同状态分别汇总 |
| depth_only | 开启 | 保留阴影深度生成和几何提交，强制关闭接收面的软阴影采样 |
| static_off | 关闭 | 不生成阴影图；加载六个接收面片元着色器时将阴影开关替换为编译期 false |

`static_off` 让编译器/链接器能够消除不可达 PCSS 和未使用的阴影 varying，
用于对比 uniform=false 与编译期移除的差异。它仍保留原有 CPU uniform 重置/绑定流程，
不等同于恢复到 upstream main，也不是新增的正式渲染模式。
`depth_only` 保留深度通道，只禁用接收面 uniform，同样是诊断用的对照。

每个阶段连续旋转同一个模型 20–30 秒；有缩放/平移卡顿时再单独录制对应阶段。
保持相同物理视口尺寸、相机距离、模型选择状态、MSAA、VSync/限帧设置、供电模式和
统计窗口状态。不要把静止视图的事件驱动重绘频率当作满负荷 FPS。
主视图 LOD 和阴影代理准备完成前后的记录分开分析；不要丢掉准备阶段的日志。
Debug 构建和 Release 不可直接比较，本分支非 NDEBUG 配置有逐调用 GL 错误检查。

请提供三个日志、生成的 manifest.json，以及实际二进制的源码提交信息：

```sh
git rev-parse HEAD
git rev-parse upstream/main
```

说明 upstream 指向哪个仓库，并确认它确实对应测试出 45 FPS 的二进制，而不只是后来
更新的本地引用。不需要分享访问令牌或带凭据的 remote URL。
日志包含本地资源目录和 GPU/驱动字符串，不记录模型内容。

## 3. 日志内容与解读

每行以 `[PCSS_PROFILE]` 开头，后面为 JSON。
每个 canvas 的 info 记录源码版本及摘要、NDEBUG 状态、GL vendor/renderer/version、
MSAA 样本数、GPU 计时支持情况、采样步长、模式、标签和实际资源目录。

默认每 4 帧采样一帧，在实际重绘期间大约每 3 秒汇总一次，或者累积到 240 个采样帧
时汇总。销毁 canvas 时输出剩余结果。空闲时不为了日志而强行触发重绘。
每个阶段分别给出 CPU 和 GPU 的 avg、p50、p95、n，单位毫秒。

| 阶段 | 含义 |
| --- | --- |
| frame CPU | 从 render 入口到 SwapBuffers 返回，包含测量本身的相关开销 |
| frame GPU | 有效 GL 上下文后的测量点，到 SwapBuffers 之前的命令流区间 |
| prelude / prepare | 上下文及初始化前段 CPU 时间；相机、投影等准备 |
| picking / picking_rectangle / mouse_depth_readback | 拾取、框选、鼠标世界坐标深度查询 |
| shadow_total CPU | RenderShadowMap 整体，包括关闭时的提前返回 |
| shadow_enabled_total | 开启后的阴影工作，包含下面的子阶段 |
| shadow_setup / shadow_bounds / shadow_resources | 光空间设置、包围盒、资源准备和清屏等 |
| shadow_geometry / shadow_restore | 投影模型遍历绘制、随后状态恢复 |
| shadow_uniforms / shadow_uniform_reset | CPU uniform 设置及重置 |
| objects_opaque / objects_transparent | 模型接收面渲染整体，不是纯 PCSS 指令时间 |
| bed / plate_list / background | 热床、盘列表、背景 |
| selection_prepare / selection_mask / selection_outline_textures / selection_gaussian / selection_composite / selection_stencil | 本分支存在的选中高亮路径 |
| selection / sequential_clearance / gizmos / overlays / imgui | 其它场景及界面绘制 |
| swap | 仅 CPU：SwapBuffers 内的耗时，可能包含呈现节奏或驱动等待 |
| model_upload / proxy_get | CPU 数据上传、代理结果接收/构建等 |
| profiler_poll_report | 查询已完成结果及定期日志输出的 CPU 开销 |

分组不混合不同 canvas、类型、视口、阴影请求/就绪状态、选择/拾取状态或 volume 数量。
开关后可能同时输出多个分组，请依据标签而不只是日志顺序。
同名 scope 在一个采样帧内多次出现时先求和，再跨帧聚合；n 是调用过该阶段的采样帧数。
缺失阶段不伪造为 0 ms。每次汇总的 p50/p95 不是可直接平均得到整段录像分位数的数据。

**父阶段包含子阶段，不能把所有行相加，也不能把 CPU 与 GPU 时间相加。**
GPU timestamp 区间可能包含流水线空闲、CPU 供给不足和边界内的其它命令，
不是只计算着色器 ALU 的独占时间。cadence_ms 是两次渲染完成之间的间隔，仍可能包含
事件循环空闲；首个间隔及超过 250 ms 的间隔排除并计数，仅在连续重绘时参考其 FPS。

计数器在 GLModel 的实际 DrawElements / DrawElementsInstanced 位置统计提交的
三角形、绘制次数，包含实例数乘数，按阶段和 shader 分开。它不是可见三角形数，
也不是硬件实际顶点着色器 invocation 数；绕过 GLModel 的其它绘制路径不在其中。
BufferData 记录实际字节数，包含 uchar/ushort 索引转换后的大小。

volume_source_triangles 和 volume_lod 等是尝试绘制的源数据计数，不能代替实际提交数。
shadow_omitted_calls、shadow_original_calls、shadow_lod_or_proxy_calls 可用于确认
大模型是否仍走原模型或者是否尚无可用阴影代理。proxy_state_N 为代理状态计数。

后台 proxy_job 单独记录真实网格拷贝、QEM 的 CPU 墙钟时间，以及输入/输出三角形数。
这些可能与渲染重叠，不应加到 GPU 帧时间里。负数表示相应阶段未完成，例如取消。

## 4. 不阻塞等待 GPU 的计时协议

使用 GL_TIMESTAMP（OpenGL 3.3 或 ARB_timer_query）支持嵌套边界，避免非法嵌套
TIME_ELAPSED 查询。每个 canvas 有 12 个槽位，每槽最多 128 个 timestamp 和
128 个 CPU scope。至少延后两帧，先检查最后一个查询的 GL_QUERY_RESULT_AVAILABLE，
确认完成后才读本组结果。同类型前序查询的可用性由 OpenGL 规范保证。

不添加 glFinish、强制 glFlush、忙等或读取尚未完成的 QUERY_RESULT。
槽位占满时只保留 CPU 记录并增加 gpu_ring_full_skips，不覆盖待完成的结果。
计数器位宽不支持/为 0 时缺省 GPU 数据，不把无效 0 当成耗时。
短位宽进行回绕处理，可能跨多个周期的长区间丢弃并记录计数。
GPU 对象仅在所属 canvas 上下文有效时释放；工作线程和静态析构器不调用 GL。

计时本身仍有成本。可用 ORCA_RENDER_PROFILE_STRIDE=16 与默认 4 对比扰动，
或用 ORCA_RENDER_PROFILE_CPU_ONLY=1 暂停 GPU 查询。脚本对应 -Stride 和 -CpuOnly。
这些选项不改变 PCSS 质量。参考：Khronos ARB_timer_query 规范，
https://registry.khronos.org/OpenGL/extensions/ARB/ARB_timer_query.txt 。

一个片元着色器内部 blocker search 和最终 PCF 的逐指令耗时，不能用包围整个
DrawCall 的 timestamp 直接拆出来。本工具利用对照模式区分较大的开销来源，
不会把 CPU 提交耗时误报成 GPU 执行耗时。

## 5. 根据结果决定优化方向

static_off 明显恢复，而 normal 关闭仍慢：优先评估正式无阴影 shader variant 和
阴影 varying/寄存器相关开销；不要只继续降低开启时的采样数。
两者都慢：检查实际提交几何、拾取/高亮额外绘制、上传、编译/运行配置和基线分支差异。
depth_only 已经很慢：优先定位深度准备/几何/上传，随后考虑正确失效的阴影图缓存。
depth_only 较快而 normal 开启模型/热床 GPU 时间上涨：优先研究比较采样器、采样预算
或有明确分辨率预算的阴影 resolve。SwapBuffers CPU 时间高本身不能证明阴影 shader 慢。

## 6. 测试范围

```sh
python3 tests/rendering/test_render_profile.py
python3 tests/rendering/test_render_profile.py --sanitize
```

五项 Python 测试覆盖 scope 识别、幂等/不修改原文件、匹配失败时不生成部分输出、
重复函数拒绝，以及实际 CMake OFF/ON/OFF 小工程的目标源文件恢复。
C++17 ASan/UBSan 测试编译真实 RenderProfile.hpp，GL 和辅助库使用可控测试替身，
覆盖关闭、无计时支持、零位宽、CPU-only、延迟结果/槽位饱和、回绕、嵌套/标记溢出、
上下文归属释放、JSON 和几何归属计数。这些检查已通过；替身时钟不是 GPU 性能数据。

尚未完成整个 Orca C++ 应用构建、原生 Windows 启动脚本执行、原始模型与目标显卡的
性能验收。此提交是为了获取这些实机证据，不是宣称 45/29/23 FPS 回退已经修复。
