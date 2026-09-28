# MusicPack Extension

泰拉瑞亚 **TEF (Terraria Eternal Framework)** 音乐 / 音效替换模块。可按编号精确替换游戏 BGM 与音效。

## 功能

- 按 **MusicID** 精确替换游戏 BGM（Boss 曲、主菜单曲、环境音乐等）
- 按 **SoundID** 精确替换游戏音效（挖掘、受击、开门等）
- 音乐：命中时静音原曲播替换曲，离开时淡出恢复；音量跟随游戏设置；支持后台自动暂停
- 音效：命中时直接播替换音，跳过原版
- 音频格式由安卓系统解码器决定（音乐常见 mp3/ogg/wav/flac/m4a；音效建议 ogg/mp3/wav）

## 安装

1. 下载 module/MusicPack_Extension.zip（或 module/MusicPack_Extension.tefpkg）
2. 通过 TEF Manager 安装模块
3. 启动游戏一次，模块目录会自动生成 config.json、music_packs/、sfx_packs/

## 怎么放音频进去

模块目录在 **Android/data** 里，安卓 11 以后系统文件管理器进不去，需用支持 Shizuku 的文件管理器（推荐 **MT管理器**）。

### 准备工作

1. 装 **Shizuku**，按提示用无线调试激活
2. 装 **MT管理器**，在其设置里开启"使用 Shizuku"

### 目录路径

~~~
/storage/emulated/0/Android/data/eternal.future.tefmanager/files/module/private/eternal.future.audiopackextension/
  ├── config.json      配置映射
  ├── music_packs/     音乐放这里
  └── sfx_packs/       音效放这里
~~~

### 操作步骤

1. 进游戏一次（让模块生成目录），然后退出
2. 用 MT管理器定位到上面的路径
3. 把音频文件复制进 music_packs/（音乐）或 sfx_packs/（音效）
4. 编辑 config.json（见下）
5. 重启游戏生效

## 配置说明

config.json 是一个列表，音乐和音效可混写，**各最多 256 条**：

~~~json
[
  { "enable": true, "music": 5,  "file": "boss.mp3" },
  { "enable": true, "music": 50, "file": "menu.ogg" },
  { "enable": true, "type": 21,  "file": "stone.ogg" }
]
~~~

| 字段 | 说明 |
| --- | --- |
| enable | 本条是否生效（true 开 / false 关） |
| music | 要替换的**音乐编号**（MusicID），音乐用 |
| type | 要替换的**音效编号**（SoundID），音效用 |
| file | 对应文件夹里的音频文件名 |

- 有 music = 音乐项（读 music_packs/）
- 有 type = 音效项（读 sfx_packs/）
- 每条之间用英文逗号隔开

## 编号查询

- 音乐编号：见 docs/MusicID_zh.txt
- 音效编号：见 docs/SoundID_zh.csv

## 已知问题

- 替换曲为非平滑过渡，可能会感到生硬。
- 调整音乐设置音量后约 0.5 秒才能作用于替换曲。
- 首次进入游戏将逐个加载音效文件（约几秒），可能导致主菜单音乐延迟响应。
- 如果音频文件过大，将会有很明显的播放延迟（建议单个不超过 10MB）。

## 编译

见 src/BUILD.md。

## 目录结构

~~~
module/   模块安装包
src/      源码
docs/     编号表
~~~

## License

MIT
