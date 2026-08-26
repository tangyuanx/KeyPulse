# KeyPulse

KeyPulse 是一个无需安装的 Windows 键盘敲击统计工具。双击 `KeyPulse.exe` 即可运行，关闭窗口后会继续驻留系统托盘。

## 功能

- 全局统计每个按键的敲击次数与今日总次数
- 实时键盘热力图、当前速度、峰值速度和 Top 5 按键
- 关闭主窗口后继续在系统托盘低资源运行
- 暂停、恢复和清空当天统计
- 一键导出 1600×900 PNG 分享图
- 每 15 秒批量保存，不记录输入内容、顺序或当前窗口
- 默认保存到 `%LOCALAPPDATA%\KeyPulse`
- EXE 同目录存在 `portable.flag` 时，数据改为保存到 `data` 子目录

## 使用

1. 下载并双击 `KeyPulse.exe`。
2. Windows SmartScreen 若提示“未知发布者”，选择“更多信息 → 仍要运行”。
3. 关闭窗口会最小化到托盘；右键托盘图标可以打开、暂停、导出或退出。
4. 点击窗口右上角“导出 PNG”生成可分享的热力图图片。

程序不需要管理员权限，也不会安装驱动或系统服务。`Ctrl+Alt+Del` 等 Windows 安全桌面输入不会被记录。

## 从源码构建

需要 Windows 10/11、Visual Studio 2022 C++ 工具链和 CMake：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成文件位于 `build\Release\KeyPulse.exe`。

## 隐私设计

KeyPulse 只把 Windows 虚拟键码映射为固定计数器，例如 `A → 502`。程序不会保存字符序列、组合键顺序、窗口标题、进程名称、剪贴板或网络数据。所有数据均保存在本机。

## License

MIT
