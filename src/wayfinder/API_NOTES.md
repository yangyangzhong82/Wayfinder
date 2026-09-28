# API 核对记录

构建使用项目原有的 `levilamina 26.51.5`、`target_type=client`，保留了用户固定的版本。上次审阅仓库的 bedrockdata 为 `26.40.5-client.2`，最终实现以当前项目实际安装的依赖头文件为准。

## 使用的接口

- LL 事件：ClientLevelTickEvent、BlockChangedEvent、ClientExitLevelEvent、KeyInputEvent、MouseInputEvent、AfterUIRenderEvent。
- 客户端：getRegion/getLocalPlayer/getLevel/getTopScreenName、鼠标抓取、输入挂起、releaseButtonsAndSticks/resetPlayerState/resetPlayerMovement。
- 世界归属使用 `getLevel()->getLevelId()` 优先，cacheProfile 仍优先；本地托管世界即使 `isPrimaryLevelMultiplayer()` 为 true，也沿用有效 LevelId，不再生成时间戳会话标识。已撤销对 `ClientNetworkSystem::getConnectionInfo()` 的新增依赖，以及等待连接类型/LevelId 的初始化门槛：后者可能使 Session::client 始终为空，同时禁用绘图与 M 键。LevelId 缺失时使用独立会话，初始化不因此提前返回；日志记录多人状态、LevelId 和最终标识。
- 地形：getChunk/getAboveTopSolidBlock/getBlock/getMinHeight/getMaxHeight、getSubChunk/isPlaceHolderSubChunk、具名状态字段、BlockType::getMapColor，回退具名字段 BlockType::mMapColor；水体用 Material::mType、getLiquidBlock（含水方块）与 getBiome + 具名字段 Biome::mMapWaterColor。BiomeColorSampling::getMapWaterColor 为 MCFOLD，不使用。
- 图像：mce::Image、cg::ImageBuffer、TextureGroup::uploadTexture/updateTextureInPlace/isLoaded/unloadTexture。
- 绘图：TexturePtr::mClientTexture 持有 BedrockTextureData，借用其具名 mClientTexture 引用，立即 drawImage/flushImages，绘制期间保留 TexturePtr。
- 26.51.5 的 ScreenView 没有旧仓库的 equalsScreenName，改用开放的 getScreenName()。

## 客户端实体显示

- 生物头像：`EntityPortraits.h` 内置 74 种原版类型的归一化 UV 与头部部件布局，核对来源为 Mojang `bedrock-samples` 提交 `46ba6ea985fb5a92d79a9419198f10dda14c199d` 的 `resource_pack/entity`、`models/entity` 和纹理。保留原版比例，部分长脸／鱼类使用侧脸；不打包或在游戏运行时下载参考资源。固定代表外观不跟随个体变种，玩家保持青色标记和名字。
- 通过 `ResourceLocation(UserPackage)`、`TextureGroup::getTexture` 与 `getCachedImageOrLoadSync` 读取客户端贴图；依据具名 ImageDescription／Blob 校验 RGBA、BGRA 或 RGB 数据。公开 `mIsMissingTexture` 和 `mTextureLoadState` 用于就绪判断，不读取未命名字段。未知类型、缺失／等待中的贴图或不支持的像素格式回退圆点，不禁用地图。
- 原版实体的 alpha 可能编码发光／染色掩码：预览已确认末影人眼睛和羊脸含 alpha=3。`PortraitPixels` 仅处理模组私有副本，将非零 alpha 转为不透明，保留零 alpha 透明区域，并限制最长边 256 像素；不修改游戏原图。GUI 线程每帧最多转换一张新图；失败等待五秒再试，避免一个缺图类型阻塞其余头像。资源句柄每秒重新获取，变更或丢失时重建副本。
- 头像按部件 drawImage，在地图裁剪区内先绘制低处、后绘制同层／高处实体，每组先非玩家、后玩家。`PortraitDraw.h` 中的 `submitPortrait` 将每个头像作为独立提交边界：同一来源的所有部件画完就 flushImages，不跨类型混用图像批次；这是针对用户反馈“其他生物显示成骷髅”的渲染隔离修复，需实机复核。每次 flushImages 的 alpha 仅作用于该头像，避免低处头像改变地表头像透明度。源贴图由游戏持有，reset 仅释放引用；上传至 `wayfinder/runtime/portraits/` 的私有纹理由模组回收。头像开关独立于实体总开关，默认 true，支持旧配置默认值、保存及恢复默认。
- `minimapEntityScale` 默认 0.75，仅缩放小地图头像和圆点；全屏仍为 1.0，名字字号独立。布局的占用矩形和名字间距同步缩放，保留透明间距作避让。`playerY - entityY >= entityDepthThreshold` 时使用 `undergroundEntityOpacity`；默认高差 4 格、不透明度 0.35，UI 显示透明度 65%，加号降低 alpha。高度分组每帧从快照坐标和当前玩家高度重算，不依赖是否开启洞穴图层，也不检测洞穴顶。低处名字及数量使用同一 alpha。
- 实体头像、圆点、数量和玩家名字不再调用黑色矩形底板绘制；保持纹理透明像素与文字自身阴影。小地图外层面板、比例尺底板及导航文字底板同样透明，`minimapOpacity` 仅控制地形贴图，不通过降低整个地图 alpha 来伪造透明背景；全屏菜单原有底板不变。
- 已生成并检查全部头像参考预览；`EntityTests` 覆盖配置切换、UV／部件边界、常见维度生物、未知类型回退、名字避让、alpha 掩码、RGBA/BGRA/RGB、损坏数据和高分辨率副本上限。仍需实机验证资源包获取、GUI 中的缩放与裁剪、资源重载及大量实体下的帧时间。

