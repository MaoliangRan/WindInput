# 清风输入法 Android（WindInputAndroid）

P2 主体框架：可安装、可启用的 **26 键拼音 + 英文** 输入法，并提供 **9 键 T9（与拼音引擎融合）**。
核心引擎复用 `wind_input`（Rust），Android 侧只做平台绑定与界面。

## 文档

- [工具链与环境配置](docs/TOOLCHAIN.md)：全部下载地址、校验值、从零安装、构建命令、常见问题
- [测试方案](docs/TESTING.md)：无设备/模拟器/云真机/真机的分层测试与 IME 专项清单
- [方案与架构](docs/architecture/windinput-android-architecture-清风输入法Android版方案与架构.md)：完整设计、审查记录、实施进度

## 目录

```
WindInputAndroid/
├── app/src/main/java/
│   ├── com/windinput/ime/        # IME 服务、键盘、候选、剪贴板/常用语面板、设置、引导
│   └── uniffi/wind_android/      # UniFFI 生成的 Kotlin 绑定（scripts/gen-bindings.sh 生成）
├── app/src/debug/java/           # TestActivity（仅 debug：多输入类型测试页）
├── app/src/main/jniLibs/         # libwind_android.so（scripts/build-rust.sh 生成；arm64-v8a）
├── app/src/main/assets  ← build/generated/assets/data（Gradle 从 build_dev/data 或 data/ 复制）
└── scripts/
    ├── gen-data.sh               # 下载/组装第三方词库 + 生成 t9.txt → build_dev/data
    ├── gen-t9.py                 # 从拼音词库生成 T9 索引（数字串/候选/权重/全拼）
    ├── build-rust.sh             # 交叉编译 .so（arm64-v8a）
    ├── gen-bindings.sh           # 生成 Kotlin 绑定（host 侧）
    └── build-apk.sh              # 一键：词库检查 → .so → 绑定 → APK
```

## 构建

工具链在仓库根 `.toolchain/`（Rust + JDK17 + Android SDK/NDK + Gradle），已加入
`.gitignore`，不污染系统环境。

```bash
cd windinput-android-dev
source .toolchain/env.sh

# 0) 首次/词库变化时：下载并组装第三方词库（约 40MB，输出 build_dev/data）
WindInputAndroid/scripts/gen-data.sh

# 1) 一键构建（arm64-v8a，debug 含完整词库）
WindInputAndroid/scripts/build-apk.sh

# 或分步
WindInputAndroid/scripts/build-rust.sh arm64-v8a
WindInputAndroid/scripts/gen-bindings.sh
cd WindInputAndroid && "$GRADLE_HOME/bin/gradle" :app:assembleDebug
```

产物：
- debug：`WindInputAndroid/app/build/outputs/apk/debug/app-debug.apk`（约 35MB）
- release：`WindInputAndroid/app/build/outputs/apk/release/app-release.apk`（约 33MB，需签名配置）

> 没有 `build_dev/data` 时 Gradle 会退回源 `data/`，但缺少 rime-frost 拼音词库，
> 拼音候选不可用；`build-apk.sh` 会打印警告。

## 安装与启用

```bash
adb install -r WindInputAndroid/app/build/outputs/apk/debug/app-debug.apk
adb shell ime list -a | grep windinput
adb shell ime enable com.windinput.ime/.WindImeService
adb shell ime set com.windinput.ime/.WindImeService
```

也可以打开「清风输入法」App（OnboardingActivity），按引导跳系统设置启用/选择。

debug 包额外带 `TestActivity`（普通文本/密码/数字/邮箱/URL/多行/搜索/发送），
方便验证输入类型：

```bash
adb shell am start -n com.windinput.ime/.TestActivity
```

## 当前实现（P2）

- Rust 绑定层 `wind_input/crates/wind-android`（UniFFI 0.32，cdylib，host-only bindgen）。
- `wind-mobile` 补薄门面：`readiness`、`snapshot`、`set_host_services`、`trim_memory`、T9。
- 单输入线程串行调用核心；EditOp 回主线程执行；InputConnection 判空/批编辑/异常兜底。
- 26 键 QWERTY 键盘（字母/符号层、Shift、退格、空格、回车、中英切换）。
- **9 键 T9（与拼音引擎融合）**：工具栏「9键/26键」切换；3×3 数字键盘（2-9 带字母提示）、
  0 空格、符号、退格、换行、中英键；候选 = 拼音引擎候选（前几个 T9 读音）∪ T9 词表；
  提交时把候选全拼回放给引擎选词，走词频学习/联想；空格在数字缓冲非空时选首个候选。
