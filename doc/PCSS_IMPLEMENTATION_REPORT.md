# Orca PCSS：实施结果、真实接口与验收记录

> 日期：2026-09-21。本文是实施记录，与先提交的 [总体技术方案](PCSS_INTEGRATION_PLAN.md) 配套阅读。
>
> **当前状态：三个目标的代码接入和远程提交已完成；独立编译的实际阴影渲染器及自动化检查通过。完整 Orca 的配置被依赖问题阻塞，尚未完成整个 GUI 的编译、运行和目标显卡交互验收。** 不把测试程序中的对象变换等同于已经在 Orca 窗口里拖动过模型。

## 1. 分支与交付范围

仓库：`PILIPALA030/OrcaSlicer`。

- 起点为远程 `main` 的 `b3417d7a66d76b4de392b84894426d376f020a6b`。
- 工作分支为 `feature/pcss-editor-gcode-shadows`。
- 功能和验证代码已提交；没有合并或强制更新 `main`。
- 本报告所核查的实现与 CI 版本为 `ede8df367fa3e1594184783a49e764a3defa512a`。本报告自身是后续纯文档提交，不改变被测功能代码。

| 用户目标 | 已接入的实际调用链 | 尚需实际 GUI 验收 |
|---|---|---|
| 编辑页移动、缩放、旋转时动态阴影 | Canvas 在颜色绘制前收集实体模型、完整世界矩阵和缓存签名；更新深度图；板面及普通不透明模型查询 PCSS | 在完整应用中拖动、镜像、增删、撤销重做，检查连续更新与画面一致性 |
| G-code 层高滑块动态阴影 | 提前消费待处理的层范围与顺序范围变化；使用可见挤出路径的 VBO/IBO、字节偏移、索引数量和顺序端面绘制深度 | 完整预览 UI 的上/下限滑块、单层、横向进度、角色隐藏、纯 G-code 导入 |
| 偏好设置开关 | `enable_shadow_map` 持久保存；同时请求两个 Canvas 刷新；关闭分支跳过阴影更新并释放该 Canvas 的资源 | 完整应用中即时切换、重启保留、确认不触发切片及页签切换无残留 |

这里不是只完成“模型投影到板面”的 M1：普通模型与挤出路径的主光接收 Shader，以及预览范围和偏好设置均已有实现。

## 2. 如何取得代码、如何开启

在已有本地仓库且工作区没有未保存改动的情况下：

```bash
git fetch origin

git switch --track origin/feature/pcss-editor-gcode-shadows
```

上面的 `--track` 用于本地尚未存在这个分支的情况。本地已经有该分支时，改用：

```bash
git switch feature/pcss-editor-gcode-shadows
git pull --ff-only origin feature/pcss-editor-gcode-shadows
```

使用项目原有、已配置好依赖的构建流程构建完整 Orca。不要把独立测试的 `tests/pcss` 构建当成完整切片器安装包，也不要直接用任意系统库替换既有构建依赖。

功能入口：**偏好设置 → 通用 → 启用柔和阴影（PCSS）**；英文界面为 **Enable soft shadows (PCSS)**。

默认关闭。开启后应在编辑页和 G-code 预览页生效，配置保存在 AppConfig，不是 PrintConfig/制造参数。切换回旧分支时这个额外显示配置项不要求修改模型文件。

### 默认算法参数

| 参数 | 当前值 | 含义 |
|---|---:|---|
| `resolution` | 2048 | 方形深度图请求分辨率；受硬件上限限制 |
| `blocker_samples` | 16 | 每次查询的 blocker 搜索样本数 |
| `filter_samples` | 32 | 最终比较过滤样本数 |
| `angular_diameter_deg` | 4.0 | 方向光完整角直径，计算时取一半 |
| `bias_mm` | 0.02 | 毫米制深度比较偏移 |
| `max_radius_mm` | 12.0 | 最大滤波圆盘半径，不是完整半影宽度 |
| `plate_strength` | 0.55 | 板面展示用阴影覆盖强度 |

本次偏好界面只有总开关，没有把这些调试参数都加入界面。4° 是展示默认值，不是太阳参数标定。

## 3. 已提交的英文 commit

