# Orca PCSS 集成技术方案

> 2026-09-21 / v1.0。本文先于功能代码提交。计划、实际实现、测试结果分别记录，不把计划等同于验收通过。
>
> 仓库：PILIPALA030/OrcaSlicer。基线：远程 main 的 b3417d7a66d76b4de392b84894426d376f020a6b。
> 工作分支：feature/pcss-editor-gcode-shadows。所有改动仅提交到该分支，不合并、不改写 main，不强制推送。

## 1. 本次三个必须实现的目标

| 目标 | 功能要求 | 验收定义 |
|---|---|---|
| 编辑页 | 真实模型投射 PCSS 阴影，移动、缩放、旋转和镜像时更新 | 拖动过程中更新，而非仅在松开鼠标时更新；增删、撤销重做没有残影 |
| G-code 预览 | 当前可见的挤出路径投射 PCSS 阴影 | 上下层滑块、单层显示、顺序进度和角色筛选改变时同步更新；隐藏层不再投影 |
| 偏好设置 | 新增持久化 enable_shadow_map 开关 | 编辑与预览即时生效，不重启、不重切片；关闭后不运行阴影 Pass |

这里的阴影是实际的 Shadow Map → blocker search → 半影估计 → 可变半径 PCF，不能使用截图模糊或完整 STL 代理预览刀路。打印板是两种视图的基本接收者；普通实体和挤出表面的接收采用明确的材质适配。透明透射、拾取、缩略图、装配视图和 Gizmo/UI 图标保留原路径，不把 UI 当成实体遮挡物。

本次包含 G-code，不沿用旧接入文档中“第一版排除 G-code”的限制。旧文档针对 feature_color_painting_rebuild；本次所有调用点重新按 main 核查。

## 2. 依据与代码规范

依据：用户提供的 NVIDIA《Percentage-Closer Soft Shadows》、独立 PCSS Demo、双项目分析及《Orca_PCSS_接入设计与新增接口说明.md》；同时遵循仓库 AGENTS.md 和 .clang-format。

- C++17，四空格，140 列；类名 CamelCase，新增函数和变量 snake_case，常量清晰命名。
- 头文件自包含；新接口注明单位、方向、所有权、当前上下文和线程要求。
- 不修改 deps/、deps_src/；不引入新窗口库、游戏引擎或在线运行依赖。
- 不搬入 Demo 的窗口、输入、场景和 UI；复用数学和采样思路，宿主层重新适配 Orca。
- 不全文件重排旧代码，不顺带修改选择、涂色、切片等无关业务。
- 英文 commit；按文档、渲染核心、编辑、预览、偏好、验证分阶段提交。
- 不提交凭据、机器配置、编译产物；不改 main、仓库设置或生产部署。
- 自动化检查、Shader 实测、完整 GUI 编译和目标显卡验收分别报告；未运行的检查不写“通过”。

## 3. 源码结构与关键限制

| 当前结构 | 作用 | 接入注意 |
|---|---|---|
| GLCanvas3D::render | current 上下文、拾取、编辑/预览、板和 UI 调度 | 深度 Pass 在对应颜色绘制前，恢复状态后继续原顺序 |
| GLShadersManager | 选择 GLSL 140/110 并管理 Shader | 新 Shader 必须注册；GLModel 会查询管理器认可的当前程序 |
| GLModel::render | VBO/IBO 与属性绑定 | 复用 GPU 几何，不每帧重建 CPU 网格 |
| GLVolume / GLVolumeCollection | 世界变换、身份、裁剪、可见性和 LOD | 深度与颜色必须一致，不能只取实例平移 |
| PartPlate::render | 背景、网格、Logo、图标 | 阴影只覆盖实际板面多边形，不覆盖整屏或按钮 |
| GCodeViewer::TBuffer / RenderPath | 路径几何与可见索引范围 | sizes 是索引个数，offsets 是字节偏移 |
| Preferences / AppConfig | 显示偏好与持久化 | 开关不属于 PrintConfig，不触发重切片 |

G-code 深度不能用编辑模型代替，也不能只按 world Z 丢弃完整模型来伪造；必须使用现有可见路径的 VBO/IBO 和 draw ranges。

## 4. 光源与算法约定

Orca 原 Gouraud 主光定义在相机空间。本次使用方向光、正交 Shadow Map、Z-up 和毫米，保持既有主光方向语义。表面指向光为 to_light，传播方向是 -to_light；用观察矩阵旋转的逆变换得到世界方向，平移不参与。

