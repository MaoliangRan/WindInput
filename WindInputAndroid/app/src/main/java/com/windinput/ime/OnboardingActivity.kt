package com.windinput.ime

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.view.Gravity
import android.view.ViewGroup
import android.view.inputmethod.InputMethodManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView

/**
 * 首次启动引导（P0 最小版）：
 * 1. 跳系统设置启用输入法；2. 弹出输入法选择器；3. 完成。
 */
class OnboardingActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, dp(32), pad, pad)
            setBackgroundColor(0xFFF2F2F2.toInt())
        }

        root.addView(TextView(this).apply {
            text = "清风输入法"
            textSize = 28f
            setTextColor(0xFF1A1A1A.toInt())
            gravity = Gravity.CENTER_HORIZONTAL
        })

        root.addView(TextView(this).apply {
            text = "轻量、快速、可定制的开源中文输入法\n\n" +
                "1. 启用清风输入法\n" +
                "2. 选择清风输入法\n" +
                "3. 在任意输入框开始打字"
            textSize = 16f
            setTextColor(0xFF666666.toInt())
            setPadding(0, dp(16), 0, dp(16))
        })

        root.addView(button("启用输入法") {
            startActivity(Intent(Settings.ACTION_INPUT_METHOD_SETTINGS))
        })
        root.addView(button("选择输入法") {
            val imm = getSystemService(INPUT_METHOD_SERVICE) as InputMethodManager
            imm.showInputMethodPicker()
        })
        root.addView(button("完成") { finish() })

        setContentView(root)
    }

    private fun button(text: String, onClick: () -> Unit): Button =
        Button(this).apply {
            setText(text)
            setOnClickListener { onClick() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(8) }
        }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
