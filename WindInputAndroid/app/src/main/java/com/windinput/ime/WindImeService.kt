package com.windinput.ime

import android.content.ClipData
import android.content.ClipboardManager
import android.content.ComponentCallbacks2
import android.content.Intent
import android.inputmethodservice.InputMethodService
import android.text.InputType
import android.view.KeyEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.widget.PopupMenu
import uniffi.wind_android.FfiEditOp
import uniffi.wind_android.FfiKeyOutcome
import uniffi.wind_android.FfiPressKey

/**
 * 输入法服务（P0 骨架）。
 *
 * 线程：按键 → RustBridge 单线程执行 → 主线程执行 EditOp。
 * 这里只做 Android 生命周期/InputConnection 适配，不做任何中英判定。
 */
class WindImeService : InputMethodService() {

    private lateinit var bridge: RustBridge
    private lateinit var keyboard: KeyboardView

    /** Enter 键的默认行为要等 Rust 返回后再决定（是否消费）。 */
    private var pendingEnter = false

    override fun onCreate() {
        super.onCreate()
        bridge = RustBridge(this)
        bridge.onFrame = { frame ->
            if (::keyboard.isInitialized) {
                keyboard.renderFrame(frame)
                refreshCandidates()
            }
        }
        bridge.onStatus = { status ->
            if (::keyboard.isInitialized) keyboard.renderStatus(status)
        }
        bridge.onToast = { text ->
            android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show()
        }
        bridge.onHide = {
            if (::keyboard.isInitialized) keyboard.hideCandidates()
        }
        bridge.start()
    }

    override fun onCreateInputView(): View {
        keyboard = KeyboardView(
            this,
            onChar = { ch, shifted -> bridge.pressChar(ch, shifted) },
            onPress = { key ->
                if (key == FfiPressKey.Enter) pendingEnter = true
                bridge.pressKey(key)
            },
            onCommand = { cmd -> bridge.runCommand(cmd) },
            onCandidate = { index -> bridge.selectCandidate(index) },
            onToggleLanguage = { chinese -> bridge.toggleLanguage(chinese) },
            onCandidateLongPress = { index, text, anchor ->
                showCandidateMenu(index, text, anchor)
            },
            onMoreCandidates = { CandidatePanel.show(this, bridge) },
            onOpenPanels = { ClipboardPhrasePanel.show(this, bridge) },
            onOpenSettings = {
                startActivity(Intent(this, SettingsActivity::class.java))
            },
            onT9Digit = { digit ->
                bridge.t9PushDigit(digit) { list -> keyboard.renderT9Candidates(list) }
            },
            onT9Space = {
                bridge.t9BufferLen { len ->
                    if (len > 0) {
                        bridge.t9CommitOutcome(0) { out ->
                            if (out != null) {
                                executeOutcome(out)
                                keyboard.clearT9()
                            }
                        }
                    } else {
                        bridge.pressKey(FfiPressKey.Space)
                    }
                }
            },
            onT9Backspace = {
                bridge.t9Backspace { list -> keyboard.renderT9Candidates(list) }
            },
            onT9Candidate = { index ->
                bridge.t9CommitOutcome(index) { out ->
                    if (out != null) {
                        executeOutcome(out)
                        keyboard.clearT9()
                    }
                }
            },
            onModeChanged = { nine ->
                bridge.t9Clear()
                keyboard.clearT9()
                if (!nine) refreshCandidates()
            },
        )
        return keyboard
    }

    override fun onStartInput(info: EditorInfo?, restarting: Boolean) {
        super.onStartInput(info, restarting)
        bridge.focusGained()
        bridge.t9Clear()
        if (::keyboard.isInitialized) keyboard.clearT9()
        currentInputConnection?.let { bridge.setPrecedingText(readBefore(it)) }

        val noLearning = info != null && (
            (info.imeOptions and EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING) != 0 ||
                isPassword(info.inputType)
            )
        bridge.setLearningSuspended(noLearning)
    }

    override fun onUpdateSelection(
        oldSelStart: Int,
        oldSelEnd: Int,
        newSelStart: Int,
        newSelEnd: Int,
        candidatesStart: Int,
        candidatesEnd: Int,
    ) {
        super.onUpdateSelection(
            oldSelStart, oldSelEnd, newSelStart, newSelEnd, candidatesStart, candidatesEnd,
        )
        currentInputConnection?.let { bridge.setPrecedingText(readBefore(it)) }
    }

    override fun onFinishInput() {
        bridge.focusLost()
        bridge.t9Clear()
        if (::keyboard.isInitialized) keyboard.clearT9()
        super.onFinishInput()
    }

    override fun onTrimMemory(level: Int) {
        super.onTrimMemory(level)
        if (level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW) {
            bridge.trimMemory()
        }
    }

