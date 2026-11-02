# AlwaysOnTop Mini

独立、轻量的 Windows「窗口置顶」小工具。核心逻辑剥离自
[microsoft/PowerToys](https://github.com/microsoft/PowerToys) 的 **Always On Top** 模块，
重写为**单文件纯 Win32** 实现，不依赖 PowerToys 及任何第三方库。

编译产物约 410 KB，常驻内存 ~10 MB，无安装、无遥测、绿色单文件。

## 功能

- **点击选窗置顶**：托盘右键 →「选择窗口置顶」，十字光标点击目标窗口即可（无需先激活，被遮挡的窗口也能选）；`Esc` / 右键取消选窗
- **单窗口置顶**：全局只保留一个置顶窗口，钉新窗口时自动取消上一个（还原 TOPMOST 标志与透明度）
- **高亮边框**：橙色 3px 边框跟随目标窗口（DWM 真实边界，Win11 圆角自适应；区域裁剪实现，绝不遮挡窗口内容）
- **透明度调节**：`Win+Ctrl+↑` / `Win+Ctrl+↓` 步进调整已置顶窗口透明度，取消置顶时自动还原
- **托盘常驻**：托盘图标右键退出；单实例互斥

## 与 PowerToys 原版的差异

剥离了与置顶功能无关的部分：

| 剥离项 | 说明 |
|---|---|
| 遥测 / ETW | 数据上报 |
| GPO 组策略检查 | 企业策略管控 |
| 提权警告通知 | UAC 相关提醒 |
| 游戏模式检测 | 全屏游戏内禁用 |
| 集中热键服务 (LLKH) | 改用标准 RegisterHotKey + 冲突回退 |
| 虚拟桌面判断 | 跨桌面时边框不隐藏，功能无碍 |
| 设置文件 / 设置界面 | 全部内置默认值 |

保留了原版的三个关键稳定性修复思路：窗口销毁清理（#15300）、最小化恢复重新钉住（#17332）、TOPMOST 丢失补回（#19168）。

## 编译

```sh
# zig cc（任意机器免安装）
zig c++ -O2 -target x86_64-windows-gnu -municode AlwaysOnTop.cpp -o AlwaysOnTop.exe \
    -lgdi32 -ldwmapi -lshell32 -Wl,--subsystem,windows
```

```sh
# MSVC
cl /O2 /EHsc /DUNICODE /D_UNICODE AlwaysOnTop.cpp user32.lib gdi32.lib dwmapi.lib shell32.lib /link /SUBSYSTEM:WINDOWS
```

## 使用

1. 双击 `AlwaysOnTop.exe`（托盘出现图标）
2. 托盘右键 →「选择窗口置顶」，点击目标窗口即可置顶
3. 开机自启：`Win+R` → `shell:startup` → 放入本程序快捷方式

## 自定义

改 `AlwaysOnTop.cpp` 顶部常量后重新编译：

- `BORDER_COLOR` — 边框颜色
- `TRANS_STEP` — 透明度步进
- 热键 — 搜索 `RegisterHotKey` 处

## 许可

源码结构源自 PowerToys（MIT License），本项目同以 MIT 发布。
