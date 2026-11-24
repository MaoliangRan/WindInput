# 工具链与构建环境（TOOLCHAIN）

本文记录构建 `WindInputAndroid` 所需的全部工具、下载地址、校验值、安装位置与构建方法。
所有工具默认安装在仓库根 **`.toolchain/`**（已加入 `.gitignore`，不随仓库提交，不污染系统环境）。

> 版本是本机实际验证过的组合：Rust 1.100.0 / JDK 17.0.21.1 / Gradle 8.7 / AGP 8.5.2 /
> Kotlin 1.9.24 / NDK r27.1.12297006 / Android platform 34。

---

## 1. 组件清单与下载地址

| 组件 | 版本 | 官方下载 | 本机使用/镜像 | SHA-256 | 安装位置 |
|---|---|---|---|---|---|
| rustup-init | 1.28+ | <https://static.rust-lang.org/rustup/dist/x86_64-unknown-linux-gnu/rustup-init> | <https://mirrors.tuna.tsinghua.edu.cn/rustup/rustup/dist/x86_64-unknown-linux-gnu/rustup-init> | `b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55` | `.toolchain/downloads/rustup-init` |
| Rust stable | 1.100.0 | `rustup` 自动下载 | `RUSTUP_DIST_SERVER=https://mirrors.tuna.tsinghua.edu.cn/rustup` | — | `.toolchain/rust/rustup` + `.toolchain/rust/cargo` |
| Temurin JDK 17 | 17.0.21.1+2 | <https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse> | <https://mirrors.tuna.tsinghua.edu.cn/Adoptium/17/jdk/x64/linux/OpenJDK17U-jdk_x64_linux_hotspot_17.0.21.1_2.tar.gz> | `0b0ac5eab6db6abaec68c9913bcddb31321e5da411c45073d520d9c125e6f7b4` | `.toolchain/jdk17/` |
| Android cmdline-tools | 11076708 | <https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip> | 同官方 | `2d2d50857e4eb553af5a6dc3ad507a17adf43d115264b1afc116f95c92e5e258` | `.toolchain/android-sdk/cmdline-tools/latest/` |
| Android platform-tools | 34.x | `sdkmanager "platform-tools"` | — | — | `.toolchain/android-sdk/platform-tools/` |
| Android platform 34 | android-34 | `sdkmanager "platforms;android-34"` | — | — | `.toolchain/android-sdk/platforms/android-34/` |
| Android build-tools | 34.0.0 | `sdkmanager "build-tools;34.0.0"` | — | — | `.toolchain/android-sdk/build-tools/34.0.0/` |
| Android NDK | 27.1.12297006 (r27b) | `sdkmanager "ndk;27.1.12297006"` | — | — | `.toolchain/android-sdk/ndk/27.1.12297006/` |
| Gradle | 8.7 | <https://services.gradle.org/distributions/gradle-8.7-bin.zip> | 腾讯镜像 `https://mirrors.cloud.tencent.com/gradle/gradle-8.7-bin.zip`（本次 567 失败，回退官方） | `544c35d6bd849ae8a5ed0bcea39ba677dc40f49df7d1835561582da2009b961d` | `.toolchain/gradle/current/` |
| Android Emulator（可选） | 最新 | `sdkmanager "emulator"` | — | — | `.toolchain/android-sdk/emulator/` |

### Gradle 依赖（首次构建自动下载）

| 依赖 | 版本 | 仓库 |
|---|---|---|
| Android Gradle Plugin | 8.5.2 | <https://dl.google.com/dl/android/maven2/com/android/tools/build/gradle/8.5.2/> |
| Kotlin Gradle Plugin | 1.9.24 | <https://repo1.maven.org/maven2/org/jetbrains/kotlin/kotlin-gradle-plugin/1.9.24/> |
| JNA（UniFFI Kotlin 绑定） | 5.14.0 (`@aar`) | <https://repo1.maven.org/maven2/net/java/dev/jna/jna/5.14.0/> |
| UniFFI | 0.32.2 | <https://crates.io/crates/uniffi/0.32.2> |

> JNA 的 `@aar` 自带各 ABI 的 `libjnidispatch.so`；当前只打包 **arm64-v8a**（`abiFilters`），
> x86_64 已移除。

### 1.1 第三方词库/数据源（gen-data 下载，不入库）