- 在 ClientLevelTickEvent 内每 250 ms 调用客户端 `getLevel()->getRuntimeActorList()`，并通过 `forEachPlayer` 补充玩家；按 Actor 指针去重，排除本地玩家和已移除对象。只保留自有坐标／类型／名字值，不跨 Tick 保留 Actor 指针，不在后台读取引擎对象。
- 初版通过 `BlockSource::getEntities(AABB, 0)` 和 `isInWorld()` 筛选，用户实测反馈没有显示；修订改为直接枚举客户端 Level 中的实体，不再依赖该空间查询和状态门槛。此处是针对采集链路的修复，仍需游戏内确认，独立回归不能验证引擎实际返回的实体。
- 使用公开 `isPlayer()` 判定后读取 `Player::getRealName()`，空名回退 `Actor::getNameTag()`；非玩家类型通过 `getTypeName()` 获取，使用规范化的命名空间 ID 精确筛选。玩家名字沿用 UTF-8 清理与 32 字符限制，绘制缺省名可见；无需 RTTI。
- 在计数、观察类型列表和 512 标记名额之前筛选存活玩家与 `ActorCategory::Mob`，同时兼容 `ActorType` 的 `Mob` 位，避免客户端分类位不全时漏掉原版生物。明确排除 `ActorType::ArmorStand` / `TripodCamera`，防止非生物 Mob 子类或使用这类基类的自定义实体混入；未知自定义类型以引擎分类为准，不要求有原版头像。类型过滤和设置列表另外拒绝旧配置中的已知非生物 ID；旧白名单不会重新启用掉落物等标记。
- 按当前维度及球形半径筛选；洞穴图层额外要求玩家高度差不超过 16 格。快照上限 512，优先玩家，再保留较近实体；渲染按实际 renderedView 投影，玩家标记及名字绘制在其他实体之上。退出世界或切层清空快照。
- 设置页面显示已加载／附近／待显示数量，用于区分采集为空、距离／维度筛选和开关／白名单／高度筛选。最后一项是可投影快照数，不是视口内计数。关闭功能且不打开实体设置时停止扫描。
- 布局使用世界坐标锚定、随缩放变化的屏幕大小网格，按类型和低处标记合并非玩家；保留精确数量与平均投影锚点，玩家永不合并。避让只移动展示位置，最大搜索三圈，并保留引导线；先分配玩家、同层与稀少种类的位置，不改原有白名单。数量角标参与碰撞，玩家名字最多搜索四档距离。缩放近看自动拆分，摄像机平移不改变世界网格边界。所有生物计数仍按合并前快照计算。
- 新增混合种类密集场景、地下独立分组、数量守恒、同种 512 只合并、边界角标、玩家名字避让、枚举顺序稳定性和模拟单纹理 UI 批次回归。测试程序支持 `--preview <JSON路径>` 导出真实 C++ 布局，离线纹理预览仅用于检查布局，不视为游戏内渲染验证。
- `WayfinderEntityTests` 覆盖配置持久化与旧配置默认值、开关／白名单／半径、稳定类型按钮、自定义类型、中文搜索、分页、负坐标投影、维度／高度排除、玩家名字与快照上限，并覆盖缩放范围、透明度方向、相对高度边界、洞穴负高度、玩家下到同层、旧非生物选择与生物分类规则。实机需验证远程玩家和名字、普通生物、掉落物不计入、传送换维度和配置重进。

