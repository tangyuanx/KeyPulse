# KeyPulse

KeyPulse 是一个无需安装的 Windows 键盘敲击统计工具。双击 `KeyPulse.exe` 即可运行，关闭窗口后会继续驻留系统托盘。

## 功能

- 使用 Windows Raw Input 统计每个按键的敲击次数与今日总次数，不安装全局键盘钩子
- 实时全键盘热力图、当前速度、峰值速度和 Top 10 按键
- 关闭主窗口后继续在系统托盘低资源运行
- 暂停、恢复和清空当天统计
- 一键导出 1600×900 PNG 分享图
- 长按按键只记录一次；松开后再次按下才会增加计数
- 内置“检查更新”，校验 Release SHA-256 后通过原生更新进程替换并重启
- 每 15 秒批量保存，不记录输入内容、顺序或当前窗口
- 默认保存到 `%LOCALAPPDATA%\KeyPulse`
- EXE 同目录存在 `portable.flag` 时，数据改为保存到 `data` 子目录

## 使用

1. 下载并双击 `KeyPulse.exe`。
2. Windows SmartScreen 若提示“未知发布者”，选择“更多信息 → 仍要运行”。
3. 关闭窗口会最小化到托盘；右键托盘图标可以打开、暂停、导出、检查更新或退出。
4. 点击窗口右上角“导出图片”生成可分享的热力图图片。
5. 点击“检查更新”会读取本仓库最新 Release；确认后下载 EXE 和 SHA-256，校验通过才会更新并重启。

程序不需要管理员权限，也不会安装驱动或系统服务。`Ctrl+Alt+Del` 等 Windows 安全桌面输入不会被记录。

## 从源码构建

需要 Windows 10/11、Visual Studio 2022 C++ 工具链和 CMake：

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成文件位于 `build\Release\KeyPulse.exe`。

## 版本与发布

当前版本写在根目录的 `VERSION` 文件中。修改版本号并推送到 `main` 后，GitHub Actions 会构建 Windows EXE；如果对应的 `v版本号` Release 尚不存在，则自动创建 Release 并上传 `KeyPulse.exe` 与 `KeyPulse.exe.sha256`。

正式版本请从 [GitHub Releases](https://github.com/tangyuanx/KeyPulse/releases) 下载。Actions 中的临时构建产物仍会保留 30 天，用于开发验证。

## 隐私设计

KeyPulse 只把 Windows 虚拟键码映射为固定计数器，例如 `A → 502`。程序不会保存字符序列、组合键顺序、窗口标题、进程名称、剪贴板或网络数据。所有数据均保存在本机。

## License

MIT
