package com.windinput.ime

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Switch
import android.widget.TextView

/**
 * P1 最小设置页：键盘高度、候选字号、深色模式、声音、触感、剪贴板历史。
 * 全部存 SharedPreferences；修改后重新弹出键盘生效。
 */
class SettingsActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, dp(24), pad, pad)
            setBackgroundColor(0xFFF2F2F2.toInt())
        }

        root.addView(TextView(this).apply {
            text = "设置"
            textSize = 24f
            setTextColor(0xFF1A1A1A.toInt())
        })
        root.addView(TextView(this).apply {
            text = "修改后重新弹出键盘生效"
            textSize = 13f
            setTextColor(0xFF888888.toInt())
            setPadding(0, dp(4), 0, dp(12))
        })

        root.addView(sectionTitle("键盘"))
        root.addView(slider("键盘高度（dp）", 180, 320, Prefs.keyboardHeight(this)) {
            Prefs.setKeyboardHeight(this, it)
        })
        root.addView(slider("候选字号（sp）", 14, 24, Prefs.candidateSize(this)) {
            Prefs.setCandidateSize(this, it)
        })
        root.addView(switchRow("9 键键盘（T9）", Prefs.nineKey(this)) {
            Prefs.setNineKey(this@SettingsActivity, it)
        })

        root.addView(sectionTitle("输入方案"))
        val schemes = SchemaCache.entries
        if (schemes.isEmpty()) {
            root.addView(TextView(this).apply {
                text = "打开一次键盘后可在这里切换方案"
                textSize = 13f
                setTextColor(0xFF888888.toInt())
                setPadding(0, dp(4), 0, dp(8))
            })
        } else {
            for (s in schemes) {
                root.addView(TextView(this).apply {
                    val name = s.name.ifBlank { s.id }
                    text = (if (s.active) "✓ " else "　") + "$name（${s.id}）"
                    textSize = 15f
                    setTextColor(if (s.active) 0xFF07C160.toInt() else 0xFF1A1A1A.toInt())
                    setPadding(0, dp(6), 0, dp(6))
                    isClickable = true
                    setOnClickListener {
                        Prefs.setPendingSchemaId(this@SettingsActivity, s.id)
                        android.widget.Toast.makeText(
                            this@SettingsActivity,
                            "下次弹出键盘生效",
                            android.widget.Toast.LENGTH_SHORT,
                        ).show()
                    }
                })
            }
        }

        root.addView(sectionTitle("显示"))
        val darkGroup = RadioGroup(this).apply {
            orientation = RadioGroup.VERTICAL
        }
        val modes = listOf("跟随系统" to 0, "浅色模式" to 1, "深色模式" to 2)
        for ((label, value) in modes) {
            darkGroup.addView(RadioButton(this).apply {
                text = label
                isChecked = Prefs.darkMode(this@SettingsActivity) == value
                setOnClickListener { Prefs.setDarkMode(this@SettingsActivity, value) }
            })
        }
        root.addView(darkGroup)

        root.addView(sectionTitle("按键"))
        root.addView(switchRow("按键声音", Prefs.sound(this)) {
            Prefs.setSound(this, it)
        })
        root.addView(switchRow("按键触感", Prefs.haptic(this)) {
            Prefs.setHaptic(this, it)
        })

        root.addView(sectionTitle("隐私"))
        root.addView(switchRow("剪贴板历史记录", Prefs.clipboardHistory(this)) {
            ClipboardHistory.setEnabled(this, it)
        })

        root.addView(sectionTitle("关于"))
        root.addView(TextView(this).apply {
            text = "清风输入法 Android\n版本 ${versionName()}\nGPL-3.0-or-later · https://windinput.com"
            textSize = 13f
            setTextColor(0xFF666666.toInt())
            setPadding(0, dp(4), 0, dp(16))
        })

        root.addView(Button(this).apply {
            text = "完成"
            setOnClickListener { finish() }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
            ).apply { topMargin = dp(16) }
        })

        setContentView(ScrollView(this).apply { addView(root) })
    }

    private fun sectionTitle(title: String): TextView = TextView(this).apply {
        text = title
        textSize = 14f
        setTextColor(0xFF888888.toInt())
        setPadding(0, dp(16), 0, dp(6))
    }

    private fun slider(
        label: String,
        min: Int,
        max: Int,
        current: Int,
        onChange: (Int) -> Unit,
    ): LinearLayout {
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val value = TextView(this).apply {
            text = "$label：$current"
            textSize = 15f
            setTextColor(0xFF1A1A1A.toInt())
        }
        box.addView(value)
        box.addView(SeekBar(this).apply {
            this.max = max - min
            progress = current - min
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                    val v = min + progress
                    value.text = "$label：$v"
                    if (fromUser) onChange(v)
                }

                override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
                override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
            })
        })
        return box
    }

    private fun switchRow(label: String, checked: Boolean, onChange: (Boolean) -> Unit): LinearLayout {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(6), 0, dp(6))
        }
        row.addView(TextView(this).apply {
            text = label
            textSize = 15f
            setTextColor(0xFF1A1A1A.toInt())
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        row.addView(Switch(this).apply {
            isChecked = checked
            setOnCheckedChangeListener { _, on -> onChange(on) }
        })
        return row
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    private fun versionName(): String = try {
        packageManager.getPackageInfo(packageName, 0).versionName ?: "-"
    } catch (_: Throwable) {
        "-"
    }
}
