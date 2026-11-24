import java.util.Properties

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

// 本地签名配置（keystore.properties 被 .gitignore 忽略；模板见 keystore.properties.example）。
val keystorePropsFile = rootProject.file("keystore.properties")
val keystoreProps = Properties().apply {
    if (keystorePropsFile.exists()) {
        keystorePropsFile.inputStream().use { load(it) }
    }
}

android {
    namespace = "com.windinput.ime"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.windinput.ime"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0"

        // 只产物 arm64-v8a（真机主流；模拟器需要 x86_64 时再按需加回）。
        ndk {
            abiFilters += "arm64-v8a"
        }
    }

    signingConfigs {
        if (keystorePropsFile.exists()) {
            create("release") {
                storeFile = rootProject.file(keystoreProps.getProperty("storeFile"))
                storePassword = keystoreProps.getProperty("storePassword")
                keyAlias = keystoreProps.getProperty("keyAlias")
                keyPassword = keystoreProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            // 有 keystore.properties 才启用 release 签名；没有则产出未签名包。
            if (keystorePropsFile.exists()) {
                signingConfig = signingConfigs.getByName("release")
            }
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    // 优先用 gen-data 组装出的完整数据（含 rime-frost 拼音词库）；
    // 没有 build_dev/data 时退回源 data/（此时拼音候选不可用，会打警告）。
    // 也可用 -PwindDataDir=../build_dev/data 显式指定。
    val windDataDir = (findProperty("windDataDir") as String?)?.let { rootProject.file(it) }
        ?: rootProject.file("../build_dev/data").takeIf { it.isDirectory }
        ?: rootProject.file("../data")

    val copyWindData = tasks.register<Copy>("copyWindData") {
        from(windDataDir)
        into(layout.buildDirectory.dir("generated/assets/data"))
    }
    sourceSets["main"].assets.srcDir(layout.buildDirectory.dir("generated/assets"))
    tasks.named("preBuild") { dependsOn(copyWindData) }

    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    // UniFFI 生成的 Kotlin 绑定依赖 JNA。
    implementation("net.java.dev.jna:jna:5.14.0@aar")
}
