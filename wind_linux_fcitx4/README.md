# wind_linux_fcitx4 — Fcitx4 / X11 前端

清风输入法在 Fcitx4 上的 addon（`fcitx-windinput.so`）。与 [`wind_linux/`](../wind_linux)
（Fcitx5 addon）对位：按键经 UDS 交给 `wind_input` 服务，候选窗 / 浮层复用 `wind_linux`
的 X11 实现。引擎、词库、候选逻辑都在服务里，本目录只做宿主适配。

## 结构

| 路径 | 角色 |
|---|---|
| `src/CompatEvent.{h,cpp}` | fcitx5 事件 API 的最小兼容层（`addIOEvent` / `addTimeEvent` / `now`），由 fcitx4 主线程 10ms tick 驱动 |
| `src/CompatLog.h` | `FCITX_LOGC` 日志宏兼容；Debug 级由 `WIND_LOG_DEBUG=1` 打开 |
| `src/X11Panel.{h,cpp}` | 自 `wind_linux/src/fcitx/X11Panel.*` 复制：只改 event/log 两处 include，逻辑零改动；另加 `capsLockOn()` 查询 X11 锁定态 |
| `src/WindEngine4.{h,cpp}` | Fcitx4 IM API 适配层（`FcitxIMClass2` / `FcitxIMIFace`） |
| `src/Addon.cpp` | 导出 `fcitx_windinput_ime2` 与 `fcitx_windinput_ABI_VERSION` |
| `data/fcitx-windinput.conf` | addon / 输入法描述（`IMRegisterMethod=Self`） |
| `CMakeLists.txt` | 复用 `../wind_linux/src/core` 纯逻辑核，构建 addon |

## 依赖与构建

依赖：Fcitx4 开发包（`pkg-config fcitx`）、`libxcb1-dev` / `libxcb-shape0-dev` /
`libxcb-randr0-dev`、CMake ≥ 3.16、支持 C++17 的编译器。

```bash
cmake -S wind_linux_fcitx4 -B wind_linux_fcitx4/build -DCMAKE_BUILD_TYPE=Release
cmake --build wind_linux_fcitx4/build -j4
```

产物 `build/fcitx-windinput.so`。GCC 8（Debian buster）下会自动链接 `stdc++fs`。

## 安装

```bash
sudo install -Dm755 build/fcitx-windinput.so \
  /usr/lib/$(gcc -print-multiarch)/fcitx/fcitx-windinput.so
sudo install -Dm644 data/fcitx-windinput.conf \
  /usr/share/fcitx/addon/fcitx-windinput.conf
```

服务与词库由 `wind_input` 负责（布局见 `wind_linux/README.md`）：`/usr/lib/windinput/`
下放 `wind_input` 与 `data/`。重启 Fcitx4 后在 `fcitx-config-gtk3` 的输入法列表里添加
「清风输入法」。用户级安装可把 addon 放到 `~/.config/fcitx/lib/` 与
`~/.config/fcitx/addon/`。

## 与 Fcitx5 版的差异

Fcitx4 会在调用输入法前改写按键状态，本适配层做了两处补偿：

- **CapsLock**：`FcitxKeyState_SimpleMask` 剥掉 LockMask。addon 在焦点进入与 CapsLock
  松开时向 X 服务器查询真实锁定态，并补进每帧 `toggles`。
- **Shift 上挡符号**：`FcitxHotkeyGetKey` 会把 keysym 已表达的 Shift 从 state 删掉
  （`Shift+.` → keysym `>`、state 无 Shift）。addon 按 keysym + CapsLock 反推
  `KEYMOD_SHIFT`，恢复 `? ! : _ < > { } | ~` 等字符与标点配对。

其他：

- 仅 X11（Fcitx4 没有 Wayland 输入法协议）。
- 不设置 Fcitx4 自带预编辑 / 候选词表：编码与候选窗由服务光栅化后自绘。
- Fcitx4 的 `fcitx-punc` / `fcitx-punc-ng` 会对透传按键做二次标点转换（`_` → `——`、
  `.` → `。`）。只使用清风时建议用户级禁用：

  ```bash
  mkdir -p ~/.config/fcitx/addon
  for a in fcitx-punc fcitx-punc-ng; do
      cp -f /usr/share/fcitx/addon/$a.conf ~/.config/fcitx/addon/
      sed -i 's/^Enabled=True/Enabled=False/' ~/.config/fcitx/addon/$a.conf
  done
  fcitx-remote -e && fcitx -d
  ```

- 设置程序 `/usr/lib/windinput/wind_setting` 由 `wind-setting` 仓库单独构建。

## 验证

实测环境：Fcitx 4.2.9.6 + Xvfb + XIM / DBus 前端。

- `a` + 空格 → 「工」；`woq` + 空格 → 「僯」；数字直选正常
- 中文 + CapsLock 开 → 字母直出大写，关 → 继续拼音
- 中文标点：`?` → `？`、`>` → `》`、`<` → `《》`（开启标点配对时）
- 英文标点：`?` / `>` / `<>`；`Ctrl+.` 切换中英标点

## 许可证

GPL-3.0-or-later，与主仓一致。
