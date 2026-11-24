# 测试方案（TESTING）

本文覆盖没有真机时的测试路径。当前 debug APK 只含 **arm64-v8a**；x86_64 模拟器需按
`TOOLCHAIN.md §4.3` 补 x86_64 产物。

---

## 1. 测试层级

| 层级 | 依赖 | 能测什么 | 不能测什么 |
|---|---|---|---|
| Rust 主机集成测试 | 无 Android 设备 | 引擎/候选/上屏 EditOp/FFI 转换/剪贴板注入 | Android UI、InputConnection 真实行为 |
| Kotlin 单元/Robolectric | JVM | EditOp 映射、VK 映射、布局解析、数据解压 | 真实 IME 绑定、系统剪贴板 |
| 静态检查 | 无 | Manifest/权限/签名/16KB 对齐/体积 | 运行时行为 |
| 模拟器（可选） | KVM + 自行加回 x86_64 | UI 冒烟、候选点选、输入类型、生命周期 | 真机性能/OEM/arm64 专属 |
| 云真机 | 账号/费用 | arm64 真机、OEM 矩阵、Android 版本 | 深度性能调优（受网络影响） |
| 本地真机 | 设备 | 最终验收、性能、剪贴板/隐私、OEM 怪癖 | — |

---

## 2. Rust 主机集成测试（最优先）

`WindAndroid` 是普通 Rust 类型，可在 Linux 主机直接构造（`data_dir` 优先用
`build_dev/data`（gen-data 产物），缺省回退仓库根 `data/`；`user_root`/`local_root`
用临时目录）。测试文件：`wind_input/crates/wind-android/tests/session.rs`。

```rust
// wind-android/tests/session.rs（示例）
let core = WindAndroid::new(
    AndroidConfig {
        data_dir: repo_data_dir,
        user_root: tmp_user,
        local_root: tmp_local,
        fallback_schema: "pinyin".into(),
    },
    Arc::new(TestSink),
    Arc::new(TestClipboard),
);

for ch in "nihao".chars() {
    core.press(
        FfiPressKey::Char { ch: ch.to_string(), shifted: false },
        FfiKeySource::Touch,
    );
}
let win = core.candidates(0, 20);
assert!(!win.items.is_empty(), "拼音应有候选");
```

可覆盖：拼音/英文、候选、模式命令、`readiness`/`trim_memory`、剪贴板注入、FFI 镜像转换。

运行：

```bash
source .toolchain/env.sh
cd wind_input
cargo test -p wind-android
cargo test -p wind-mobile
cargo check-android
```

---

## 3. Kotlin 单元 / Robolectric

建议先把 `EditOpExecutor` 从 `WindImeService` 抽成独立类，便于用
`BaseInputConnection(SpannableStringBuilder(), false)` 测：

- `Commit` / `SetComposition` / `DeleteBackward` / `ReplaceBackward` / `MoveCursor`
- 批编辑（`beginBatchEdit/endBatchEdit`）与异常兜底
- 空 `InputConnection`、失效连接

其他单测：`Vk` 映射、`DataInstaller`（Robolectric assets）、`KeyboardView` 点击回调。

运行：

```bash
cd WindInputAndroid
"$GRADLE_HOME/bin/gradle" :app:testDebugUnitTest
"$GRADLE_HOME/bin/gradle" :app:lintDebug
```

---

## 4. 静态检查

```bash
APK=WindInputAndroid/app/build/outputs/apk/debug/app-debug.apk
"$ANDROID_BUILD_TOOLS/aapt" dump badging "$APK"
"$ANDROID_BUILD_TOOLS/aapt" dump permissions "$APK"
"$ANDROID_BUILD_TOOLS/apksigner" verify --print-certs "$APK"
"$ANDROID_BUILD_TOOLS/zipalign" -c -p 4 "$APK"
"$NDK_TOOLCHAIN/llvm-readelf" -l \
  WindInputAndroid/app/src/main/jniLibs/arm64-v8a/libwind_android.so | grep LOAD
```

检查点：IME service 有 `BIND_INPUT_METHOD`、无多余危险权限、`.so` LOAD 对齐 `0x4000`、
APK 体积（当前 debug 约 35MB / release 约 33MB，arm64）。