浏览器 Demo 是有限面光源与透视投影，不能直接混用其距离比例公式。这里保留论文三步结构，方向光角尺寸换算属于本次适配设计。

编码深度近处 0、远处 1，未写入为 1。令 D 为编码深度，S 为正交 far-near（毫米），theta 为完整光角直径：

```text
gap_mm = max(0, (D_receiver - D_blocker) * S)
tan_half_angle = tan(theta_degrees * pi / 360)
radius_mm = min(gap_mm * tan_half_angle, max_radius_mm)
radius_uv = radius_mm / light_extent_mm
```

light_extent_mm 是完整宽、高，不是半宽；radius 是滤波半径，不是完整过渡宽度。窗口大小、观察相机 Z 和相机缩放不参与这个单位换算。

第一轮搜索用有效 caster 的最近光深度与接收深度推导保守范围；不能在不知道 blocker 时使用最终半影半径。原始深度使用最近邻或 texelFetch，平均比较结果才是 PCF。无 blocker 返回 1；零角尺寸退化为硬比较；参数非有限、除零、退化包围盒和 UV 越界显式处理。

采样使用确定性圆盘分布。可对光图坐标旋转降低结构感，但不把静态随机纹理称为 TAA。初版没有跨帧历史累积。

接收面偏移：bias_mm 除以 S 转为编码单位；倾斜面按 dDepth/dUV 修正各采样点比较深度，导数退化时安全回退。导数应在可能不一致的 discard/控制流前计算。半影几何距离不应受到人为 bias 无限制放大。

## 5. 模块与新增接口

拟新增 PCSSShadowRenderer（具体声明以最终头文件为准）：

```cpp
struct PCSSSettings {
    bool enabled = false;
    unsigned resolution = 2048;
    unsigned blocker_samples = 16;
    unsigned filter_samples = 32;
    float angular_diameter_deg = 4.0f;
    float bias_mm = 0.02f;
    float max_radius_mm = 12.0f;
    float plate_strength = 0.55f;
};

class PCSSShadowRenderer {
public:
    bool initialize_gl();
    void shutdown_gl();
    void invalidate();
    void set_settings(const PCSSSettings&);
    bool update(const PCSSFrameInput&,
                const std::function<void(GLShaderProgram&)>& draw_depth);
    bool is_ready() const;
    // 接收绑定需作用域恢复 Shader、纹理和启用状态。
    void with_receiver(GLShaderProgram&, const std::function<void()>& draw) const;
};
```

接口契约：只有 UI 渲染线程且拥有的 GL 上下文 current 时操作资源；update 回调只画深度，不递归 render、不改变相机和业务状态；失败返回不可用并继续旧渲染。set_settings 不在任意 wx 回调中直接删除 GPU 对象。

其他新增接口：

| 所属类 | 接口责任 |
|---|---|
| GLCanvas3D | 收集当前编辑 caster、准备/释放阴影资源、按当前视图调度 |
| GLVolumeCollection | 提供只读几何裁剪状态，或由宿主统一上传相同值 |
| GCodeViewer | render_shadow_depth：封装私有路径缓冲和可见范围；shadow_revision：数据/范围/可见性代数 |
| PartPlate / PartPlateList | 渲染可空的本帧阴影上下文；在真实板面上绘制覆盖 |
| GLShadersManager | 可选 PCSS 程序注册，公共 GLSL 源拼接 |
| Preferences | 保存 enable_shadow_map 并使两个 Canvas dirty |

PCSSFrameInput 只借用本帧几何；跨帧缓存保存值类型签名和自有 GL 资源，不持有可能被删除的模型裸指针。资源由实际 Canvas/上下文拥有，不使用进程级“当前 Shadow Map”。

## 6. 帧流程

```text
设置/场景或滑块发生变化 → 原有按需重绘
    → make current / 初始化
    → 应用最终可见几何和变换
    → 编辑：实体模型深度；预览：可见挤出路径深度
    → 恢复进入时的 GL 状态
    → 原有颜色流程 + 对应接收 Shader 的 PCSS 查询
    → 板面阴影覆盖
    → 透明对象 / 选择效果 / Gizmo / 图例 / ImGui
```

