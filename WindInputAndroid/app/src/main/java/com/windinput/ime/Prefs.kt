package com.windinput.ime

import android.content.Context
import android.content.res.Configuration

/**
 * P1 最小设置（SharedPreferences）。
 * 这些是纯 UI/输入法偏好，先本地存；后续接核心 config_api 时可迁到 TOML。
 */
object Prefs {
    private const val NAME = "wind_prefs"

    private fun sp(c: Context) = c.getSharedPreferences(NAME, Context.MODE_PRIVATE)

    // 键盘高度（dp）
    fun keyboardHeight(c: Context): Int = sp(c).getInt("kb_height", 232)
    fun setKeyboardHeight(c: Context, v: Int) =
        sp(c).edit().putInt("kb_height", v).apply()

    // 候选字号（sp）
    fun candidateSize(c: Context): Int = sp(c).getInt("candidate_size", 18)
    fun setCandidateSize(c: Context, v: Int) =
        sp(c).edit().putInt("candidate_size", v).apply()

    // 0=跟随系统 1=浅色 2=深色
    fun darkMode(c: Context): Int = sp(c).getInt("dark_mode", 0)
    fun setDarkMode(c: Context, v: Int) =
        sp(c).edit().putInt("dark_mode", v).apply()

    fun sound(c: Context): Boolean = sp(c).getBoolean("sound", false)
    fun setSound(c: Context, v: Boolean) =
        sp(c).edit().putBoolean("sound", v).apply()

    fun haptic(c: Context): Boolean = sp(c).getBoolean("haptic", true)
    fun setHaptic(c: Context, v: Boolean) =
        sp(c).edit().putBoolean("haptic", v).apply()

    // 剪贴板历史默认关闭（隐私优先）
    fun clipboardHistory(c: Context): Boolean = sp(c).getBoolean("clip_history", false)
    fun setClipboardHistory(c: Context, v: Boolean) =
        sp(c).edit().putBoolean("clip_history", v).apply()

    // 是否使用 9 键键盘（默认 26 键）
    fun nineKey(c: Context): Boolean = sp(c).getBoolean("nine_key", false)
    fun setNineKey(c: Context, v: Boolean) =
        sp(c).edit().putBoolean("nine_key", v).apply()

    // 是否已完成「首次启动选拼音」的一次性初始化（之后尊重用户在设置/语言键里的选择）
    fun schemaInitialized(c: Context): Boolean = sp(c).getBoolean("schema_initialized", false)
    fun setSchemaInitialized(c: Context, v: Boolean) =
        sp(c).edit().putBoolean("schema_initialized", v).apply()

    // 设置页选择的方案：下次输入法初始化时应用
    fun pendingSchemaId(c: Context): String? = sp(c).getString("pending_schema", null)
    fun setPendingSchemaId(c: Context, id: String?) {
        val e = sp(c).edit()
        if (id == null) e.remove("pending_schema") else e.putString("pending_schema", id)
        e.apply()
    }

    fun isDark(c: Context): Boolean = when (darkMode(c)) {
        1 -> false
        2 -> true
        else -> (c.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) ==
            Configuration.UI_MODE_NIGHT_YES
    }
}
