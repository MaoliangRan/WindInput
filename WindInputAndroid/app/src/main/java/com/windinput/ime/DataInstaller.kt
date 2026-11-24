package com.windinput.ime

import android.content.Context
import java.io.File

/**
 * 把 APK assets/data 解到私有 filesDir/data。
 *
 * 原子解压：先解到 data.tmp，完整后再 rename；中断不会留下半解压目录。
 * 版本号变化才重新解压（升级 APK 时替换数据）。
 */
object DataInstaller {
    // 与 assets 内数据版本同步；改 data/ 内容时递增。
    private const val DATA_VERSION = "1"

    fun ensure(context: Context): File {
        val out = File(context.filesDir, "data")
        val stamp = File(out, ".version")
        if (stamp.exists() && stamp.readText() == DATA_VERSION) return out

        val tmp = File(context.filesDir, "data.tmp")
        tmp.deleteRecursively()
        copyAssetDir(context, "data", tmp)

        if (out.exists()) out.deleteRecursively()
        if (!tmp.renameTo(out)) {
            // rename 失败（极少数文件系统）时退回逐项复制。
            copyDir(tmp, out)
            tmp.deleteRecursively()
        }
        File(out, ".version").writeText(DATA_VERSION)
        return out
    }

    /** assets 目录递归复制；`assets.list` 对文件返回空数组。 */
    private fun copyAssetDir(context: Context, assetPath: String, dest: File) {
        val children = context.assets.list(assetPath) ?: emptyArray()
        if (children.isEmpty()) {
            dest.parentFile?.mkdirs()
            context.assets.open(assetPath).use { input ->
                dest.outputStream().use { output -> input.copyTo(output) }
            }
        } else {
            dest.mkdirs()
            for (child in children) {
                copyAssetDir(context, "$assetPath/$child", File(dest, child))
            }
        }
    }

    private fun copyDir(src: File, dest: File) {
        if (src.isDirectory) {
            dest.mkdirs()
            src.listFiles()?.forEach { copyDir(it, File(dest, it.name)) }
        } else {
            dest.parentFile?.mkdirs()
            src.inputStream().use { input -> dest.outputStream().use { input.copyTo(it) } }
        }
    }
}
