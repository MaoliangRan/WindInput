package com.windinput.ime

import android.app.Activity
import android.os.Bundle
import android.text.InputType
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * 仅 debug 构建包含的输入法测试页：覆盖常见 inputType / imeOptions。
 * 用 `adb shell am start -n com.windinput.ime/.TestActivity` 打开。
 */
class TestActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, dp(24), pad, pad)
        }

        val status = TextView(this).apply {
            text = "聚焦一个输入框后显示 EditorInfo"
            textSize = 12f
            setTextColor(0xFF888888.toInt())
            setPadding(0, dp(8), 0, dp(8))
        }

        root.addView(title("清风输入法 · 测试页"))
        root.addView(status)

        root.addView(field(status, "普通文本", InputType.TYPE_CLASS_TEXT))
        root.addView(
            field(
                status,
                "密码",
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD,
            ),
        )
        root.addView(field(status, "数字", InputType.TYPE_CLASS_NUMBER))
        root.addView(
            field(
                status,
                "邮箱",
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS,
            ),
        )
        root.addView(
            field(
                status,
                "URL",
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI,
            ),
        )
        root.addView(
            field(
                status,
                "多行（回车换行）",
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE,
                lines = 4,
                singleLine = false,
            ),
        )
        root.addView(
            field(
                status,
                "搜索（imeOptions=actionSearch）",
                InputType.TYPE_CLASS_TEXT,
                imeOptions = EditorInfo.IME_ACTION_SEARCH,
            ),
        )
        root.addView(
            field(
                status,
                "发送（imeOptions=actionSend）",
                InputType.TYPE_CLASS_TEXT,
                imeOptions = EditorInfo.IME_ACTION_SEND,
            ),
        )

        setContentView(ScrollView(this).apply { addView(root) })
    }

    private fun title(text: String): TextView = TextView(this).apply {
        this.text = text
        textSize = 22f
        setTextColor(0xFF1A1A1A.toInt())
    }

    private fun field(
        status: TextView,
        label: String,
        inputType: Int,
        imeOptions: Int = EditorInfo.IME_ACTION_NONE,
        lines: Int = 1,
        singleLine: Boolean = true,
    ): LinearLayout {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        box.addView(TextView(this).apply {
            text = label
            textSize = 13f
            setTextColor(0xFF666666.toInt())
            setPadding(0, dp(8), 0, dp(2))
        })
        box.addView(
            EditText(this).apply {
                this.inputType = inputType
                this.imeOptions = imeOptions
                hint = label
                setSingleLine(singleLine)
                minLines = lines
                setOnFocusChangeListener { _, hasFocus ->
                    if (hasFocus) {
                        status.text = "inputType=0x${Integer.toHexString(inputType)}  " +
                            "imeOptions=0x${Integer.toHexString(imeOptions)}  " +
                            "field=$label"
                    }
                }
            },
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ),
        )
        return box
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