## 洞穴、下界、末地与分层历史

- 已移除主世界独占采样条件。`TerrainSampler::selectLayer` 按玩家 Y 和 `caveSwitchY` 选择图层，不再查询玩家列顶面来判断自动模式。主世界自动模式在 Y≤阈值时进入洞穴，Y≥阈值+4 时退出；默认阈值 48，可在地图叠加设置中逐格调整至 -64～320，自动保存。旧配置缺省为 48，重置设置恢复默认。下界自动使用洞穴层，末地默认地表；手动 surface/cave 覆盖高度策略。
- 洞穴采样先在 8 格层中心上下 4 格查找最近的非实心空间，再最多向下扫描 64 格；处于液体内时先有界向上查找液面。所有查询先核对区块 Loaded、子区块初始化／状态和占位方块；数据缺失返回未知，不请求新加载。岩浆使用公开的 MaterialType::Lava 分类；玩家维度与客户端区域不一致时暂停采样。
- `MapLayer` 显式持有维度和带符号高度层，`TileKey` 增加 slice；地表用独立哨兵值，路标维度继续是真实维度。缓存、调度、历史索引／概要／边界、异步像素和选点查询均按完整图层隔离。切层取消旧选点并清空图片，后台旧层结果仍保存但不显示。
- 新地图文件为 v4：每区块增加 signed slice；单元原预留字节保存墙体／虚空标记。v1/v2/v3 读取默认地表，v3 访问顺序保留；地表文件名不变，洞穴增加 `_c<slice>`。墙体与确认的虚空可显示且可保存，但不提供传送高度。
- 右键传送在已加载目的地重新调用同层 TerrainSampler，避免洞穴或下界目标被替换为世界顶面。保持原有命令权限、服务端命令请求和未加载目的地历史策略。
- `WayfinderTerrainTests` 覆盖自动切换、负高度、叠层洞穴、液面／水深、占位数据语义、墙体与虚空、调度切层、缓存淘汰、细节／概要渲染、旧格式迁移和设置持久化。游戏内仍需验证下界地形可读性、末地空列子区块状态、进出洞穴与跨维度切换、窄窗口标题及不同高度层的右键传送。

## 右键传送与地形高差（基础行为）

- 右键传送使用公开 `PlayerCommandOrigin(Level&, ActorUniqueID)`、`CommandContext`、`CommandRequestPacketPayload(context, false)`、`CommandRequestPacket(payload)` 和 `LocalPlayer::sendNetworkPacket`，版本取 `CurrentCmdVersion::Latest`。来源保持为玩家，调用前检查 `IClientInstance::hasCommands()` 和玩家命令权限；不直接调用 Actor 传送或修改位置。`/tp @s X Y Z false` 按方块中心／当前层地形上方一格请求，关闭强制落点障碍检查以支持未加载目的地区域，服务器仍校验命令权限。不能用客户端区块已加载状态推断服务端的可检查状态。已加载目的地重新采样当前层；远处采用同层历史高度，无法保证历史未过时。需要实机验证单人作弊开启／关闭、远程 OP／无权限，以及离开加载范围后的历史目的地传送。
- 历史高度查询复用已有串行后台任务，仅捕获图层和 X/Z 值；菜单仍为同一图层、同一选点且等待结果时才更新高度，实时缓存优先。退出地图清除待请求，切换维度／世界沿用原有关闭和任务收尾。未知高度禁用传送，客户端不请求加载远处区块。
- 地形着色使用北／西邻格，坡度幅度对数压缩，叠加温和海拔明暗和 8 格高度带上沿；水深单独着色。历史合成先复制北边行和西边列再载入当前块，详情缓存仅一块时仍安全。远景概要在启动时从原始颜色和高度重新生成，无存档版本变更。需实机检查草地山坡、雪山、断崖、浅滩及不同缩放下的视觉效果。

## 未使用的未开放接口

| 类型/函数 | 当前处理 |
| --- | --- |
| 原版 MapRenderer/MapInstance | 不复用；需要时申请实现 |
| Editor Minimap 服务/协调器/纹理后端中的空类型 | 不使用，不假设它们是游戏 HUD 控件 |
| IClientBlockData/ClientBlockData 空类型 | 不使用，后续材质采样另行核对/申请 |
| TexturePairHelper::updateTexture、未开放 Dragon 纹理服务 | 不使用，通过公开 TextureGroup 接口更新 |
| 未知 mUnk/UntypedStorage 字段 | 不读取，不推测布局或类型 |