| 数据 | 上游 | 用途 | 许可 |
|---|---|---|---|
| rime-frost | <https://github.com/gaboolic/rime-frost>（raw: `https://raw.githubusercontent.com/gaboolic/rime-frost/master/...`；镜像: `https://cdn.jsdelivr.net/gh/gaboolic/rime-frost@master/...`） | 拼音主库 `rime_frost.dict.yaml`、`cn_dicts/*`、细胞词库、英文词库、emoji/符号表 | GPL-3.0（见上游） |
| pinyin-data | <https://github.com/mozillazg/pinyin-data> | 汉字读音底表 → `pinyin_map.txt` | 见上游 |
| OpenCC | <https://github.com/BYVoid/OpenCC>（`data/dictionary/*.txt`） | 简繁转换 `.octrie` | Apache-2.0（见上游） |
| rime-wubi86-jidian | <https://github.com/KyleBing/rime-wubi86-jidian> | 五笔词库（Phase 2 用） | 见上游 |
| rime-lua-aux-code | <https://github.com/HowcanoeWang/rime-lua-aux-code> | 小鹤/自然码辅助码 | 见上游 |
| rime-stroke | <https://github.com/rime/rime-stroke> | 笔画表（LGPL-3.0，只下载不入库） | LGPL-3.0 |
| rime-emoji | <https://github.com/rime/rime-emoji> | 候选 emoji 扩展（LGPL-3.0，只下载不入库） | LGPL-3.0 |
| Unicode CLDR | <https://github.com/unicode-org/cldr>、<https://unicode.org/Public/emoji/> | emoji 中文名（仅重生成时需要） | Unicode-3.0 |
| T9 索引 | `WindInputAndroid/scripts/gen-t9.py` 从 rime-frost 生成 | 9 键候选 `build_dev/data/t9.txt`（数字串/候选/权重/全拼） | 数据来自 rime-frost（GPL-3.0） |

> 这些数据**不提交到 git**（`.cache/`、`build_dev/` 均被忽略）；分发 APK 时遵守各自许可，
> 完整声明见仓库根 `NOTICE.md`。`WindInputAndroid/scripts/gen-data.sh` 会先走 jsdelivr
> 预取大文件，失败再由 `scripts/dev.sh gen-data` 回退 raw。

---

## 2. 从零安装（复现步骤）

> 以下命令均假设在仓库根执行；`$PWD` 即 `windinput-android-dev/`。

```bash
# 0) 目录
mkdir -p .toolchain/{downloads,rust/rustup,rust/cargo,jdk17,android-sdk/cmdline-tools,gradle}

# 1) rustup + Rust stable
cd .toolchain
curl -fL -o downloads/rustup-init \
  https://mirrors.tuna.tsinghua.edu.cn/rustup/rustup/dist/x86_64-unknown-linux-gnu/rustup-init
chmod +x downloads/rustup-init
export RUSTUP_HOME="$PWD/rust/rustup"
export CARGO_HOME="$PWD/rust/cargo"
export RUSTUP_DIST_SERVER="https://mirrors.tuna.tsinghua.edu.cn/rustup"
export RUSTUP_UPDATE_ROOT="https://mirrors.tuna.tsinghua.edu.cn/rustup/rustup"
./downloads/rustup-init -y --no-modify-path --default-toolchain stable --profile minimal
./rust/cargo/bin/rustup default stable
./rust/cargo/bin/rustup target add aarch64-linux-android

# 2) JDK 17
curl -fL -o downloads/jdk17.tar.gz \
  "https://mirrors.tuna.tsinghua.edu.cn/Adoptium/17/jdk/x64/linux/OpenJDK17U-jdk_x64_linux_hotspot_17.0.21.1_2.tar.gz"
tar xzf downloads/jdk17.tar.gz -C jdk17 --strip-components=1
jdk17/bin/java -version

# 3) Android cmdline-tools
curl -fL -o downloads/cmdline-tools.zip \
  https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
mkdir -p android-sdk/cmdline-tools/latest
unzip -q downloads/cmdline-tools.zip -d /tmp/cmdline-tools-extract
mv /tmp/cmdline-tools-extract/cmdline-tools/* android-sdk/cmdline-tools/latest/
rm -rf /tmp/cmdline-tools-extract

# 4) Android SDK 组件
export JAVA_HOME="$PWD/jdk17"
export ANDROID_HOME="$PWD/android-sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
SDKMGR="$ANDROID_HOME/cmdline-tools/latest/bin/sdkmanager"
yes | "$SDKMGR" --licenses
"$SDKMGR" "platform-tools" "platforms;android-34" "build-tools;34.0.0" "ndk;27.1.12297006"

# 5) Gradle
curl -fL -o downloads/gradle-8.7-bin.zip https://services.gradle.org/distributions/gradle-8.7-bin.zip
mkdir -p gradle
unzip -q downloads/gradle-8.7-bin.zip -d gradle
mv gradle/gradle-8.7 gradle/current

# 6) 环境脚本
# 模板在 WindInputAndroid/scripts/env.example.sh；复制到 .toolchain/env.sh 后 source
cp WindInputAndroid/scripts/env.example.sh .toolchain/env.sh
source .toolchain/env.sh
```

