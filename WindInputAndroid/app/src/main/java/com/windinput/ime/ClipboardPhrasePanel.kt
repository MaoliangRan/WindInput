package com.windinput.ime

import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.text.InputType
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.Switch
import android.widget.TextView
import uniffi.wind_android.FfiPhrase

/**
 * 剪贴板 / 常用语面板：同一个 Dialog，顶部 Tab 切换。
 *
 * - 剪贴板历史默认关闭；开启后才记录，关闭时清空。
 * - 常用语复用核心短语系统（`phrases/phrase_add/phrase_remove`），点击直接上屏。
 */
object ClipboardPhrasePanel {

    fun show(service: WindImeService, bridge: RustBridge) {
        val ctx: Context = service
        val dialog = Dialog(ctx)
        val pad = dp(ctx, 16)

        val root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
        }

        val tabRow = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        val clipTab = tab(ctx, "剪贴板")
        val phraseTab = tab(ctx, "常用语")
        tabRow.addView(clipTab, LinearLayout.LayoutParams(0, dp(ctx, 42), 1f))
        tabRow.addView(phraseTab, LinearLayout.LayoutParams(0, dp(ctx, 42), 1f))
        root.addView(tabRow)

        val content = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(ctx, 8), 0, 0)
        }
        root.addView(content, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        fun selectClipboard() {
            clipTab.setTextColor(0xFF07C160.toInt())
            phraseTab.setTextColor(0xFF888888.toInt())
            content.removeAllViews()
            buildClipboardTab(ctx, service, content) { dialog.dismiss() }
        }

        fun selectPhrases() {
            phraseTab.setTextColor(0xFF07C160.toInt())
            clipTab.setTextColor(0xFF888888.toInt())
            content.removeAllViews()
            buildPhraseTab(ctx, service, bridge, content) { dialog.dismiss() }
        }

        clipTab.setOnClickListener { selectClipboard() }
        phraseTab.setOnClickListener { selectPhrases() }
        selectClipboard()

        dialog.setContentView(root)
        dialog.show()
        dialog.window?.setLayout(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(ctx, 460),
        )
    }

    // ── 剪贴板 Tab ────────────────────────────────────────────────────────

    private fun buildClipboardTab(
        ctx: Context,
        service: WindImeService,
        parent: LinearLayout,
        dismiss: () -> Unit,
    ) {
        val switch = Switch(ctx).apply {
            text = "记录剪贴板历史"
            isChecked = ClipboardHistory.isEnabled(ctx)
        }
        parent.addView(switch)

        val hint = TextView(ctx).apply {
            setTextColor(0xFF888888.toInt())
            setTextSize(12f)
            text = "默认关闭。开启后仅在本机记录最近 80 条，可在下方清空。"
            setPadding(0, dp(ctx, 4), 0, dp(ctx, 8))
        }
        parent.addView(hint)

        val list = ListView(ctx)
        parent.addView(list, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        fun refresh() {
            val items = if (ClipboardHistory.isEnabled(ctx)) ClipboardHistory.list() else emptyList()
            val labels = items.map { it.text.replace("\n", " ⏎ ") }
            list.adapter = ArrayAdapter(
                ctx,
                android.R.layout.simple_list_item_1,
                labels,
            )
            list.setOnItemClickListener { _, _, position, _ ->
                items.getOrNull(position)?.let {
                    service.commitText(it.text)
                    dismiss()
                }
            }
            list.setOnItemLongClickListener { _, _, position, _ ->
                val entry = items.getOrNull(position) ?: return@setOnItemLongClickListener false
                AlertDialog.Builder(ctx)
                    .setMessage("删除这条剪贴板记录？")
                    .setPositiveButton("删除") { _, _ ->
                        ClipboardHistory.remove(ctx, entry.text)
                        refresh()
                    }
                    .setNegativeButton("取消", null)
                    .show()
                true
            }
        }

        switch.setOnCheckedChangeListener { _, checked ->
            ClipboardHistory.setEnabled(ctx, checked)
            refresh()
        }

        val clear = Button(ctx).apply {
            text = "清空历史"
            setOnClickListener {
                ClipboardHistory.clear(ctx)
                refresh()
            }
        }
        parent.addView(clear)
        refresh()
    }

    // ── 常用语 Tab ────────────────────────────────────────────────────────

    private fun buildPhraseTab(
        ctx: Context,
        service: WindImeService,
        bridge: RustBridge,
        parent: LinearLayout,
        dismiss: () -> Unit,
    ) {
        val list = ListView(ctx)
        parent.addView(list, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        var phrases: List<FfiPhrase> = emptyList()

        fun refresh() {
            bridge.phrases { loaded ->
                phrases = loaded
                val labels = loaded.map { p ->
                    val prefix = if (p.isSystem) "[系统] " else ""
                    val disabled = if (p.enabled) "" else "（停用）"
                    "$prefix${p.code} → ${p.preview.replace("\n", " ⏎ ")}$disabled"
                }
                list.adapter = ArrayAdapter(
                    ctx,
                    android.R.layout.simple_list_item_1,
                    labels,
                )
            }
        }

        list.setOnItemClickListener { _, _, position, _ ->
            phrases.getOrNull(position)?.let {
                service.commitText(it.preview)
                dismiss()
            }
        }
        list.setOnItemLongClickListener { _, _, position, _ ->
            val p = phrases.getOrNull(position) ?: return@setOnItemLongClickListener false
            if (p.isSystem) {
                // 系统短语只允许启用/停用，不允许删除。
                AlertDialog.Builder(ctx)
                    .setTitle(p.code)
                    .setItems(arrayOf(if (p.enabled) "停用" else "启用")) { _, _ ->
                        bridge.phraseSetEnabled(p.code, p.text, !p.enabled) { refresh() }
                    }
                    .show()
            } else {
                AlertDialog.Builder(ctx)
                    .setTitle(p.code)
                    .setItems(arrayOf("删除")) { _, _ ->
                        bridge.phraseRemove(p.code, p.text) { refresh() }
                    }
                    .show()
            }
            true
        }

        val add = Button(ctx).apply {
            text = "添加常用语"
            setOnClickListener {
                val form = LinearLayout(ctx).apply {
                    orientation = LinearLayout.VERTICAL
                    setPadding(dp(ctx, 16), dp(ctx, 8), dp(ctx, 16), 0)
                }
                val code = EditText(ctx).apply {
                    hint = "编码（如 sig）"
                    inputType = InputType.TYPE_CLASS_TEXT
                }
                val text = EditText(ctx).apply {
                    hint = "内容（如 此致 敬礼）"
                    inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
                }
                form.addView(code)
                form.addView(text)
                AlertDialog.Builder(ctx)
                    .setTitle("添加常用语")
                    .setView(form)
                    .setPositiveButton("保存") { _, _ ->
                        bridge.phraseAdd(code.text.toString(), text.text.toString()) { err ->
                            if (err != null) {
                                android.widget.Toast.makeText(ctx, err, android.widget.Toast.LENGTH_SHORT).show()
                            }
                            refresh()
                        }
                    }
                    .setNegativeButton("取消", null)
                    .show()
            }
        }
        parent.addView(add)
        refresh()
    }

    // ── 小工具 ────────────────────────────────────────────────────────────

    private fun tab(ctx: Context, text: String): TextView = TextView(ctx).apply {
        this.text = text
        gravity = Gravity.CENTER
        setTextSize(15f)
        setTextColor(0xFF888888.toInt())
        isClickable = true
    }

    private fun dp(c: Context, v: Int): Int = (v * c.resources.displayMetrics.density).toInt()
}