影子查询必须上传到最终实际使用的程序，不能在函数外绑定后又被内部旧 Shader 覆盖。拾取、缩略图等未适配路径显式关闭阴影，不能依赖上一帧全局 uniform。

## 7. 编辑页实现

投射者采用真正实体：排除 modifier、负体积辅助、support 标记、旅行线和界面几何。采用完整 world_matrix，包含实例变换、volume 变换、镜像和适用的 Z 偏移。裁剪与颜色 Pass 保持一致；纯颜色分界不是几何裁剪。

使用稳定的基础模型缓冲绘制深度，不触发 LOD 后台简化，也不读取未完成的异步模型。需要接收时保证所用 LOD 与深度几何有一致策略，不能把相机 LOD 的副作用直接引入深度调用。

普通模型颜色使用原 alpha、坡度/越界标色和环境贴图。主光单独拆出：ambient_and_fill + visibility * main_diffuse_and_specular。不把环境光和整张材质统一乘阴影。功能关闭选择原路径。

移动、非均匀缩放、旋转、镜像、增删、撤销重做都会改变深度输入；更新在拖动每帧而不是仅最终事件中发生。

## 8. G-code 实现

1. 使用现有 refresh_render_paths 产生的可见挤出三角形集合，包含层范围、角色和顺序进度筛选。
2. 深度绘制遍历所属 IBuffer，绑定其 VBO/IBO，沿既有 stride/position_offset/IBufferType 绘制 sizes 与字节 offsets。
3. 不把 travel/retract/seam/工具位置等线条和标记作为实体投射者。
4. 顺序范围产生的端面与颜色路径同步，不凭空封口；隐藏下层后下层不得继续投影。
5. 新文件、reset、重新切片、范围和可见性改变递增 revision；不要仅用可能复用的 GL ID 当缓存身份。
6. 滑块已存在的重绘链路继续使用，确保范围更新先于阴影更新；不增加等待鼠标松开的限制。
7. 纯 G-code 导入没有 STL 时也工作；不重新解析 G-code 或每帧重建路径 CPU 三角形。
8. 预览接收沿用色标语义，图例/喷头/UI 不受阴影着色影响。

## 9. 打印板接收

使用实际板面三角化多边形，非矩形板不出现矩形阴影外溢；世界坐标只应用一次 plate origin。覆盖层在板面背景/Logo 之后、按钮标签之前。开启深度测试但关闭深度写入，不能污染模型深度与后续鼠标映射。

板面目前是分层展示材质，覆盖层属于展示用 alpha 合成，不冒称为完整物理面光照；几何可见度仍来自真实 PCSS。工具原本隐藏打印板时保持隐藏。

## 10. 覆盖域和缓存失效

从模型/路径与板面的世界包围盒 8 角转换到光空间拟合；增加搜索/滤波保护边界。离屏 caster 仍可能影响可见区域，不能沿主相机视锥简单剔除全部离屏物体。

G-code 使用稳定的路径空间范围拟合，拖层主要改变深度内容，避免 UV 尺度跳变。空/退化场景和极端尺寸必须验证，资源数量受预算限制。

| 变化 | 深度更新 |
|---|---|
| 实体变换、集合、裁剪、数据重载 | 是 |
| G-code 可见层/顺序范围/角色 | 是 |
| 相机旋转（主光相机固定） | 是 |
| 相机平移/缩放且世界域不变 | 可复用，除非几何选择改变 |
| 材质色、主题、选择状态 | 通常不需要 |
| 滤波采样数、板面强度 | 只更新接收 |
| 分辨率、光角度/最大半径影响覆盖 | 重建/重新拟合 |
| 关闭后重开、上下文重建 | 是 |

初始实现可以对难以证明的路径保守重画，但必须有明确的正确性/性能说明。不得为了命中缓存漏掉滑块变化，也不启动无意义的永久满速重绘。

## 11. 开关和兼容回退

AppConfig 新 key enable_shadow_map，默认关闭（避免新增 GPU 成本自动影响旧用户）。英文 UI 为 Enable soft shadows (PCSS)，提供中文翻译与说明。修改后持久保存、使编辑和预览 dirty；不进入制造/项目配置，不引发重新切片。

GLSL 140 + 支持的 framebuffer/depth texture 路径启用；旧 110 路径继续无阴影渲染。能力不足、Shader 编译或 FBO 完整性检查失败时按帧关闭阴影、限量日志，不能黑屏或影响基础 Shader 初始化。