| 提交 | Commit message | 内容 |
|---|---|---|
| `4397f422` | `docs: define the PCSS editor and G-code integration plan` | 先提交总技术方案 |
| `6a203fd9` | `feat(rendering): add the PCSS shadow renderer and shader pipeline` | 数学、深度图、PCSS、板面与材质变体 |
| `2db6f7ef` | `feat(preview): expose visible G-code extrusion ranges to the shadow pass` | 真实可见刀路深度与 revision |
| `d48ffb01` | `feat(editor): integrate dynamic model and preview shadows into the canvas` | 两种视图的调度、对象变换、滑块和生命周期 |
| `45556f7b` | `feat(preferences): add a persistent localized soft shadow toggle` | 偏好设置、配置持久化与中文字符串 |
| `8419b0d4` | `test(rendering): cover PCSS math, GPU readback and integration contracts` | CPU、实际 OpenGL、源码契约与手工验收清单 |
| `ede8df36` | `ci: retain cross-platform PCSS checks and remove transfer helpers` | 跨平台检查；移除临时传输工作流；UTF-8 读取修正 |

历史中还有四个英文 `ci:` 辅助提交，用于受限环境下取得源码/测试输入、校验传输和应用已审阅的补丁。**这些临时工作流已从分支最新文件树移除**；保留下来的是只读的 `pcss-validation.yml`。没有把传输脚本变成应用运行或编译依赖，也没有改写历史来隐藏它们。

## 4. 实际新增接口：以此替代方案中的草案签名

核心头文件是 `src/slic3r/GUI/PCSSShadowRenderer.hpp`；不依赖 Demo 的窗口库或 `Program` 类型。命名空间为 `Slic3r::GUI`。

### 4.1 数据类型

```cpp
struct PCSSSettings
{
    unsigned resolution{2048};
    unsigned blocker_samples{16};
    unsigned filter_samples{32};
    float angular_diameter_deg{4.0f};
    float bias_mm{0.02f};
    float max_radius_mm{12.0f};
    float plate_strength{0.55f};
};

struct PCSSFrameInput
{
    pcss::Bounds casters;
    pcss::Bounds receivers;
    pcss::Vec3 to_light{0, 0, 1};
    std::uint64_t revision{0};
};
```

`to_light` 是世界空间中“表面指向光”的方向。`casters` 与 `receivers` 使用世界毫米坐标。`revision` 包含影响深度内容的业务版本；投影矩阵和最近 caster 深度也参与缓存比较。

总开关由宿主 AppConfig 控制，不重复放进 `PCSSSettings`。数学类型是与 GUI 解耦的 `pcss::Bounds/Vec3/Projection`，不是旧方案草案中直接依赖 Eigen 的输入。

### 4.2 渲染器公共方法

| 实际接口 | 行为和前提 |
|---|---|
| `bool set_settings(const PCSSSettings&)` | 检查范围与非有限值；无效设置返回 false；不把任意参数无条件送进 Shader |
| `const PCSSSettings& settings() const` | 查询当前算法设置 |
| `bool update(const PCSSFrameInput&, unsigned depth_program, const std::function<void()>& draw_depth)` | 拟合光空间、检查/创建资源、比较签名并按需执行深度回调；失败返回 false |
| `bool is_ready() const` | 当前深度图是否可被接收查询消费 |
| `void invalidate()` | 下次 update 必须重画；不会立即重新构建 CPU 网格 |
| `std::uint64_t depth_generation() const` | 实际深度重绘代数，用于测试与诊断 |
| `const std::string& error() const` | 返回能力/资源等失败原因 |
| `void shutdown_gl()` | 在拥有资源的上下文 current 时删除资源 |
| `void abandon_lost_context()` | 仅在旧上下文已经不可用/销毁时清除旧 ID，不发 GL 删除命令 |
| `void render_plate(unsigned program, const std::array<float,16>& view_projection, float surface_z, const std::function<void()>& draw_polygon) const` | 在实际板面多边形上做受保护的接收与 alpha 合成 |

`initialize_gl()` 是 **private**，由 `update()` 内部管理，不是对外初始化入口。析构函数不偷偷发起 OpenGL 删除调用；Canvas 必须先在适当上下文调用释放接口。

传入的 Shader 程序仍由 Orca 的 `GLShadersManager` 所有，模型 VBO/IBO 由现有几何系统所有；本类只拥有自己的深度纹理、FBO 和专用 VAO。回调同步执行，不保存借用的模型指针，不可递归调用整个 Canvas 渲染。

### 4.3 接收绑定不是 `with_receiver()`，而是 RAII

```cpp
shader->start_using();
{
    PCSSReceiverScope shadow_scope(&renderer, shader->get_id());
    // 设置该材质原有的矩阵/颜色，并绘制已有几何。
}
shader->stop_using();
```