26.51.5 的 ClientResourcePointer 已提供实现，与旧仓库空模板不同。本模组依然不构造、复制或解析 ClientTexture/ClientResourcePointer，只借用引擎创建的引用。不修改模板布局、不调用未开放 vftable、不扫描签名、不读取固定偏移。回退旧依赖时必须重新核对或申请缺失 API。

## 需实机核实

- 本地世界进入后确认小地图可见且 M 键可用；存档标识缺失不得阻塞初始化。用户日志已确认本地世界 `multiplayer=true` 且 `levelId=cdxhG2iMotU=`，因此多人状态不再参与存档键选择。独立回归测试使用此 LevelId 和两次不同启动时间，验证相同 JSON 文件的保存/重读，并覆盖空 LevelId 的非阻塞回退、profile 优先级、不同世界隔离和编辑/删除。修改后的游戏内退出重进仍需验证。

- getAboveTopSolidBlock 按地表上方一格处理，再检查实际方块；缺失子区块不记为空气。
- HUD 名称为 hud_screen，当前顶层界面也必须是 HUD。
- 图片材质使用 ui_textured_and_glcolor，需要客户端材质集验证。
- MouseDevice 的按钮编号、滚轮方向需要实测；按 clientScreenSize 到 clientUIScreenSize 转换坐标。
- 全屏只保留 releaseMouse 释放后的系统原生光标，不再绘制额外箭头。MapMouse 通过 LL HookRegistrar 管理 ClientInstance::$grabMouse 与 $shouldRenderUICursor 的作用域拦截，仅对当前打开地图的客户端生效：阻止 HUD 再次抓取/回中，避免同时绘制游戏 UI 光标。关闭地图先清除拦截状态再恢复抓取；禁用在回调锁外卸载 hook。未使用 Win32 全局 hook、ShowCursor 计数循环或 SetCursorPos。
- 打开地图后一次性 ClipCursor(nullptr) 解除窗口限制。鼠标位置仅从 GetCursorPos/ScreenToClient/GetClientRect 获取，固定使用打开地图时的窗口；读取失败、窗口失焦或 PtInRect 判断在客户区外时清空位置与拖动，不回退游戏事件坐标或屏幕中心。GetAsyncKeyState(VK_LBUTTON) 补偿窗口外松键。需实机验证静止指针、进出窗口、拖动后在外部松键，以及关闭地图恢复视角。
- 采样在客户端 Tick 受预算限制，跨区块移动保留重叠任务。历史初始化、旧文件迁移和近期区块预加载通过独立的初始化 future 在后台完成，期间仅共享原子进度，工作线程不捕获引擎/Session 对象；初始化结束后才启动原有串行保存/合成通道。主线程合并历史时保留实时新样本、dirty 状态及淘汰队列。UI 回调在初次显示、分辨率/比例/维度切换、视口超出扩边范围，以及后台初始化期间以刷新间隔使用当前缓存即时合成。正常退出等待初始化后再保存。需实测大量历史下首次进入的响应和加载期间退出。
- 方向键/WASD 按住时按帧经过时间移动，斜向速度归一化；失焦/关闭全屏清空平移状态。鼠标坐标通过实际已显示的 renderedView 计算，避免后台图片尚未更新时显示未来视图的坐标。
- 路标/设置菜单复用 HUD 覆盖层；按钮绘制和命中共用 UiFrame，字段输入优先于地图快捷键。Win32 ToUnicodeEx 转换键盘字符，Ctrl+V 读取 Unicode 文本；尚未接入 IME 组合输入，中文通过粘贴输入。
- 死亡点通过 LocalPlayer 继承的公开 Actor::isAlive 在 ClientLevelTickEvent 观察，检测发生在窗口前台/HUD 判断之前，以支持死亡界面及切出窗口时的 Tick。需要实测服务端死亡同步与快速重生；若存活→死亡→重生完全发生在两个客户端 Tick 之间，轮询可能漏记。
- 路标使用独立的 320×256 RGBA 图集（320 KiB）：4 种图标和 16 方向箭头 × 8 色 × 普通/目标样式，单格 16×16，透明边距防止双线性采样串色。背景及白色目标描边已烘焙，每个标记一次 drawImage，同一轮在地图裁剪区域内统一 flushImages，再 flushText。图集仅首次使用或 TextureGroup 条目失效后生成/上传；与地形纹理分开持有，reset 同时释放两者，重载时重建。图标保持原有存档 ID；边缘箭头取最近的 22.5° 方向。菜单背景、玩家朝向等仍用矩形，文字通过 drawDebugText。菜单逐按钮设置裁剪区域，目标距离为 X/Z 水平距离，跨维度不计算距离。需实测中文字体、低 GUI 分辨率、密集路标的透明边缘/叠放/目标置顶、资源包重载、右键建点、死亡去重、配置保存后重进。
- 语言通过公开 `getI18n().getCurrentLanguage()` 返回的 Localization 读取具名 `mCode`，不调用 MCFOLD 的 getFullLanguageCode，不读取操作系统语言。界面绘制时重新选择词典；auto 跟随 Minecraft，zh_CN/en_US 为模组覆盖选项，不修改游戏语言。
- 玩家群系在客户端 Tick 每 500 ms 查询一次：先确认玩家区块已 Loaded，再调用 BlockSource::getBiome(BlockPos)，使用具名 Biome::mHash 与 HashedString::getString 获取标识符。主线程保留字符串，绘制时进行本地化；不通过地图中心或鼠标位置请求远处群系。
- 小地图将 `textureView`（像素对齐、四周 8 像素扩边的采样范围）与 `renderedView`（精确跟随玩家的可见范围）分开。`drawImage` 的 `uv` / `uvSize` 每帧按二者计算；纹理尺寸使用前者，玩家、路标、区块网格和比例尺使用后者。保留 1 像素过滤余量，视口超出覆盖范围时立即补图；后台完成结果需匹配尺寸、比例及当前覆盖范围才接收，存档写入与边界更新不受丢弃旧图影响。全屏仍沿用原有视图刷新行为。需实机验证慢走、负坐标过零、疾跑、传送及全屏/小地图切换。
- 比例尺和区块边界均使用实际已显示的 renderedView，避免后台图片延迟造成叠加错位。定位仅修改 fullView；不会修改玩家坐标或强制加载区块。需实机验证游戏语言切换、中文字体、比例尺与网格显示。