D24/等价受支持深度格式、清除到 1、最近邻原始深度读取、禁止 wrap。分辨率受 GL_MAX_TEXTURE_SIZE 限制。GL_FLOAT 输入类型不代表实际内部一定是浮点深度。

## 12. GL 状态与资源生命周期

保存/恢复实际修改的：读写 FBO、viewport、scissor、program、depth test/function/write mask/range、clear depth、color mask、blend、cull/front-face、polygon offset、纹理槽绑定、VAO/VBO/IBO。采用专用 VAO 隔离属性。清除深度前关闭 scissor；恢复进入时的 FBO，不假设 framebuffer 0。

shutdown_gl 在拥有上下文 current 时执行；不要在无法确定上下文的析构中随意 glDelete。真正丢失上下文时清空旧 ID，而不是在新上下文删除重用编号。关闭功能不让上次接收绑定泄漏到下一次 draw。

## 13. 文件计划

- 新增 PCSSShadowRenderer.hpp/.cpp、可独立测试的数学辅助和状态作用域。
- 新增 resources/shaders/140/pcss_depth、pcss_plate 及公共采样 GLSL；模型/路径接收使用独立可选变体或受控公共代码拼接。
- 修改 GLCanvas3D、GLShadersManager、PartPlate、GCodeViewer 和所需的 3DScene 接口。
- 修改 Preferences/AppConfig/i18n，更新 src/slic3r/CMakeLists.txt 和资源安装规则。
- 新增自动化测试、验收清单及实际实施报告；依赖目录不变。

## 14. 提交和执行顺序

1. docs: define the PCSS editor and G-code integration plan
2. feat(rendering): add the PCSS shadow renderer and shader pipeline
3. feat(editor): update PCSS shadows with model transforms
4. feat(preview): render shadows from visible G-code extrusion ranges
5. feat(preferences): add a persistent soft shadow toggle
6. test(rendering): cover PCSS math and integration contracts
7. 必要的英文 fix 提交与实际验证记录。

远程提交采用 fast-forward，遇到他人更新先重新读取，不覆盖。若当前执行环境不能直接取得 Git 工作树，可在这个 feature 分支使用短时、限定分支的辅助 Actions：只读源码快照；应用已审阅补丁的步骤最多 contents:write；不访问额外 secrets、不写 main。辅助传输不成为产品依赖。

## 15. 必须记录的验证

| 类别 | 用例 |
|---|---|
| 数学 | 毫米/深度/角尺寸换算、近远面变动不改变物理半影、空 blocker、边界与退化输入 |
| Shader | 实际编译/链接深度、板面、普通模型和路径接收程序；采样比较与偏移验证 |
| 编辑 | 平移/旋转/缩放/镜像拖动；增删、撤销重做、多实例、空场景 |
| 预览 | 上/下限层、单层、顺序滑块、角色筛选、纯 G-code、重载与切片 |
| 配置 | 即时开关、两页同步、重启保留、不重切片、关闭不画深度 |
| 状态 | Pass 前后状态相等、FBO/Shader 失败回退、窗口关闭/上下文重建 |
| 工程 | diff --check、改动区格式、CPU 测试、完整 GUI 构建、资源安装 |
| 性能 | 分开记录 depth 和 receiver 成本；大 G-code 拖层；缓存是否真正有效 |

每项标记通过/失败/未运行。源码存在调用链不等于目标 GPU 实测通过；软件渲染结果不代表用户显卡性能。最终报告列出远程分支、英文提交、实际新增接口、功能范围和剩余验证限制。

## 16. 源码参考

所有仓库事实固定到基线，不把未来 main 的变化静默混入本方案：

- https://github.com/PILIPALA030/OrcaSlicer/tree/b3417d7a66d76b4de392b84894426d376f020a6b
- AGENTS.md、.clang-format
- src/slic3r/GUI/GLCanvas3D.cpp、GLShadersManager.cpp、GLModel.cpp
- src/slic3r/GUI/GCodeViewer.hpp、GCodeViewer.cpp
- src/slic3r/GUI/PartPlate.cpp、3DScene.cpp
- resources/shaders/140/gouraud.vs、gouraud.fs
- 用户 NVIDIA PDF 第 1–3 页：三步算法与 parallel planes 假设。

文档是实施依据，最终代码与验证结果将在同一分支补充，不以“方案写完”代替三个功能完成。