    override fun onDestroy() {
        bridge.close()
        super.onDestroy()
    }

    /** 输入法自己管理高度，不启用系统全屏提取模式。 */
    override fun onEvaluateFullscreenMode(): Boolean = false

    /** 告诉应用内容区到键盘顶部为止，避免被键盘遮挡。 */
    override fun onComputeInsets(outInsets: InputMethodService.Insets) {
        super.onComputeInsets(outInsets)
        outInsets.contentTopInsets = outInsets.visibleTopInsets
    }

    /** 执行核心下发的编辑指令（主线程）。 */
    fun executeOutcome(outcome: FfiKeyOutcome) {
        val ic = currentInputConnection
        if (ic == null) {
            pendingEnter = false
            return
        }
        try {
            if (outcome.ops.isNotEmpty()) {
                ic.beginBatchEdit()
                try {
                    for (op in outcome.ops) {
                        when (op) {
                            is FfiEditOp.Commit -> ic.commitText(op.text, 1)
                            is FfiEditOp.SetComposition -> {
                                if (op.text.isEmpty()) ic.finishComposingText()
                                else ic.setComposingText(op.text, 1)
                            }
                            is FfiEditOp.DeleteBackward ->
                                ic.deleteSurroundingTextInCodePoints(op.count.toInt(), 0)
                            is FfiEditOp.ReplaceBackward -> {
                                ic.deleteSurroundingTextInCodePoints(op.count.toInt(), 0)
                                ic.commitText(op.text, 1)
                            }
                            is FfiEditOp.MoveCursor -> moveCursor(ic, op.delta)
                        }
                    }
                } finally {
                    ic.endBatchEdit()
                }
            }

            // Enter：核心未消费时执行编辑器默认动作（搜索/发送/下一项…）。
            if (pendingEnter) {
                pendingEnter = false
                if (!outcome.consumed && outcome.ops.isEmpty()) {
                    if (!sendDefaultEditorAction(true)) {
                        ic.commitText("\n", 1)
                    }
                }
            }
        } catch (_: Throwable) {
            // InputConnection 失效（切 App/输入框销毁）：丢弃本批 ops，等下一次焦点重置。
            pendingEnter = false
        }
    }

    /** 面板/常用语/剪贴板直接上屏（绕过引擎，不触发候选/学习）。 */
    fun commitText(text: String) {
        val ic = currentInputConnection ?: return
        try {
            ic.commitText(text, 1)
        } catch (_: Throwable) {
            // InputConnection 失效：忽略。
        }
    }

    /** 候选长按菜单：动作由核心给出，`copy` 由宿主写剪贴板。 */
    private fun showCandidateMenu(index: Int, text: String, anchor: View) {
        bridge.candidateMenu(index) { items ->
            if (items.isEmpty()) return@candidateMenu
            val popup = PopupMenu(this, anchor)
            items.forEachIndexed { i, item ->
                popup.menu.add(0, i, i, item.label).isEnabled = item.enabled
            }
            popup.setOnMenuItemClickListener { mi ->
                val item = items.getOrNull(mi.itemId) ?: return@setOnMenuItemClickListener false
                if (item.action == "copy") {
                    copyText(text)
                }
                bridge.candidateMenuRun(index, item.action) { refreshCandidates() }
                true
            }
            popup.show()
        }
    }

    private fun copyText(text: String) {
        try {
            val cm = getSystemService(CLIPBOARD_SERVICE) as ClipboardManager
            cm.setPrimaryClip(ClipData.newPlainText("windinput", text))
        } catch (_: Throwable) {
            // 剪贴板不可用：忽略。
        }
    }

    private fun refreshCandidates() {
        bridge.candidates(0, 30) { win -> keyboard.renderCandidates(win) }
    }

    private fun readBefore(ic: InputConnection): String =
        ic.getTextBeforeCursor(64, 0)?.toString() ?: ""

    private fun moveCursor(ic: InputConnection, delta: Int) {
        val key = if (delta < 0) KeyEvent.KEYCODE_DPAD_LEFT else KeyEvent.KEYCODE_DPAD_RIGHT
        repeat(kotlin.math.abs(delta)) {
            ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, key))
            ic.sendKeyEvent(KeyEvent(KeyEvent.ACTION_UP, key))
        }
    }

    private fun isPassword(type: Int): Boolean {
        val cls = type and InputType.TYPE_MASK_CLASS
        val variation = type and InputType.TYPE_MASK_VARIATION
        return cls == InputType.TYPE_CLASS_TEXT && (
            variation == InputType.TYPE_TEXT_VARIATION_PASSWORD ||
                variation == InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD ||
                variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD
            )
    }
}