> build-tools 34 的 `zipalign` 不支持 `-P 16`；16KB 用 `llvm-readelf` 的 LOAD 对齐验证。

### 4.1 主机侧性能基线（2026-11-24）

x86_64 Linux，`cargo test -p wind-android --test session -- --nocapture`：

| 项 | 实测 |
|---|---|
| 26 键候选（`nihao`，20 条） | ~0.76ms |
| 9 键首键（后台预加载后） | ~0.01ms |
| 9 键融合候选（21 条） | ~0.55ms |

T9 索引 6.6MB / 20 万条，启动后台预加载；Android 真机内存/首键需用
`dumpsys meminfo` + 实测复核。

---

## 5. 模拟器（当前默认不含 x86_64）

当前 APK 只含 **arm64-v8a**；标准 x86_64 模拟器无法加载 `.so`。如需模拟器，需自行加回：

1. `app/build.gradle.kts` 的 debug `abiFilters` 加 `x86_64`；
2. `rustup target add x86_64-linux-android`，并按 aarch64 的方式补
   `CARGO_TARGET_X86_64_LINUX_ANDROID_LINKER` / `CC/CXX/AR_x86_64_linux_android`；
3. `scripts/build-rust.sh` 加回 x86_64 分支；
4. 安装 `emulator` + `system-images;android-34;google_apis;x86_64`，创建 AVD。

> 无 KVM 时模拟器很慢；真机或云真机更可靠。

---

## 6. 云真机

适合覆盖 arm64 真机与 OEM（小米/华为/OPPO/vivo/三星）：

| 平台 | 方式 |
|---|---|
| Firebase Test Lab | `gcloud firebase test android run --app app-debug.apk --test app-debug-androidTest.apk --device model=Pixel7,version=34,locale=zh_CN` |
| AWS Device Farm | 上传 APK + instrumentation 测试包，选设备池 |
| BrowserStack App Automate / App Live | Appium/Espresso；App Live 可远程手动点按 |
| 腾讯 WeTest / 阿里云 EMAS | 国内 OEM 真机矩阵 |

测试里用 UIAutomator 启用 IME：

```kotlin
UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())
    .executeShellCommand("ime enable com.windinput.ime/.WindImeService")
```

**隐私**：输入法可读取全部按键，云真机只输入测试数据（`nihao`/`hello`），不要用真实账号、
密码、聊天内容。

---

## 7. IME 专项清单

- 26 键字母/Shift/数字/符号层/空格/退格/回车
- 9 键：2-9 输入、T9 候选、**融合候选（引擎排序 + T9 召回）**、空格选首、退格、
  26/9 切换、中英键、数字缓冲清空、索引缺失时的降级
- 9 键提交：选中后应走引擎学习（词频/联想），与 26 键选词行为一致
- 全拼候选、点选、翻页/展开、候选长按菜单、拼音提示
- 中英/标点/全半角/简繁切换
- 文本/密码/数字/电话/邮箱/URL/多行；`imeOptions` 发送/搜索/下一项
- 切 App、切输入框、横竖屏、分屏、进程被杀、`onTrimMemory`
- 剪贴板默认关闭、敏感标记、密码框不记录、历史清空
- 密码框暂停学习、无痕、日志不含文本、基础包无网络权限
- 连续输入 30 分钟、快速连点、Monkey、低内存
- 冷启动到键盘可见、首键、按键→候选 P95、内存 RSS
- 亮暗/字号/横竖屏/平板/折叠屏不溢出
- TalkBack、字体缩放、高对比度、触控目标 ≥44dp
- 微信/QQ/浏览器/WebView/终端/游戏/密码框；小米/华为/OPPO/vivo/三星

---

## 8. 推荐执行顺序

1. **本机（无设备）**：Rust 集成测试 + Kotlin 单测 + 静态检查 + Lint/Clippy。
2. **真机 / 云真机**：安装/启用 IME，UIAutomator 冒烟，截图留档；重点验证 arm64 `.so` 加载、
   9 键、剪贴板、密码框。
3. **模拟器（可选）**：自行加回 x86_64 后按第 5 节构建。

> 模拟器通过不等于真机通过；JNA/UniFFI、剪贴板、触觉、OEM 行为都可能有差异。