- 候选栏 + 编码栏：首屏候选/注释、点选上屏、**长按菜单**（置顶/删除/屏蔽/复制）、
  **「更多」展开面板**（最多 200 条，`rebuilt` 以本次窗口为准）。
- **剪贴板 / 常用语同面板顶部 Tab**：剪贴板历史默认关闭、用户开启、可清空；
  常用语复用核心短语系统（增删、系统短语启停、点击上屏）。
- **最小设置页**：键盘高度、候选字号、深色模式（跟随/浅/深）、按键声音、触感、
  9 键键盘开关、**输入方案切换**、剪贴板历史开关、关于/版本。
- **首次启动自动选拼音**（桌面 config 默认五笔；写入用户层后尊重用户选择）。
- 剪贴板 `HostServices` 桥接（系统 ClipboardManager，缓存版不阻塞）。
- 密码框/无痕：`IME_FLAG_NO_PERSONALIZED_LEARNING` → 暂停学习。
- `onTrimMemory` → 释放闲置方案/缓存；16KB page size 对齐；**arm64-v8a**（x86_64 已移除）。
- 引导页：启用输入法、选择输入法、完成。
- **release 签名**：`keystore.properties` + `keystore/`（均被 .gitignore 忽略），
  模板见 `keystore.properties.example`。

## 性能与兼容性（2026-11-24，主机/静态检查）

主机侧实测（x86_64 Linux，test profile，非 Android 真机）：

| 项 | 耗时 |
|---|---|
| 26 键候选（`nihao`，20 条） | ~0.76ms |
| 9 键首键（后台预加载后） | ~0.01ms |
| 9 键融合候选（21 条） | ~0.55ms |

- T9 索引：6.6MB / 20 万条，启动时后台预加载；**未测 Android 真机内存**（需 `dumpsys meminfo`）。
- release APK：minSdk 26 / target 34 / **arm64-v8a**；`aapt dump permissions` 无任何 uses-permission（无网络）。
- 组件：IME service `exported` + `BIND_INPUT_METHOD`；Settings 不导出；Onboarding 是唯一 launcher。
- 16KB：`.so` LOAD `Align=0x4000`；`zipalign -c -p 4` 通过；APK Signature v2 验证通过。
  （build-tools 34 的 zipalign 不支持 `-P 16`，用 `-p` + `llvm-readelf` 校验。）
- **未在真机/模拟器运行验证**（无 adb 设备）；OEM 兼容性待真机矩阵。

## 签名

```bash
cd WindInputAndroid
# 方式一：已有 keystore，复制模板并填密码
cp keystore.properties.example keystore.properties
# 方式二：本地开发用 keystore（本仓库已生成，不入库）
#   keystore/windinput-dev.jks + keystore.properties
"$GRADLE_HOME/bin/gradle" :app:assembleRelease
"$ANDROID_BUILD_TOOLS/apksigner" verify --print-certs \
  app/build/outputs/apk/release/app-release.apk
```

> 当前 `keystore/windinput-dev.jks` 是**开发自签名**，不能用于正式发布；正式发布请换成
> 你自己的 keystore，并妥善保管 `keystore.properties`。

## 已知限制 / 下一步

- **尚未在真机验证**（当前环境无 adb 设备）。首次安装请重点验证：初始化、首键、
  候选上屏、横竖屏切换、密码框、9 键输入、剪贴板/常用语面板。
- 9 键是 **T9 + 拼音引擎融合的 MVP**：候选由 T9 索引召回、经引擎 `convert` 排序合并；
  提交走引擎回放选词。尚未把“数字集合”解析并入拼音 DAG，因此没有严格意义上的
  9 键整句解码；后续可继续下沉到 `wind-engine`。
- 仅 arm64-v8a；x86_64 模拟器产物已移除（需要时按 `docs/TOOLCHAIN.md` 自行加回）。
- 设置页仍缺词库/统计/主题包等；剪贴板历史仅文本；常用语未做分组/排序/导入导出。
- 物理键盘 `onKeyDown` 未接（仅软键盘）；密码框目前只暂停学习，未强制英文/隐藏候选。
- 未做 CI、性能/兼容矩阵；debug 35MB / release 33MB（含完整词库）。

## 许可

核心为 GPL-3.0-or-later；Android 版作为其平台扩展遵循同一许可。详见仓库根
`LICENSE` / `NOTICE.md` / `BRANDING.md`。