实际类为 `PCSSReceiverScope(const PCSSShadowRenderer*, unsigned program)`。它在保留的纹理单元 7 上绑定原始深度，恢复进入前的纹理/采样器绑定和活动单元；退出时清除该程序的 PCSS 启用 uniform，避免把阴影状态留给拾取、缩略图或其他材质。调用者必须先启用实际使用的注册 Shader。

### 4.4 宿主新增/变更接口

| 所属位置 | 已实现接口或改动 |
|---|---|
| `GLCanvas3D` | `_prepare_pcss_shadow_map()`；`_apply_gcode_slider_changes()`；`set_context()` 改为带旧资源释放逻辑的外部定义 |
| `GCodeViewer` | `has_shadow_geometry() const`；`render_shadow_depth(GLShaderProgram&) const`；`shadow_revision() const` |
| `GCodeViewer::render` | 末尾增加 `const PCSSShadowRenderer* shadows = nullptr`，旧调用方默认不启用 |
| `GCodeViewer::render_toolpaths` | 可空接收绑定，普通挤出三角形和顺序范围端面走对应材质变体 |
| `PartPlate` / `PartPlateList::render` | 传递可空阴影指针；只对适用的板面调用 `render_plate()` |
| `GLShadersManager` | 可选注册 `pcss_depth`、`pcss_plate`、`gouraud_pcss`、`gouraud_light_pcss` |
| `Preferences` / `AppConfig` | `enable_shadow_map` 默认值、持久化与两个 Canvas 的按需刷新 |

旧方案中的独立 `GLVolumeCollection::shadow_clip_state()` 没有新增：当前实现直接在 Canvas 复用同一组裁剪数据并上传给深度 Shader，避免为了单一消费者扩张公共 API。

## 5. 两个页面的真实数据流

### 5.1 编辑页

```text
移动/旋转/缩放/镜像改变已有 GLVolume 世界变换
  → 原有按需 render()
  → _prepare_pcss_shadow_map()
  → 收集实体基础模型、完整 world_matrix、几何 ID/range/场景代数
  → PCSSShadowRenderer::update()
  → pcss_depth + 现有 GLModel 顶点索引缓冲
  → 恢复 GL 状态
  → gouraud_pcss 普通模型接收 + pcss_plate 板面接收
```

过滤 modifier、辅助体、wipe tower、透明投射者与挤出线等不适用对象；不把渲染 UI 颜色用于生成假阴影。对象世界矩阵逐项参与签名，reload/reset 有明确失效处理；删除或集合变化不会只凭“顶点数量相同”复用结果。

为了避免摄像机 LOD 与投射基础网格不一致造成假自阴影，PCSS 接收路径使用与投射一致的基础模型。大模型下这是一个明确的性能取舍，必须在目标 GPU 上测量；关闭功能后保留原来的 LOD 路径。

### 5.2 G-code 预览页

```text
层范围/顺序进度/角色变化
  → 现有可见 render_paths 更新、shadow_revision 递增
  → _apply_gcode_slider_changes() 先处理已有输入
  → _prepare_pcss_shadow_map()
  → GCodeViewer::render_shadow_depth()
  → 可见 Extrude 三角形的 VBO / IBO / sizes / 字节 offsets
  → 顺序范围端面
  → 正常颜色绘制与 PCSS 接收
  → UI 本帧产生新变化时 request_extra_frame()
```

这里没有每帧重新解析 G-code，也没有生成替代 STL。`glMultiDrawElements` 使用已有的可见索引范围；Travel、回抽标记、接缝图标和喷头指示器不当作实体遮挡物。稳定路径包围域避免拖层时整个光图映射频繁跳变。

`FilamentId` 显示模式不对挤出材质启用 PCSS 接收，保留它原有的颜色语义；可见挤出仍可向板面投影。普通预览颜色模式使用主光贡献调制，不对图例做阴影处理。

### 5.3 实施中修正的一项光照约定

初始方案概括为“保持相机固定主光”。进一步核查 main 后，实际两条路径不同：编辑器把模型法线变到观察空间；G-code 路径现有代码上传 identity normal matrix，顶点/法线按世界空间进行该主光计算。

因此实现分别保持旧语义：**编辑页主光随相机旋转；预览主光保持现有世界方向**。没有为了让两页代码看起来一致而暗中改变原有光照，也没有把观察相机的位置放进 PCSS 半影尺度。

## 6. Shader、单位和状态约束

新增 `pcss.glsl` 以原始深度比较实现三步查询：搜索、毫米间距估计、可变半径 PCF。其采样是确定性 Vogel 圆盘，不声称是 Poisson 或时间累积。光图常规正交深度为近 0 远 1；滤波半径来自毫米间距与光角半径，转换 UV 时除以完整光覆盖宽高。

