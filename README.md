# MusicPack Extension

泰拉瑞亚 **TEF (Terraria Eternal Framework)** 音乐替换模块。可按 MusicID 精确替换游戏 BGM。

## 功能

- 按 MusicID 精确替换指定曲子（如 Boss 曲、主菜单曲）
- 替换曲支持 ogg / mp3 / wav
- 命中替换曲时自动静音原版音乐，离开时淡出恢复
- 替换曲音量跟随游戏设置里的音乐音量
- 支持后台自动暂停

## 安装

1. 下载 `module/MusicPack_Extension.zip`（或 `module/MusicPack_Extension.tefpkg`）
2. 通过 TEF Manager 安装模块
3. 进游戏，模块目录自动生成 `config.json`
4. 把音频文件放进模块目录下的 `music_packs/` 文件夹
5. 编辑 `config.json` 配置替换映射
6. 重启游戏生效

## 配置说明

`config.json` 是一个列表，每条代表把某首曲子换成某个文件：

```json
[
  { "enable": true, "music": 5, "file": "a.mp3" }
]
```

| 字段 | 说明 |
| --- | --- |
| enable | 这条是否生效（true 开 / false 关） |
| music | 要替换的曲子编号（MusicID） |
| file | `music_packs/` 里的音频文件名，要带后缀 |

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

## 编译

见 `src/BUILD.m``。

## 目录结构

```
module/   模块安装包
src/      源码
```

## License

MIT
