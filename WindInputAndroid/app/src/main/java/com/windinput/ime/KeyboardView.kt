package com.windinput.ime

import android.content.Context
import android.graphics.drawable.GradientDrawable
import android.media.AudioManager
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.View
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.TextView
import uniffi.wind_android.FfiCandidateFrame
import uniffi.wind_android.FfiCandidateWindow
import uniffi.wind_android.FfiInputStatus
import uniffi.wind_android.FfiMobileCommand
import uniffi.wind_android.FfiPressKey
import uniffi.wind_android.FfiT9Candidate

/**
 * 26 键键盘 + 候选栏 + 工具栏（P1 最小版）。
 *
 * 布局用代码构建；后续换 JSON 数据驱动（方案 §4.6）。
 * 颜色/高度/字号/声音/触感从 [Prefs] 读取，设置页修改后下次弹出键盘生效。
 */
class KeyboardView(
    context: Context,
    private val onChar: (String, Boolean) -> Unit,
    private val onPress: (FfiPressKey) -> Unit,
    private val onCommand: (FfiMobileCommand) -> Unit,
    private val onCandidate: (Int) -> Unit,
    private val onToggleLanguage: (Boolean) -> Unit,
    private val onCandidateLongPress: (Int, String, View) -> Unit,
    private val onMoreCandidates: () -> Unit,
    private val onOpenPanels: () -> Unit,
    private val onOpenSettings: () -> Unit,
    private val onT9Digit: (String) -> Unit,
    private val onT9Space: () -> Unit,
    private val onT9Backspace: () -> Unit,
    private val onT9Candidate: (Int) -> Unit,
    private val onModeChanged: (Boolean) -> Unit,
) : LinearLayout(context) {

    private enum class Action {
        CHAR, SHIFT, BACKSPACE, SPACE, ENTER, LANG, SYMBOL, COMMA, PERIOD,
        T9_DIGIT, T9_SPACE, T9_BACKSPACE,
    }

    private enum class Kind { CHAR, FUNC, PRIMARY, T9 }

    private data class KeySpec(
        val label: String,
        val code: String = label,
        val weight: Float = 1f,
        val action: Action = Action.CHAR,
        val kind: Kind = Kind.CHAR,
    )

    private val dark = Prefs.isDark(context)
    private val bgColor = if (dark) 0xFF1E1E1E.toInt() else 0xFFF2F2F2.toInt()
    private val panelColor = if (dark) 0xFF252525.toInt() else 0xFFFFFFFF.toInt()
    private val keyColor = if (dark) 0xFF3A3A3A.toInt() else 0xFFFFFFFF.toInt()
    private val funcColor = if (dark) 0xFF2C2C2C.toInt() else 0xFFE3E3E3.toInt()
    private val textColor = if (dark) 0xFFEEEEEE.toInt() else 0xFF1A1A1A.toInt()
    private val secondaryColor = if (dark) 0xFFAAAAAA.toInt() else 0xFF888888.toInt()
    private val dividerColor = if (dark) 0xFF333333.toInt() else 0xFFE6E6E6.toInt()
    private val accent = 0xFF07C160.toInt()

    private val candidateSize = Prefs.candidateSize(context).toFloat()
    private val keyboardHeight = Prefs.keyboardHeight(context)

    private val candidateRow = LinearLayout(context)
    private val candidateScroll = HorizontalScrollView(context)
    private val preeditText = TextView(context)
    private val keyboard = LinearLayout(context)

    private var shifted = false
    private var symbolLayer = false
    private var chineseMode = true
    private var nineKey = Prefs.nineKey(context)
    private val t9Digits = StringBuilder()
    private var langButton: TextView? = null
    private var modeButton: TextView? = null

    init {
        orientation = VERTICAL
        setBackgroundColor(bgColor)

        addView(
            buildCandidateArea(),
            LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT),
        )
        addView(buildToolbar(), LayoutParams(LayoutParams.MATCH_PARENT, dp(36)))
        addView(
            View(context).apply { setBackgroundColor(dividerColor) },
            LayoutParams(LayoutParams.MATCH_PARENT, dp(1)),
        )
        addView(keyboard, LayoutParams(LayoutParams.MATCH_PARENT, dp(keyboardHeight)))
        keyboard.orientation = VERTICAL
        keyboard.setPadding(dp(4), dp(4), dp(4), dp(4))
        rebuild()
    }

    // ── 候选区 ────────────────────────────────────────────────────────────

    private fun buildCandidateArea(): View {
        val area = LinearLayout(context).apply {
            orientation = VERTICAL
            setBackgroundColor(panelColor)
        }

        preeditText.apply {
            setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 14f)
            setTextColor(secondaryColor)
            setPadding(dp(12), dp(4), dp(12), dp(2))
            maxLines = 1
        }
        area.addView(preeditText, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))

        candidateRow.orientation = HORIZONTAL
        candidateRow.gravity = Gravity.CENTER_VERTICAL
        candidateRow.setPadding(dp(4), 0, dp(4), 0)

        candidateScroll.apply {
            isHorizontalScrollBarEnabled = false
            isFillViewport = false
            addView(candidateRow, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
        }
        area.addView(candidateScroll, LayoutParams(LayoutParams.MATCH_PARENT, dp(46)))

        area.addView(
            View(context).apply { setBackgroundColor(dividerColor) },
            LayoutParams(LayoutParams.MATCH_PARENT, dp(1)),
        )
        return area
    }

    private fun buildToolbar(): View {
        val bar = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(8), 0, dp(8), 0)
            setBackgroundColor(panelColor)
        }
        modeButton = toolButton(if (nineKey) "26键" else "9键") { toggleMode() }
        bar.addView(modeButton, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT))
        bar.addView(
            toolButton("剪贴板 / 常用语") { onOpenPanels() },
            LayoutParams(0, LayoutParams.MATCH_PARENT, 1f),
        )
        bar.addView(
            toolButton("设置") { onOpenSettings() },
            LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT),
        )
        return bar
    }

    /** 9 键 / 26 键切换（下次弹出键盘也保持）。 */
    private fun toggleMode() {
        nineKey = !nineKey
        Prefs.setNineKey(context, nineKey)
        symbolLayer = false
        t9Digits.setLength(0)
        modeButton?.text = if (nineKey) "26键" else "9键"
        onModeChanged(nineKey)
        hideCandidates()
        rebuild()
    }

    private fun toolButton(text: String, onClick: () -> Unit): TextView =
        TextView(context).apply {
            this.text = text
            gravity = Gravity.CENTER
            setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextColor(secondaryColor)
            isClickable = true
            setPadding(dp(8), 0, dp(8), 0)
            setOnClickListener { onClick() }
        }

    /** 推送帧：更新编码串；无编码无候选时收起候选区。 */
    fun renderFrame(frame: FfiCandidateFrame) {
        // 9 键数字缓冲期间，不被引擎帧覆盖。
        if (nineKey && t9Digits.isNotEmpty()) return
        preeditText.text = frame.preedit
        if (frame.preedit.isEmpty() && !frame.hasCandidates) {
            hideCandidates()
        }
    }

    /** 候选窗口：渲染传入窗口 + 末尾「更多」。 */
    fun renderCandidates(win: FfiCandidateWindow) {
        // 9 键数字缓冲期间，T9 候选优先。
        if (nineKey && t9Digits.isNotEmpty()) return
        candidateRow.removeAllViews()
        candidateScroll.visibility = View.VISIBLE
        val base = win.offset.toInt()
        val selected = win.selected.toInt()

        win.items.forEachIndexed { i, item ->
            val comment = win.comments.getOrNull(i)?.takeIf { it.isNotBlank() }
            val tv = TextView(context).apply {
                text = if (comment != null) "$item  $comment" else item
                setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, candidateSize)
                gravity = Gravity.CENTER
                setPadding(dp(12), 0, dp(12), 0)
                setTextColor(if (base + i == selected) accent else textColor)
                setOnClickListener {
                    tapFeedback()
                    onCandidate(base + i)
                }
                setOnLongClickListener {
                    tapFeedback()
                    onCandidateLongPress(base + i, item, this)
                    true
                }
            }
            candidateRow.addView(
                tv,
                LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT),
            )
        }

        val more = TextView(context).apply {
            text = "更多"
            setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 13f)
            gravity = Gravity.CENTER
            setPadding(dp(12), 0, dp(12), 0)
            setTextColor(secondaryColor)
            setOnClickListener { onMoreCandidates() }
        }
        candidateRow.addView(
            more,
            LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT),
        )
    }

    /** 9 键候选项：编码栏显示数字串，候选栏显示 T9 结果。 */
    fun renderT9Candidates(items: List<FfiT9Candidate>) {
        candidateRow.removeAllViews()
        preeditText.text = t9Digits.toString()
        if (items.isEmpty()) {
            candidateScroll.visibility = View.GONE
            return
        }
        candidateScroll.visibility = View.VISIBLE
        items.forEachIndexed { i, c ->
            val tv = TextView(context).apply {
                text = c.text
                setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, candidateSize)
                gravity = Gravity.CENTER
                setPadding(dp(12), 0, dp(12), 0)
                setTextColor(if (i == 0) accent else textColor)
                setOnClickListener {
                    tapFeedback()
                    onT9Candidate(i)
                }
            }
            candidateRow.addView(
                tv,
                LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.MATCH_PARENT),
            )
        }
    }

    /** 清空 9 键数字缓冲与候选显示（上屏、换输入框、切换模式时调用）。 */
    fun clearT9() {
        t9Digits.setLength(0)
        hideCandidates()
    }

    /** 模式真值：更新语言键显示。 */
    fun renderStatus(status: FfiInputStatus) {
        chineseMode = status.chineseMode
        langButton?.text = if (chineseMode) "中" else "英"
    }

    fun hideCandidates() {
        candidateRow.removeAllViews()
        candidateScroll.visibility = View.GONE
        preeditText.text = ""
    }

    // ── 键盘 ──────────────────────────────────────────────────────────────

    private fun rebuild() {
        keyboard.removeAllViews()
        val rows = when {
            symbolLayer -> symbolRows()
            nineKey -> nineKeyRows()
            else -> letterRows()
        }
        for (keys in rows) {
            keyboard.addView(buildRow(keys), LayoutParams(LayoutParams.MATCH_PARENT, 0, 1f))
        }
    }

    private fun letterRows(): List<List<KeySpec>> = listOf(
        "qwertyuiop".map { charKey(it.toString()) },
        listOf(spacer(0.5f)) + "asdfghjkl".map { charKey(it.toString()) } + listOf(spacer(0.5f)),
        listOf(
            KeySpec("⇧", weight = 1.5f, action = Action.SHIFT, kind = Kind.FUNC),
        ) + "zxcvbnm".map { charKey(it.toString()) } + listOf(
            KeySpec("⌫", weight = 1.5f, action = Action.BACKSPACE, kind = Kind.FUNC),
        ),
        listOf(
            KeySpec("?123", weight = 1.5f, action = Action.SYMBOL, kind = Kind.FUNC),
            KeySpec(if (chineseMode) "中" else "英", weight = 1.5f, action = Action.LANG, kind = Kind.FUNC),
            KeySpec("空格", weight = 4f, action = Action.SPACE, kind = Kind.FUNC),
            KeySpec("，", ",", action = Action.COMMA, kind = Kind.FUNC),
            KeySpec("。", ".", action = Action.PERIOD, kind = Kind.FUNC),
            KeySpec("换行", weight = 1.5f, action = Action.ENTER, kind = Kind.PRIMARY),
        ),
    )

    private fun nineKeyRows(): List<List<KeySpec>> = listOf(
        listOf(
            KeySpec("，", ",", action = Action.COMMA, kind = Kind.FUNC),
            KeySpec("2\nABC", "2", action = Action.T9_DIGIT, kind = Kind.T9),
            KeySpec("3\nDEF", "3", action = Action.T9_DIGIT, kind = Kind.T9),
        ),
        listOf(
            KeySpec("4\nGHI", "4", action = Action.T9_DIGIT, kind = Kind.T9),
            KeySpec("5\nJKL", "5", action = Action.T9_DIGIT, kind = Kind.T9),
            KeySpec("6\nMNO", "6", action = Action.T9_DIGIT, kind = Kind.T9),
        ),
        listOf(
            KeySpec("7\nPQRS", "7", action = Action.T9_DIGIT, kind = Kind.T9),
            KeySpec("8\nTUV", "8", action = Action.T9_DIGIT, kind = Kind.T9),
            KeySpec("9\nWXYZ", "9", action = Action.T9_DIGIT, kind = Kind.T9),
        ),
        listOf(
            KeySpec("符号", weight = 1.0f, action = Action.SYMBOL, kind = Kind.FUNC),
            KeySpec(if (chineseMode) "中" else "英", weight = 1.2f, action = Action.LANG, kind = Kind.FUNC),
            KeySpec("0 空格", "0", weight = 2.0f, action = Action.T9_SPACE, kind = Kind.FUNC),
            KeySpec("⌫", weight = 1.2f, action = Action.T9_BACKSPACE, kind = Kind.FUNC),
            KeySpec("换行", weight = 1.4f, action = Action.ENTER, kind = Kind.PRIMARY),
        ),
    )

    private fun symbolRows(): List<List<KeySpec>> = listOf(
        "1234567890".map { charKey(it.toString()) },
        listOf("@", "#", "$", "%", "&", "*", "(", ")", "-", "+").map { charKey(it) },
        listOf(
            KeySpec("，", ","),
            KeySpec("。", "."),
            KeySpec("？", "?"),
            KeySpec("！", "!"),
            KeySpec("、", ","),
            KeySpec("：", ":"),
            KeySpec("；", ";"),
            KeySpec("“", "\""),
            KeySpec("”", "\""),
        ),
        listOf(
            KeySpec("ABC", weight = 1.5f, action = Action.SYMBOL, kind = Kind.FUNC),
            KeySpec(if (chineseMode) "中" else "英", weight = 1.5f, action = Action.LANG, kind = Kind.FUNC),
            KeySpec("空格", weight = 4f, action = Action.SPACE, kind = Kind.FUNC),
            KeySpec("换行", weight = 1.5f, action = Action.ENTER, kind = Kind.PRIMARY),
        ),
    )

    private fun charKey(ch: String) = KeySpec(
        label = if (shifted && ch.length == 1 && ch[0].isLetter()) ch.uppercase() else ch,
        code = ch,
    )

    private fun spacer(weight: Float) = KeySpec("", weight = weight, action = Action.CHAR)

    private fun buildRow(keys: List<KeySpec>): View {
        val row = LinearLayout(context).apply { orientation = HORIZONTAL }
        for (spec in keys) {
            if (spec.label.isEmpty()) {
                row.addView(View(context), LayoutParams(0, LayoutParams.MATCH_PARENT, spec.weight))
            } else {
                row.addView(makeKey(spec), LayoutParams(0, LayoutParams.MATCH_PARENT, spec.weight))
            }
        }
        return row
    }

    private fun makeKey(spec: KeySpec): TextView {
        val tv = TextView(context).apply {
            text = spec.label
            gravity = Gravity.CENTER
            isClickable = true
            setTextSize(
                android.util.TypedValue.COMPLEX_UNIT_SP,
                when (spec.kind) {
                    Kind.T9 -> 16f
                    Kind.CHAR -> 18f
                    else -> 14f
                },
            )
            setTextColor(if (spec.kind == Kind.PRIMARY) 0xFFFFFFFF.toInt() else textColor)
            background = keyBackground(spec.kind)
            setOnClickListener { handle(spec) }
            if (spec.action == Action.LANG) langButton = this
        }
        val lp = LayoutParams(0, LayoutParams.MATCH_PARENT, spec.weight)
        lp.setMargins(dp(2), dp(2), dp(2), dp(2))
        tv.layoutParams = lp
        return tv
    }

    private fun keyBackground(kind: Kind): GradientDrawable {
        val color = when (kind) {
            Kind.CHAR, Kind.T9 -> keyColor
            Kind.FUNC -> funcColor
            Kind.PRIMARY -> accent
        }
        return GradientDrawable().apply {
            setColor(color)
            cornerRadius = dp(8).toFloat()
        }
    }

    private fun handle(spec: KeySpec) {
        tapFeedback()
        when (spec.action) {
            Action.CHAR -> {
                onChar(spec.code.lowercase(), shifted)
                if (shifted) {
                    shifted = false
                    rebuild()
                }
            }
            Action.SHIFT -> {
                shifted = !shifted
                rebuild()
            }
            Action.BACKSPACE -> onPress(FfiPressKey.Backspace)
            Action.SPACE -> onPress(FfiPressKey.Space)
            Action.ENTER -> onPress(FfiPressKey.Enter)
            Action.LANG -> onToggleLanguage(chineseMode)
            Action.SYMBOL -> {
                symbolLayer = !symbolLayer
                rebuild()
            }
            Action.COMMA -> onChar(",", false)
            Action.PERIOD -> onChar(".", false)
            Action.T9_DIGIT -> {
                t9Digits.append(spec.code)
                onT9Digit(spec.code)
            }
            Action.T9_SPACE -> {
                if (t9Digits.length == 0) onPress(FfiPressKey.Space) else onT9Space()
            }
            Action.T9_BACKSPACE -> {
                if (t9Digits.length > 0) {
                    t9Digits.deleteCharAt(t9Digits.length - 1)
                    onT9Backspace()
                } else {
                    onPress(FfiPressKey.Backspace)
                }
            }
        }
    }

    private fun tapFeedback() {
        if (Prefs.haptic(context)) {
            performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
        }
        if (Prefs.sound(context)) {
            try {
                val am = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
                am.playSoundEffect(AudioManager.FX_KEYPRESS_STANDARD)
            } catch (_: Throwable) {
                // 系统音效不可用时静默。
            }
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