---

## 3. 环境变量（`.toolchain/env.sh`）

构建前一律先加载：

```bash
cd windinput-android-dev
source .toolchain/env.sh
```

它设置：

| 变量 | 作用 |
|---|---|
| `RUSTUP_HOME` / `CARGO_HOME` / `PATH` | 使用项目内 Rust，不碰 `~/.cargo` |
| `JAVA_HOME` | JDK 17（AGP 8.x 要求） |
| `ANDROID_HOME` / `ANDROID_SDK_ROOT` | 项目内 Android SDK |
| `NDK_VERSION` / `ANDROID_NDK_HOME` / `NDK_TOOLCHAIN` | NDK r27 |
| `CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER` | **必须**；否则 Cargo 用主机 `ld` 链接 AArch64 对象（`redb` 报 `EM:183`） |
| `CC/CXX/AR_aarch64_linux_android` | cc-rs 交叉编译 |
| `RUSTFLAGS=-C link-arg=-Wl,-z,max-page-size=16384` | Android 15+ 16KB page size |
| `GRADLE_HOME` | 项目内 Gradle 8.7 |

> `env.sh` **不把 gradle 加入 PATH**，避免与项目 wrapper 冲突；构建时用
> `"$GRADLE_HOME/bin/gradle"` 或 `scripts/build-apk.sh`。

---

## 4. 构建

### 4.0 数据准备（gen-data，首次/词库变化时）

拼音/英文词库来自第三方（rime-frost 等），**不在 git 里**，需要下载并组装：

```bash
cd windinput-android-dev
source .toolchain/env.sh
WindInputAndroid/scripts/gen-data.sh
# 输出：build_dev/data（约 47MB，含 rime_frost.dict.yaml / cn_dicts / ext / en_dicts /
#       grammar / pinyin_map.txt / opencc / wubi / aux / comments / t9.txt）
```

`scripts/gen-data.sh` 内部用 `WIND_BUILD_LOCAL=1 ./scripts/dev.sh gen-data`：
桌面仓默认把 gen-data 转发 Windows 编译机，Android 只需要数据，本机执行即可。

Gradle 的 `copyWindData` 会**优先**使用 `build_dev/data`；没有时退回源 `data/`
（此时缺拼音词库，拼音候选不可用）。

### 4.1 一键 debug APK（arm64）

```bash
cd windinput-android-dev
source .toolchain/env.sh
WindInputAndroid/scripts/build-apk.sh
# 产物：WindInputAndroid/app/build/outputs/apk/debug/app-debug.apk
#       约 35MB（arm64 + 完整词库）；release 约 33MB
```

`build-apk.sh` 依次执行：

1. `scripts/build-rust.sh arm64-v8a`：`cargo build -p wind-android --release --target aarch64-linux-android`，
   复制 `libwind_android.so` 到 `app/src/main/jniLibs/arm64-v8a/`。
2. `scripts/gen-bindings.sh`：用 `uniffi-bindgen` 生成 Kotlin 绑定到
   `app/src/main/java/uniffi/wind_android/wind_android.kt`。
3. `gradle :app:assembleDebug`。

### 4.2 分步构建

```bash
source .toolchain/env.sh
WindInputAndroid/scripts/build-rust.sh arm64-v8a
WindInputAndroid/scripts/gen-bindings.sh
cd WindInputAndroid
"$GRADLE_HOME/bin/gradle" :app:assembleDebug --no-daemon --console=plain
```

### 4.3 静态验收与签名

```bash
APK=WindInputAndroid/app/build/outputs/apk/debug/app-debug.apk
"$ANDROID_BUILD_TOOLS/aapt" dump badging "$APK"
"$ANDROID_BUILD_TOOLS/apksigner" verify --print-certs "$APK"
"$ANDROID_BUILD_TOOLS/zipalign" -c -p 4 "$APK"
"$NDK_TOOLCHAIN/llvm-readelf" -l WindInputAndroid/app/src/main/jniLibs/arm64-v8a/libwind_android.so | grep LOAD
```

`LOAD` 段 `Align` 应为 `0x4000`（16KB）。

