# MusicPack Extension

泰拉瑞亚 **TEF (Terraria Eternal Framework)** 音乐替换模块。可按 MusicID 精确替换游戏 BGM。

## 功能

- 按 MusicID 精确替换指定曲子（如 Boss 曲、主菜单曲、环境音乐）
- 替换曲格式由安卓系统播放器决定（常见 mp3 / ogg / wav / flac / m4a）
- 命中替换曲时自动静音原版音乐，离开时淡出恢复
- 替换曲音量跟随游戏设置里的音乐音量
- 支持后台自动暂停

## 安装

1. 下载 `module/MusicPack_Extension.zip`（或 `module/MusicPack_Extension.tefpkg`）
2. 通过 TEF Manager 安装模块
3. 进一次游戏，模块目录会自动生成 `config.json` 和 `music_packs/` 文件夹

## 怎么放音乐进去

模块目录在 **Android/data** 里面，安卓 11 以后系统自带的文件管理器进不去，需要用支持 Shizuku 的文件管理器（推荐 **MT管理器**）。

### 准备工作

1. 装 **Shizuku**（应用商店或 GitHub 搜 Shizuku），按提示用无线调试激活
2. 装 **MT管理器**，在 MT管理器里授权 Shizuku（MT管理器设置里开"使用 Shizuku"）

### 目录路径

```
/storage/emulated/0/Android/data/eternal.future.tefmanager/files/module/private/eternal.future.audiopackextension/
  ├── config.json      配置映射
  └── music_packs/     音乐放这里
```

### 操作步骤

1. 进游戏一次（让模块生成目录），然后退出游戏
2. 用 MT管理器（已开 Shizuku）定位到上面的路径
3. 打开 `music_packs/`，把你的音频文件复制进去
4. 打开同级的 `config.json` 编辑（见下）
5. 重启游戏生效

## 配置说明

`config.json` 是一个列表，每条代表把某首曲子换成某个文件，最多 32 条：

```json
[
  { "enable": true, "music": 5, "file": "a.mp3" },
  { "enable": true, "music": 50, "file": "menu.ogg" }
]
```

| 字段 | 说明 |
| --- | --- |
| enable | 本条是否生效（true 开 / false 关） |
| music | 要替换的曲子编号（MusicID），需自行确认 |
| file | `music_packs/` 里的音频文件名 |

## 常用 MusicID

| ID | 曲子 |
| --- | --- |
| 1 | 地表白天 |
| 5 | Boss1（克苏鲁之眼等） |
| 12 | 血肉之墙 |
| 18 | 地表备用 |
| 24 | 世纪之花 |
| 38 | 月亮领主 |
| 50 | 主菜单 |
| 57 | 光之女皇 |
| 58 | 猪龙鱼公爵 |

## 已知问题

- 替换曲为非平滑过渡，可能会感到生硬。
- 调整音乐设置音量后约 0.5 秒才能作用于替换曲。
- 首次进入游戏将逐个校验音频文件（约几秒），可能导致主菜单音乐延迟响应（如果替换了）。
- 如果放入的音频文件过大，将会有很明显的播放延迟（建议单个文件不超过 10MB）。
- 在有背景音乐的事件期间，如果出现替换过音乐的 BOSS 战斗场景，可能会导致无 Boss 战音乐。
- 在打 Boss 的时候，请慎重进行环境切换，因为在某些特定的环境下（如：传奇空岛主岛），会导致 Boss 战音乐无法正常播放，如遇到这个问题，换个环境，也许就能解决。
- 升级模块时建议直接覆盖安装，卸载会导致 `music_packs/` 和 `config.json` 一并被清除。

## 编译

见 `src/BUILD.md`。

## 目录结构

```
module/   模块安装包
src/      源码
```

## License

MIT
