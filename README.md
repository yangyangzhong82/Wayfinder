# Wayfinder

LeviLamina **26.51.5 客户端**小地图模组。第一版提供主世界地表小地图、全屏地图、缩放拖动、探索缓存和本地保存。

## 操作

| 按键 | 功能 |
| --- | --- |
| M | 打开/关闭全屏地图 |
| N | 显示/隐藏小地图 |
| Esc | 关闭全屏地图 |
| 左键拖动、方向键或 WASD | 平移全屏地图 |
| 滚轮、加减键 | 缩放地图，鼠标在地图内时以指针为中心 |
| Home | 恢复跟随玩家 |
| F | 显示当前维度缓存中的全部已知区域，受缩放上限限制 |

北方朝上；白点为玩家，黄线指示朝向。棋盘格代表尚未收到/采样的地形，不会自动探测服务器未发送的区域。全屏期间暂停游戏操作，**不会暂停世界或服务器**。聊天、背包等界面不响应地图快捷键。当前仅支持键鼠与主客户端视图。

全屏地图绘制独立的白色箭头光标，拖动时变黄；使用窗口客户区到 GUI 的坐标转换，使光标、拖动与滚轮缩放保持一致。关闭地图恢复原有鼠标抓取，切换窗口或打开其他界面会关闭地图。

## 构建与安装

```powershell
xmake f -y -p windows -a x64 -m release --target_type=client
xmake -y
```

将构建生成的整个 `bin/Wayfinder/` 放入匹配 LL 26.51.5 的游戏客户端 `mods` 目录。不要加载到服务端，不要混用不同版本的 MC/LL 运行库。

首次加载在 LL 提供的模组配置目录生成 `config.json`。

| 配置 | 默认值 | 说明 |
| --- | --- | --- |
| fullMapKey / toggleMinimapKey | 77 / 78 | Windows 虚拟键码，对应 M/N；修改后重启游戏 |
| sampleRadiusChunks | 8 | 玩家周围巡检半径，不会主动加载区块；实际不小于小地图可见范围 +1 区块 |
| columnsPerTick | 1024 | 每 Tick 采样列数上限 |
| samplingBudgetMicros | 1500 | 采样循环软预算，单次引擎调用可能超出预算 |
| minimapPixels / fullscreenPixels | 128 / 384 | 小地图分辨率、全屏贴图宽度 |
| minimapSize | 112 | 小地图边长，单位为 GUI 单位 |
| minimapBlocksPerPixel | 2.0 | 小地图每贴图像素覆盖的方块数 |
| refreshMilliseconds | 100 | 地图合成与纹理更新间隔；玩家标记每帧绘制 |
| autosaveSeconds | 30 | 后台自动保存间隔；退出世界和禁用也保存 |
| maxCachedChunks | 8192 | 缓存上限，超过后淘汰最久未采样区块 |
| includeWater / includeLeaves | true / true | 地表高度查询包含水和树叶 |
| cacheProfile | 空字符串 | 指定特定联机世界的唯一名称以复用跨会话历史 |

快捷键使用 LL KeyInputEvent，当前通过配置改键，不注册游戏键位设置。设置读入时限制到合理范围；语法错误会记录日志并拒绝启用。

## 世界隔离与存储

- 单人世界使用有效的 LevelId 区分记录。
- 联机或没有有效世界 ID 时，每次进入生成独立会话记录，避免服务器同地址换档导致串图。
- 联机跨会话保存：填写唯一 `cacheProfile`（例如 `friends-survival-2026`），换服务器/存档时必须换名称。相同 profile 会主动复用历史。
- 文件存放在 LL 模组数据目录的 `maps/*.wfmap`，实际路径会打印到日志。维度包含在缓存键中。
- 文件带版本、身份、大小验证，后台保存采用临时文件原子替换；损坏记录保持原样，新记录写入独立 recovery 文件。
- **这是有界缓存快照，不是无限历史档案。**最多保存 `maxCachedChunks` 个区块，已淘汰区域不会永久保留。

## 模块与验证

- `src/wayfinder/MapCore.h`：坐标、缓存、雾区、高度阴影和视图变换。
- `TerrainSampler`：客户端 Tick 分批读取区块，未就绪数据不覆盖已有样本。
- `MapRenderer`：Image → ImageBuffer → TextureGroup → TexturePtr → drawImage。
- `Wayfinder`：事件、输入、会话、刷新及生命周期。
- `MapStorage`：后台只处理自有值快照，不访问 MC 对象。

```powershell
xmake build -y wayfinder-core-tests
xmake run wayfinder-core-tests
```

测试覆盖负坐标、维度隔离、未知数据不覆盖历史、LRU、缩放锚点、栅格生成、文件往返、替换与损坏检测。API 核对与待实测事项见 `src/wayfinder/API_NOTES.md`。

**编译通过不等于完成游戏实测。**需验证 HUD/材质显示、地表高度、GUI 比例、滚轮方向、资源包重载、按住移动/攻击键打开地图、切换维度、退出重进和禁用。

本版不采样下界、末地、洞穴；不包含路标、实体雷达、材质顶面采样、地图旋转或触屏/手柄支持。非主世界显示范围说明。
