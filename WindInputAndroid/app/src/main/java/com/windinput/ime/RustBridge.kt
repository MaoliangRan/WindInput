package com.windinput.ime

import android.os.Handler
import android.os.Looper
import uniffi.wind_android.AndroidConfig
import uniffi.wind_android.AndroidEventSink
import uniffi.wind_android.FfiCandidateFrame
import uniffi.wind_android.FfiCandidateMenuItem
import uniffi.wind_android.FfiCandidateWindow
import uniffi.wind_android.FfiInputStatus
import uniffi.wind_android.FfiKeyOutcome
import uniffi.wind_android.FfiKeySource
import uniffi.wind_android.FfiMobileCommand
import uniffi.wind_android.FfiPhrase
import uniffi.wind_android.FfiPressKey
import uniffi.wind_android.FfiSchemaEntry
import uniffi.wind_android.FfiT9Candidate
import uniffi.wind_android.WindAndroid
import java.io.File
import java.util.concurrent.Executors

/**
 * Kotlin ↔ Rust 的桥。
 *
 * 线程模型：所有 WindAndroid 调用在**单线程** executor 上串行执行；结果回主线程。
 * 这样按键不会乱序（后按的键先回 UI），也不会在 UI 线程上阻塞。
 */
class RustBridge(private val service: WindImeService) {

    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "wind-input")
    }
    private val main = Handler(Looper.getMainLooper())

    @Volatile
    private var core: WindAndroid? = null

    @Volatile
    private var schemas: List<FfiSchemaEntry> = emptyList()

    var onFrame: ((FfiCandidateFrame) -> Unit)? = null
    var onStatus: ((FfiInputStatus) -> Unit)? = null
    var onToast: ((String) -> Unit)? = null
    var onHide: (() -> Unit)? = null
    var onReady: (() -> Unit)? = null

    fun start() {
        executor.execute {
            try {
                val data = DataInstaller.ensure(service)
                val cfg = AndroidConfig(
                    dataDir = data.absolutePath,
                    userRoot = File(service.filesDir, "user").apply { mkdirs() }.absolutePath,
                    localRoot = File(service.cacheDir, "wind").apply { mkdirs() }.absolutePath,
                    fallbackSchema = "pinyin",
                )
                val c = WindAndroid(cfg, EventSink(), ClipboardBridge(service))
                core = c
                schemas = c.schemas()
                SchemaCache.entries = schemas

                // 桌面 config 默认激活五笔；Android 首装应落在拼音。
                // 只做一次：写入用户层后，后续尊重用户在设置/语言键里的选择。
                if (!Prefs.schemaInitialized(service)) {
                    val pinyin = schemas.indexOfFirst { it.id == "pinyin" }
                    if (pinyin >= 0) {
                        c.runCommand(FfiMobileCommand.SelectSchema(pinyin.toUInt()))
                    }
                    Prefs.setSchemaInitialized(service, true)
                }

                // 设置页选择的方案：下次初始化时应用一次。
                Prefs.pendingSchemaId(service)?.let { pending ->
                    val idx = schemas.indexOfFirst { it.id == pending }
                    if (idx >= 0) {
                        c.runCommand(FfiMobileCommand.SelectSchema(idx.toUInt()))
                    }
                    Prefs.setPendingSchemaId(service, null)
                }

                main.post { onReady?.invoke() }
            } catch (e: Throwable) {
                main.post { onToast?.invoke("输入法初始化失败：${e.message}") }
            }
        }
    }

    fun pressChar(ch: String, shifted: Boolean) = execOutcome {
        core?.press(FfiPressKey.Char(ch, shifted), FfiKeySource.TOUCH)
    }

    fun pressKey(key: FfiPressKey) = execOutcome {
        core?.press(key, FfiKeySource.TOUCH)
    }

    fun selectCandidate(index: Int) = execOutcome {
        core?.selectCandidate(index.toUInt())
    }

    /** 拉取候选窗口；回调在主线程。 */
    fun candidates(offset: Int, limit: Int, callback: (FfiCandidateWindow) -> Unit) {
        executor.execute {
            try {
                core?.candidates(offset.toUInt(), limit.toUInt())?.let { win ->
                    main.post { callback(win) }
                }
            } catch (_: Throwable) {
                // 初始化未完成/InputConnection 失效：忽略这一轮，等下一次推送。
            }
        }
    }

    /** 候选长按菜单。 */
    fun candidateMenu(index: Int, callback: (List<FfiCandidateMenuItem>) -> Unit) {
        executor.execute {
            try {
                core?.candidateMenu(index.toUInt())?.let { items ->
                    main.post { callback(items) }
                }
            } catch (_: Throwable) {
                // 忽略。
            }
        }
    }

    /** 执行候选菜单动作；`copy` 由宿主自己处理。 */
    fun candidateMenuRun(index: Int, action: String, done: () -> Unit = {}) {
        executor.execute {
            try {
                core?.candidateMenuRun(index.toUInt(), action)
            } catch (_: Throwable) {
                // 忽略。
            }
            main.post { done() }
        }
    }

    /** 命令短语（系统 + 用户）。 */
    fun phrases(callback: (List<FfiPhrase>) -> Unit) {
        executor.execute {
            try {
                core?.phrases()?.let { list -> main.post { callback(list) } }
            } catch (_: Throwable) {
                main.post { callback(emptyList()) }
            }
        }
    }

    /** 加用户短语；`done` 传 null 表示成功，否则为错误文案。 */
    fun phraseAdd(code: String, text: String, done: (String?) -> Unit) {
        executor.execute {
            val err = try {
                core?.phraseAdd(code, text)
                null
            } catch (e: Throwable) {
                e.message ?: "添加失败"
            }
            main.post { done(err) }
        }
    }

    fun phraseRemove(code: String, text: String, done: (String?) -> Unit) {
        executor.execute {
            val err = try {
                core?.phraseRemove(code, text)
                null
            } catch (e: Throwable) {
                e.message ?: "删除失败"
            }
            main.post { done(err) }
        }
    }

    fun phraseSetEnabled(code: String, text: String, on: Boolean, done: (String?) -> Unit) {
        executor.execute {
            val err = try {
                core?.phraseSetEnabled(code, text, on)
                null
            } catch (e: Throwable) {
                e.message ?: "保存失败"
            }
            main.post { done(err) }
        }
    }

    // ── 9 键 T9 ───────────────────────────────────────────────────────────

    fun t9PushDigit(digit: String, callback: (List<FfiT9Candidate>) -> Unit) {
        executor.execute {
            try {
                core?.t9PushDigit(digit)?.let { list -> main.post { callback(list) } }
            } catch (_: Throwable) {
                main.post { callback(emptyList()) }
            }
        }
    }

    fun t9Backspace(callback: (List<FfiT9Candidate>) -> Unit) {
        executor.execute {
            try {
                core?.t9Backspace()?.let { list -> main.post { callback(list) } }
            } catch (_: Throwable) {
                main.post { callback(emptyList()) }
            }
        }
    }

    fun t9Clear() {
        executor.execute { core?.t9Clear() }
    }

    fun t9Commit(index: Int, callback: (String?) -> Unit) {
        executor.execute {
            try {
                val text = core?.t9Commit(index.toUInt())
                main.post { callback(text) }
            } catch (_: Throwable) {
                main.post { callback(null) }
            }
        }
    }

    /** 9 键提交：走拼音引擎（回放全拼 + 选词），返回编辑指令流。 */
    fun t9CommitOutcome(index: Int, callback: (FfiKeyOutcome?) -> Unit) {
        executor.execute {
            try {
                val out = core?.t9CommitOutcome(index.toUInt())
                main.post { callback(out) }
            } catch (_: Throwable) {
                main.post { callback(null) }
            }
        }
    }

    fun t9BufferLen(callback: (Int) -> Unit) {
        executor.execute {
            val len = try {
                core?.t9BufferLen()?.toInt() ?: 0
            } catch (_: Throwable) {
                0
            }
            main.post { callback(len) }
        }
    }

    fun runCommand(cmd: FfiMobileCommand) {
        executor.execute {
            try {
                core?.runCommand(cmd)
            } catch (_: Throwable) {
                // 初始化未完成：忽略。
            }
        }
    }

    /** 中英切换：中文→英文用 English；英文→中文按方案 id="pinyin" 的下标切回。 */
    fun toggleLanguage(chineseMode: Boolean) {
        if (chineseMode) {
            runCommand(FfiMobileCommand.English)
        } else {
            val idx = schemas.indexOfFirst { it.id == "pinyin" }
            if (idx >= 0) runCommand(FfiMobileCommand.SelectSchema(idx.toUInt()))
        }
    }

    fun focusGained() {
        executor.execute { core?.focusGained() }
    }

    fun focusLost() {
        executor.execute { core?.focusLost() }
    }

    fun setPrecedingText(text: String) {
        executor.execute { core?.setPrecedingText(text) }
    }

    fun setLearningSuspended(on: Boolean) {
        executor.execute { core?.setLearningSuspended(on) }
    }

    fun trimMemory() {
        executor.execute { core?.trimMemory() }
    }

    fun close() {
        executor.execute {
            try {
                core?.close()
            } catch (_: Throwable) {
                // 进程退出路径，忽略。
            }
            core = null
        }
        executor.shutdown()
    }

    private fun execOutcome(block: () -> FfiKeyOutcome?) {
        executor.execute {
            try {
                val out = block() ?: return@execute
                // EditOp 必须在主线程执行（InputConnection 线程约束）。
                main.post { service.executeOutcome(out) }
            } catch (e: Throwable) {
                main.post { onToast?.invoke("输入异常：${e.message}") }
            }
        }
    }

    private inner class EventSink : AndroidEventSink {
        override fun onCandidates(frame: FfiCandidateFrame) {
            main.post { onFrame?.invoke(frame) }
        }

        override fun onHideCandidates() {
            main.post { onHide?.invoke() }
        }

        override fun onStatus(status: FfiInputStatus) {
            main.post { onStatus?.invoke(status) }
        }

        override fun onToast(text: String) {
            main.post { onToast?.invoke(text) }
        }
    }
}
