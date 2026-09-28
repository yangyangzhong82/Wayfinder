# API 核对记录

构建使用项目原有的 `levilamina 26.51.5`、`target_type=client`，保留了用户固定的版本。上次审阅仓库的 bedrockdata 为 `26.40.5-client.2`，最终实现以当前项目实际安装的依赖头文件为准。

## 使用的接口

- LL 事件：ClientLevelTickEvent、BlockChangedEvent、ClientExitLevelEvent、KeyInputEvent、MouseInputEvent、AfterUIRenderEvent。
- 客户端：getRegion/getLocalPlayer/getLevel/getTopScreenName、鼠标抓取、输入挂起、releaseButtonsAndSticks/resetPlayerState/resetPlayerMovement。
- 地形：getChunk/getAboveTopSolidBlock/getBlock/getMinHeight/getMaxHeight、getSubChunk/isPlaceHolderSubChunk、具名状态字段、BlockType::getMapColor，回退具名字段 BlockType::mMapColor；水体用 Material::mType、getLiquidBlock（含水方块）与 getBiome + 具名字段 Biome::mMapWaterColor。BiomeColorSampling::getMapWaterColor 为 MCFOLD，不使用。
- 图像：mce::Image、cg::ImageBuffer、TextureGroup::uploadTexture/updateTextureInPlace/isLoaded/unloadTexture。
- 绘图：TexturePtr::mClientTexture 持有 BedrockTextureData，借用其具名 mClientTexture 引用，立即 drawImage/flushImages，绘制期间保留 TexturePtr。
- 26.51.5 的 ScreenView 没有旧仓库的 equalsScreenName，改用开放的 getScreenName()。

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

- getAboveTopSolidBlock 按地表上方一格处理，再检查实际方块；缺失子区块不记为空气。
- HUD 名称为 hud_screen，当前顶层界面也必须是 HUD。
- 图片材质使用 ui_textured_and_glcolor，需要客户端材质集验证。
- MouseDevice 的按钮编号、滚轮方向需要实测；按 clientScreenSize 到 clientUIScreenSize 转换坐标。
- 全屏只是 HUD 覆盖层，releaseMouse 不负责为覆盖层画指针。现在通过公开 Win32 GetCursorPos/ScreenToClient/GetClientRect 读取并转换位置，自行绘制箭头；事件坐标仅作读取失败时的备用。绘制、拖动和缩放共用同一坐标，避免取消 MouseInputEvent 后原版光标坐标停滞。
- CPU 图像合成在 UI 回调节流执行；采样在客户端 Tick 受预算限制；工作线程只执行数据文件 I/O。
