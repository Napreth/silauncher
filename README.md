# silauncher

**Silent Launcher** 是一个小型 Windows GUI 程序启动器，适合那些支持开机自启动、却不能在启动后自动最小化或关闭到托盘的软件。它不注入 DLL，不修改目标程序，完成窗口操作后就会退出。

## 默认行为

```powershell
silauncher.exe "C:\Program Files (x86)\eM Client\MailClient.exe"
```

直接传入程序路径，默认会隐藏启动界面，再将主窗口最小化。

启动时，`silauncher` 以 `CREATE_SUSPENDED` 创建目标进程，通过 `STARTF_USESHOWWINDOW + SW_HIDE` 请求隐藏启动，并在恢复进程前安装 `WinEvent` 窗口显示监听。发现目标进程及其子进程的顶层主窗口后，会立即隐藏窗口，等待短暂的稳定时间，再以不激活窗口的方式将其最小化。随后继续维持一小段时间的窗口状态，最后退出。

这样可以减少启动时的窗口闪现，但无法像进程内 Hook 一样保证任意程序都完全不闪现。

## 关闭到托盘

如果软件在点击右上角 X 后会驻留托盘，可以加上 `--close`。

```powershell
silauncher.exe --close "C:\Program Files\Feishu\Feishu.exe"
```

`silauncher` 会先隐藏窗口，再向主窗口发送 `WM_CLOSE`。

## 参数

```text
silauncher.exe [options] <program> [program arguments...]

--minimize           隐藏启动界面，然后保持主窗口最小化（默认）
--close              隐藏启动界面，然后向主窗口发送 WM_CLOSE
--cwd <path>         指定目标程序工作目录
--timeout-ms <n>     最长等待窗口时间，默认 30000 ms
--settle-ms <n>      首次发现窗口后，隐藏多久再执行动作，默认 250 ms
--grace-ms <n>       执行动作后继续约束多久，默认 1500 ms
--quiet              出错时不弹 MessageBox
--help               显示帮助
```

第一个不属于 `silauncher` 选项的参数是目标程序，后面的参数会原样作为独立参数转发给它。

```powershell
silauncher.exe "C:\Path\app.exe" --profile foo --another-option
```

这里的 `--profile foo --another-option` 属于 `app.exe`，不是 `silauncher`。

## 编译

使用 Visual Studio / MSVC 编译，在 **Developer PowerShell for VS** 中运行以下命令。

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

也可以使用 Visual Studio 生成器。

```powershell
cmake -S . -B build
cmake --build build --config Release
```

编译后得到 `silauncher.exe`。CMake 将它设为 `WIN32` 可执行目标，运行时不会额外弹出控制台窗口。

## 开机启动示例

按 `Win + R`，输入 `shell:startup` 打开启动文件夹，在其中创建快捷方式，将目标设为下面的命令。

```text
"C:\Tools\silauncher.exe" "C:\Program Files (x86)\eM Client\MailClient.exe"
```

需要关闭到托盘时，在目标程序路径前加上 `--close`。

```text
"C:\Tools\silauncher.exe" --close "C:\Program Files\Feishu\Feishu.exe"
```

## 当前限制

`--close` 依赖目标程序处理 `WM_CLOSE`，有些程序会忽略这个消息。如果目标程序以更高完整性级别运行，Windows 的 UIPI 可能限制跨进程窗口操作，此时需要让 `silauncher` 以相同权限运行。

某些程序由外部服务、计划任务或完全脱离原进程树的 broker 创建 GUI，这类窗口可能无法关联到最初启动的进程树。

`silauncher` 只控制启动时的窗口状态，不提供沙箱或安全隔离。
