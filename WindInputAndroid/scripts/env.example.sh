#!/usr/bin/env bash
# 清风输入法 Android —— 本地工具链环境变量模板。
#
# 用法：
#   cp WindInputAndroid/scripts/env.example.sh .toolchain/env.sh
#   source .toolchain/env.sh
#
# 所有工具链都在 <仓库根>/.toolchain/ 下，不污染系统环境。
# 路径基于本文件所在目录（.toolchain/）推导；放在别处时请自行调整 TOOLCHAIN_DIR。

TOOLCHAIN_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$TOOLCHAIN_DIR/.." && pwd)"

# —— Rust ——
export RUSTUP_HOME="$TOOLCHAIN_DIR/rust/rustup"
export CARGO_HOME="$TOOLCHAIN_DIR/rust/cargo"
export RUSTUP_DIST_SERVER="https://mirrors.tuna.tsinghua.edu.cn/rustup"
export RUSTUP_UPDATE_ROOT="https://mirrors.tuna.tsinghua.edu.cn/rustup/rustup"
export PATH="$TOOLCHAIN_DIR/rust/cargo/bin:$PATH"

# —— JDK 17 ——
export JAVA_HOME="$TOOLCHAIN_DIR/jdk17"
export PATH="$JAVA_HOME/bin:$PATH"

# —— Android SDK / NDK ——
export ANDROID_HOME="$TOOLCHAIN_DIR/android-sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export NDK_VERSION="27.1.12297006"
export ANDROID_NDK_HOME="$ANDROID_HOME/ndk/$NDK_VERSION"
export NDK_TOOLCHAIN="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
export ANDROID_BUILD_TOOLS="$ANDROID_HOME/build-tools/34.0.0"
export ANDROID_PLATFORM="$ANDROID_HOME/platforms/android-34"

# —— Rust 交叉编译：aarch64 ——
# 不指定 linker 会退回主机 /usr/bin/ld，链接 AArch64 对象报 EM:183。
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$NDK_TOOLCHAIN/aarch64-linux-android26-clang"
export CC_aarch64_linux_android="$NDK_TOOLCHAIN/aarch64-linux-android26-clang"
export CXX_aarch64_linux_android="$NDK_TOOLCHAIN/aarch64-linux-android26-clang++"
export AR_aarch64_linux_android="$NDK_TOOLCHAIN/llvm-ar"


# Android 15+ 要求 16KB page size。
export RUSTFLAGS="-C link-arg=-Wl,-z,max-page-size=16384"

# —— Gradle ——
export GRADLE_HOME="$TOOLCHAIN_DIR/gradle/current"

echo "[env.sh] 工具链环境已加载：
  Rust   : $("$TOOLCHAIN_DIR/rust/cargo/bin/rustc" --version 2>/dev/null)
  JDK    : $("$JAVA_HOME/bin/java" -version 2>&1 | head -1)
  Gradle : $("$GRADLE_HOME/bin/gradle" --version 2>/dev/null | grep Gradle | head -1)
  NDK    : r$NDK_VERSION ($ANDROID_NDK_HOME)
  SDK    : $ANDROID_HOME"
