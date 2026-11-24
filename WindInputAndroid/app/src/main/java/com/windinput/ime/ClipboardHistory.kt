package com.windinput.ime

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject

/**
 * 本地剪贴板历史（默认关闭）。
 *
 * - 默认不监听、不落盘；用户在面板/设置里开启后才记录。
 * - 关闭时清空历史（避免留下旧的敏感内容）。
 * - 存储：SharedPreferences 中的 JSON 数组，限制条数。
 */
object ClipboardHistory {
    private const val PREFS = "wind_clip_history"
    private const val KEY = "entries"
    private const val MAX = 80

    data class Entry(val text: String, val time: Long)

    private var prefs: SharedPreferences? = null
    private val entries = mutableListOf<Entry>()

    fun init(context: Context) {
        if (prefs != null) return
        prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        load()
    }

    fun isEnabled(context: Context): Boolean = Prefs.clipboardHistory(context)

    fun setEnabled(context: Context, on: Boolean) {
        Prefs.setClipboardHistory(context, on)
        if (!on) {
            entries.clear()
            persist()
        }
    }

    /** 剪贴板变化时调用；未开启/空白/重复时不记录。 */
    fun record(context: Context, text: String) {
        if (!isEnabled(context) || text.isBlank()) return
        init(context)
        synchronized(entries) {
            entries.removeAll { it.text == text }
            entries.add(0, Entry(text, System.currentTimeMillis()))
            while (entries.size > MAX) entries.removeAt(entries.size - 1)
            persist()
        }
    }

    fun list(): List<Entry> = synchronized(entries) { entries.toList() }

    fun clear(context: Context) {
        init(context)
        synchronized(entries) {
            entries.clear()
            persist()
        }
    }

    fun remove(context: Context, text: String) {
        init(context)
        synchronized(entries) {
            entries.removeAll { it.text == text }
            persist()
        }
    }

    private fun load() {
        val raw = prefs?.getString(KEY, null) ?: return
        try {
            val arr = JSONArray(raw)
            entries.clear()
            for (i in 0 until arr.length()) {
                val o = arr.getJSONObject(i)
                val text = o.optString("text")
                if (text.isNotEmpty()) {
                    entries.add(Entry(text, o.optLong("time", 0L)))
                }
            }
        } catch (_: Throwable) {
            entries.clear()
        }
    }

    private fun persist() {
        val arr = JSONArray()
        for (e in entries) {
            arr.put(JSONObject().put("text", e.text).put("time", e.time))
        }
        prefs?.edit()?.putString(KEY, arr.toString())?.apply()
    }
}
