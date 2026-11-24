package com.windinput.ime

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import uniffi.wind_android.AndroidClipboard

/**
 * 系统剪贴板桥：实现 Rust 侧 HostServices 需要的三个方法。
 *
 * `getTextCached` 只读缓存，绝不阻塞——它是按键候选构建期调用的。
 * 剪贴板历史默认关闭；开启后由 [ClipboardHistory] 记录。
 */
class ClipboardBridge(context: Context) : AndroidClipboard {
    private val appContext = context.applicationContext
    private val manager =
        appContext.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager

    @Volatile
    private var cached = ""

    init {
        ClipboardHistory.init(appContext)
        manager.addPrimaryClipChangedListener {
            val clip = manager.primaryClip
            val text = readNow()
            cached = text
            if (!isSensitive(clip)) {
                ClipboardHistory.record(appContext, text)
            }
        }
        cached = readNow()
    }

    override fun setText(text: String) {
        manager.setPrimaryClip(ClipData.newPlainText("windinput", text))
        cached = text
        ClipboardHistory.record(appContext, text)
    }

    override fun getText(): String = readNow()

    override fun getTextCached(): String = cached

    private fun readNow(): String = try {
        val clip = manager.primaryClip
        if (clip == null || clip.itemCount == 0) {
            ""
        } else {
            clip.getItemAt(0).coerceToText(appContext)?.toString() ?: ""
        }
    } catch (_: Throwable) {
        ""
    }

    private fun isSensitive(clip: ClipData?): Boolean = try {
        clip?.description?.extras?.getBoolean(EXTRA_IS_SENSITIVE, false) ?: false
    } catch (_: Throwable) {
        false
    }

    private companion object {
        // ClipDescription.EXTRA_IS_SENSITIVE 的字符串值；用字面量避免 API 33 才有的字段。
        const val EXTRA_IS_SENSITIVE = "android.content.extra.IS_SENSITIVE"
    }
}