## Wayfinder 组件边界

- 单块载入失败（启动扫描或运行时按需读取）只隔离该文件并更新索引/边界，不触发整库 recovery；身份标记/目录级故障保留原 recovery 机制。隔离失败不覆盖原文件。坏的旧快照保持原样，已迁移的健康区块仍可用。`FeatureTests` 覆盖坏块保留、运行时损坏、文件缺失后重建、旧格式迁移、后台扫描进度和实时样本合并。
- 地图图标绘制与点击使用 `MapMarkers` 同一几何布局，左键释放且位移不超过 3 GUI 单位才触发编辑，Shift+点击直接选择导航；超过阈值仍可从图标开始拖图。失焦/出窗口/关闭地图取消待点击目标。图标位置不移动，标签上下左右避让并按标签框裁剪；完全重叠点可通过列表选择。需实机验证点击、拖动、Shift 修饰键和中文长标签。
- 路标分组为 v1 JSON 可选 `group` 字段，旧档案缺省为空；搜索/筛选仅改变视图，保存/导航继续先写磁盘再提交。距离排序只比较当前维度内的距离。设置新增缩放快捷键、水面/树叶、性能预设与二次确认恢复默认；恢复不改世界档案、缓存容量或保存周期。需实测改键持久化、采样选项更新及各 GUI 比例菜单分页。

- `Wayfinder::Impl` 只组合配置、会话、输入、视图、菜单和渲染调度，保留事件监听器、回调互斥与启用状态。`WayfinderInternal.h` 为私有声明，不暴露给模组调用方。
- `Session` 持有客户端指针、玩家快照、缓存、采样器和路标状态；`Session::History` 持有存档、在途批次、失败重试、历史边界和合成请求。后台任务仍只捕获存档与自有值数据，绝不捕获组件或引擎对象；结果在客户端回调锁内接收。
- `Input` 持有按键集合、拖动和鼠标接管状态；`Menu` 持有菜单与文字编辑状态；`WayfinderView` 仅做布局和世界坐标变换，不依赖客户端对象。`Rendering` 持有已显示的视图、像素与纹理，处理刷新节流及绘制。
- 退出顺序保持为关闭地图、保存路标、等待/刷新历史任务、清理会话；禁用时先停止事件，再完成会话退出和纹理释放，最后在回调锁外卸载鼠标 hook。地形扩展保持此生命周期与快捷键，历史格式和新设置见分层历史说明。