普通模型及路径的材质拆出主光漫反射和高光，保留环境光、补光、原有 alpha 和色彩逻辑。没有简单给最后的整个颜色统一乘阴影。

板面使用真实三角化多边形，以名义打印表面高度查询；有深度测试、没有深度写入，覆盖合成安排在 Logo 后、操作图标前。它是展示用合成，不是完整物理面积光材质。

新程序仅在 `140/` 路径可选初始化。旧 `110/`、能力不足或 optional Shader/FBO 失败回到旧渲染；不要求移除 Orca 的旧 OpenGL 路径。

深度 Pass 使用专用 VAO，并保护读/写 FBO、viewport、scissor、program、缓冲、深度范围/写入、颜色掩码、混合、剔除、纹理绑定等实际修改状态。清除前不受原 scissor 截断，恢复目标不假定为 FBO 0。非零 unpack PBO 的资源分配情况也在测试中覆盖。

## 7. 验证结果：分清测试层级

### 7.1 已通过

最终功能版本的 [PCSS validation / run 35558056035](https://github.com/PILIPALA030/OrcaSlicer/actions/runs/35558056035) 总体成功，四个 job 均成功：

| 检查 | 实际环境/范围 | 结果 |
|---|---|---|
| CPU 数学 | Linux、Windows 2022、macOS 14 的独立 C++17 目标 | 每次 117 个断言通过 |
| 源码契约 | 上述三个系统，显式 UTF-8 读取源文件 | 每次 12 个测试通过 |
| 实际 PCSS 渲染器 | Linux Mesa 软件 OpenGL 3.1 / GLSL 140 | 73 个断言通过 |
| Core Profile | Linux OpenGL 3.3 Core | 73 个断言通过 |
| ASan + UBSan | Linux，CPU 与实际 OpenGL 渲染器和源码检查 | 通过；GL 运行 `detect_leaks=0`，未启用 LeakSanitizer |
| 严格编译 | 实际 `PCSSShadowRenderer.cpp`，GCC，`-Wall -Wextra -Werror -pedantic` | 通过 |
| 补丁检查 | 远程应用五个功能提交后的 `git diff --check` | 通过 |

“117/73”是断言计数，不是 190 个互相独立的 GUI 场景。源码契约会检查滑块更新顺序、真实索引范围/端面、配置隔离和 Shader 注册等，但它本质上是静态检查。

OpenGL 测试直接编译**本次提交的生产渲染器**，编译/链接深度、板面及两类接收 Shader，绘制真实深度几何，再从 RGBA32F 结果中读回像素。覆盖半影、接触、光源尺度、平移/非均匀缩放/旋转、倾斜接收面、空几何、缓存、异常退出恢复、分离读写 FBO、unpack PBO 与纹理槽恢复。

这些 GPU 行为在软件 OpenGL 上执行；不能推导为用户显卡的帧率或全部驱动兼容结论。Windows/macOS job 检查的是 CPU 和源码契约，**没有在这两个系统运行真实 OpenGL 测试或整个 Orca GUI**。

### 7.2 完整宿主构建：配置阶段被阻塞

另外实际发起了完整源码配置及修改过的 GUI 编译单元验证流程，见 [run 35557682207](https://github.com/PILIPALA030/OrcaSlicer/actions/runs/35557682207)。其中应用功能提交和原生渲染测试的 `apply` job 成功；`host-build` job 失败。

该 job 没有命中项目依赖缓存，使用 Ubuntu 24.04 的系统依赖尝试配置。下载回来的 `configure.log` 中关键内容是：

```text
Found OpenVDB: /usr/include (found suitable version "10.0.1" ...)
IlmBase::Half can not be found!
OpenVDB could not be found with the bundled find module.
Configuring incomplete, errors occurred!
```

因此根项目没有完成 configure，**修改过的整套 GUI 编译单元没有在这个任务中编译，完整应用也没有链接或启动**。这里不是 PCSS C++ 编译器已报告某个错误，也不是证明 PCSS 整体一定可编译；这是在进入那一步之前被宿主依赖配置阻塞。

没有为绕过这项验证而修改 `deps/`、替换项目依赖规范，或把失败任务伪装成成功。原生渲染 CI 与宿主构建尝试分别保留记录。

### 7.3 仍未执行的验收

完整 Orca 窗口的模型拖动、层滑块拖动、真实偏好即时生效和重启持久化；目标 NVIDIA/AMD/Intel/Apple GPU 图像质量与性能；大模型/长 G-code 的交互延迟；完整 GUI 回归与安装包验证，均尚未实际完成。

没有提供假称来自 Orca 窗口的运行截图，也没有把之前独立 Demo 的截图当成本次集成结果。

## 8. 在具备项目构建环境后如何复验

### 8.1 不依赖完整 Orca 的测试

```bash
cmake -S tests/pcss -B build-pcss-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-pcss-tests --parallel
ctest --test-dir build-pcss-tests --output-on-failure -V
```

### 8.2 Linux 实际 OpenGL 检查

```bash
sudo apt-get install libglew-dev libglfw3-dev libgl1-mesa-dev xvfb xauth
cmake -S tests/pcss -B build-pcss-gl \
  -DPCSS_TEST_OPENGL=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-pcss-gl --parallel
LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a \
  ctest --test-dir build-pcss-gl --output-on-failure -V

LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3 \
  MESA_GLSL_VERSION_OVERRIDE=330 xvfb-run -a \
  ./build-pcss-gl/pcss_gl_tests --core
```

这些命令运行的是独立验证目标，不会构建完整 Orca。完整应用请使用本机已配置好的项目依赖与平台脚本，不把上一节系统依赖配置失败忽略掉。

### 8.3 实际 GUI 验收顺序

1. 用本分支构建应用并开启偏好开关，观察模型和打印板；关闭后恢复原材质显示，确认不重新切片。
2. 在编辑页持续拖动物体平移、旋转、非均匀缩放和镜像；接着删除、撤销、重做，检查接触、投影方向与残影。
3. 打开 G-code 预览，拖动上下两端层滑块、单层模式及横向进度；隐藏的挤出段不得继续投影，旅行线/工具图标不得产生实体影子。
4. 用没有 STL 的纯 G-code 验证预览，再隐藏不同路径角色、重载文件，来回切换编辑和预览，确认没有旧数据或板偏移串用。
5. 检查非矩形板、主题、选中轮廓、对象透明显示与各工具，确认未适配路径仍保持原行为。
6. 重启检查开关保存；在代表性大数据场景分别测量深度更新和接收着色成本，不能只报一个 FPS。

## 9. 明确的边界与回退

涂色/切割/可变层高专用工具、装配页、拾取、缩略图及真实透明透射没有扩展 PCSS，继续走原路径。普通编辑器的移动、旋转、缩放和放置相关路径在本次范围内。

透明实体、辅助 modifier 和 wipe tower 不作为本版常规实体投射者。预览阴影聚焦可见挤出三角形。软件驱动上验证成功不等于这些业务边界已经完成实机回归。

总开关为默认关闭的显示偏好，可立即回退。出现 optional Shader/FBO 失败时应保持旧图像；现场排查应区分“配置未开启/当前工具被排除”“驱动能力回退”“没有有效 caster”与真正采样错误。

## 10. 文件和来源索引

- [远程分支](https://github.com/PILIPALA030/OrcaSlicer/tree/feature/pcss-editor-gcode-shadows)
- [PCSSShadowRenderer.hpp](https://github.com/PILIPALA030/OrcaSlicer/blob/ede8df367fa3e1594184783a49e764a3defa512a/src/slic3r/GUI/PCSSShadowRenderer.hpp)
- [PCSSShadowRenderer.cpp](https://github.com/PILIPALA030/OrcaSlicer/blob/ede8df367fa3e1594184783a49e764a3defa512a/src/slic3r/GUI/PCSSShadowRenderer.cpp)
- [GLCanvas3D.cpp](https://github.com/PILIPALA030/OrcaSlicer/blob/ede8df367fa3e1594184783a49e764a3defa512a/src/slic3r/GUI/GLCanvas3D.cpp)
- [GCodeViewer.cpp](https://github.com/PILIPALA030/OrcaSlicer/blob/ede8df367fa3e1594184783a49e764a3defa512a/src/slic3r/GUI/GCodeViewer.cpp)
- [Preferences.cpp](https://github.com/PILIPALA030/OrcaSlicer/blob/ede8df367fa3e1594184783a49e764a3defa512a/src/slic3r/GUI/Preferences.cpp)
- [测试说明和验收清单](https://github.com/PILIPALA030/OrcaSlicer/blob/ede8df367fa3e1594184783a49e764a3defa512a/tests/pcss/README.md)
- [通过的跨平台与 OpenGL 验证](https://github.com/PILIPALA030/OrcaSlicer/actions/runs/35558056035)
- [功能提交验证及宿主构建尝试](https://github.com/PILIPALA030/OrcaSlicer/actions/runs/35557682207)

与初始方案不一致的接口命名、预览光空间和验证状态，以本报告与固定版本实际头文件为准。