> build-tools 34 的 `zipalign` 不支持 `-P 16`；16KB 用 `llvm-readelf` 的 LOAD 对齐验证，
> `zipalign -c -p 4` 只验证 page-align 的 `.so`。升级 build-tools 35+ 后可用 `-P 16`。

release 签名：

```bash
cd WindInputAndroid
cp keystore.properties.example keystore.properties   # 填 storePassword/keyPassword
# 或使用本地开发 keystore：keystore/windinput-dev.jks（.gitignore 忽略）
"$GRADLE_HOME/bin/gradle" :app:assembleRelease
"$ANDROID_BUILD_TOOLS/apksigner" verify --print-certs \
  app/build/outputs/apk/release/app-release.apk
```

> `keystore/` 与 `keystore.properties` 均不入库；开发自签名证书不能用于正式发布。

---

## 5. 常见问题

### 5.1 `redb` / `EM:183` / `file in wrong format`

Cargo 未指定 Android linker，退回主机 `/usr/bin/ld`。确认：

```bash
echo "$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER"
# 应指向 .../ndk/27.1.12297006/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang
```

### 5.2 `uniffi-bindgen` 卡在 `cargo metadata`

它会解析整个 workspace，可能尝试下载无关依赖（如 `rav1e`）。用：

```bash
CARGO_NET_OFFLINE=true ./target/debug/uniffi-bindgen generate \
  --library target/debug/libwind_android.so \
  --language kotlin --out-dir /tmp/bindings \
  --metadata-no-deps --no-format
```

若提示缺某个 crate（如 `pxfm`），先 `cargo fetch` 补齐再离线执行。

### 5.3 数据目录/拼音词库缺失

- 源数据在**仓库根 `data/`**（不是 `wind_input/data`）。
- 拼音主词库 `rime_frost.dict.yaml` 来自第三方，**不在 git**；运行
  `WindInputAndroid/scripts/gen-data.sh` 生成到 `build_dev/data`。
- Gradle 的 `copyWindData` 优先 `build_dev/data`，其次 `data/`；可在命令行覆盖：
  `-PwindDataDir=../build_dev/data`。

### 5.4 Gradle 下载慢/镜像失败

官方 `services.gradle.org` 可用；腾讯镜像曾返回 567。Gradle 依赖（AGP/Kotlin/JNA）走
`google()` + `mavenCentral()`，首次构建需联网。

### 5.5 模拟器

- 当前 APK **只含 arm64**；标准 x86_64 模拟器无法加载，需要时自行在
  `app/build.gradle.kts` 加回 x86_64、`rustup target add x86_64-linux-android`，并补
  `x86_64-linux-android26-clang` 的 linker。
- 真机验证不需要模拟器；云真机也可（见 `docs/TESTING.md`）。
- 若加回模拟器：检查 `/dev/kvm` 与 `emulator -accel-check`；无 KVM 时 `-accel off` 极慢。

### 5.6 真机/模拟器安装与启用

```bash
adb install -r WindInputAndroid/app/build/outputs/apk/debug/app-debug.apk
adb shell ime list -a | grep windinput
adb shell ime enable com.windinput.ime/.WindImeService
adb shell ime set com.windinput.ime/.WindImeService
# 还原默认输入法
adb shell ime reset
```

---

## 6. 目录与产物（哪些不进 git）

| 路径 | 说明 | git |
|---|---|---|
| `.toolchain/` | 全部本地工具链（约 4GB） | 忽略 |
| `.cache/` | 第三方词库下载（rime-frost 等） | 忽略（根 `.gitignore`） |
| `build_dev/` | gen-data 组装出的完整数据（约 40MB） | 忽略 |
| `wind_input/target/` | Rust 编译产物 | 忽略（根 `.gitignore`） |
| `WindInputAndroid/.gradle/` | Gradle 缓存 | 忽略 |
| `WindInputAndroid/app/build/` | APK/中间产物 | 忽略 |
| `WindInputAndroid/app/src/main/jniLibs/**/*.so` | Rust 交叉编译产物 | 忽略（由脚本生成） |
| `WindInputAndroid/app/src/main/java/uniffi/` | UniFFI 生成的 Kotlin | 忽略（由脚本生成） |
| `WindInputAndroid/local.properties` | 本机 SDK 路径 | 忽略 |
| `*.jks` / `*.keystore` | 签名密钥 | 忽略（不入库） |

首次克隆后先跑 `scripts/gen-data.sh` + `scripts/build-apk.sh`，上述生成物会自动补齐。
