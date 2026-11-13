package gg.ollie.codmloader

import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.concurrent.thread

class MainActivity : AppCompatActivity() {

    private lateinit var logView: TextView
    private lateinit var packageField: EditText

    private val knownPackages = listOf(
        "com.activision.callofduty.shooter",
        "com.tencent.tmgp.cod",
        "com.garena.game.codm",
        "com.tencent.tmgp.kr.codm",
        "com.vng.codmvn"
    )

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(48, 48, 48, 48)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.MATCH_PARENT
            )
        }

        val title = TextView(this).apply {
            text = "codm loader"
            textSize = 22f
        }
        root.addView(title)

        packageField = EditText(this).apply {
            hint = "game package"
            setText(detectInstalledPackage())
            isSingleLine = true
        }
        root.addView(packageField)

        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        row.addView(makeButton("install") { doInstall() })
        row.addView(makeButton("start") { doStart() })
        row.addView(makeButton("stop") { doStop() })
        row.addView(makeButton("launch") { doLaunch() })
        root.addView(row)

        logView = TextView(this).apply {
            textSize = 12f
            typeface = android.graphics.Typeface.MONOSPACE
            setTextIsSelectable(true)
        }
        val scroll = ScrollView(this).apply {
            addView(logView)
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        }
        root.addView(scroll)

        setContentView(root)
        log("root: " + if (hasRoot()) "granted" else "missing")
        log("assets: " + listOf("frida-server", "codm.js", "codm_config.json")
            .joinToString { if (hasAsset(it)) it else it + "(missing)" })
    }

    private fun makeButton(label: String, action: () -> Unit): Button {
        return Button(this).apply {
            text = label
            setOnClickListener { action() }
        }
    }

    private fun hasAsset(name: String): Boolean {
        return try {
            assets.open(name).close()
            true
        } catch (e: Exception) {
            false
        }
    }

    private fun detectInstalledPackage(): String {
        for (pkg in knownPackages) {
            val intent = packageManager.getLaunchIntentForPackage(pkg)
            if (intent != null) return pkg
        }
        return knownPackages.first()
    }

    private fun log(message: String) {
        runOnUiThread {
            val stamp = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date())
            logView.append("[$stamp] $message\n")
            (logView.parent as? ScrollView)?.fullScroll(View.FOCUS_DOWN)
        }
    }

    private fun hasRoot(): Boolean {
        return try {
            val process = Runtime.getRuntime().exec(arrayOf("su", "-c", "id"))
            val reader = BufferedReader(InputStreamReader(process.inputStream))
            val out = reader.readLine() ?: ""
            process.waitFor()
            out.contains("uid=0")
        } catch (e: Exception) {
            false
        }
    }

    private fun su(command: String): String {
        return try {
            val process = Runtime.getRuntime().exec(arrayOf("su", "-c", command))
            val stdout = BufferedReader(InputStreamReader(process.inputStream))
            val stderr = BufferedReader(InputStreamReader(process.errorStream))
            val text = buildString {
                stdout.forEachLine { appendLine(it) }
                stderr.forEachLine { appendLine(it) }
            }
            process.waitFor()
            text.trim()
        } catch (e: Exception) {
            "error: " + e.message
        }
    }

    private fun stageAssets(): File {
        val dir = File(filesDir, "codm")
        dir.mkdirs()
        val mapping = mapOf(
            "frida-server" to "frida-server",
            "codm.js" to "codm.js",
            "codm_config.json" to "codm_config.json"
        )
        for ((assetName, fileName) in mapping) {
            val target = File(dir, fileName)
            assets.open(assetName).use { input ->
                target.outputStream().use { output -> input.copyTo(output) }
            }
        }
        return dir
    }

    private fun doInstall() {
        thread {
            log("staging assets")
            val dir = stageAssets()
            val server = File(dir, "frida-server")
            log("chmod: " + su("chmod 755 ${server.absolutePath}"))
            log("staged at ${dir.absolutePath}")
        }
    }

    private fun doStart() {
        thread {
            if (!hasRoot()) {
                log("root missing - cannot start frida-server")
                return@thread
            }
            val dir = stageAssets()
            val server = File(dir, "frida-server")
            val script = File(dir, "codm.js").absolutePath
            val config = File(dir, "codm_config.json").absolutePath
            su("chmod 755 ${server.absolutePath}")
            su("pkill -f frida-server")
            su("nohup ${server.absolutePath} >/dev/null 2>&1 &")
            Thread.sleep(1500)
            val pid = su("pidof frida-server")
            if (pid.isBlank()) {
                log("frida-server did not start")
                return@thread
            }
            log("frida-server pid $pid")
            log("script: $script")
            log("config: $config")
            log("now launch the game, then run the host frida session:")
            log("frida -U -f ${packageField.text} -l $script")
        }
    }

    private fun doStop() {
        thread {
            log("stop: " + su("pkill -f frida-server"))
        }
    }

    private fun doLaunch() {
        val pkg = packageField.text.toString().trim()
        val intent = packageManager.getLaunchIntentForPackage(pkg)
        if (intent == null) {
            log("no launch intent for $pkg")
            return
        }
        log("launching $pkg")
        startActivity(intent)
    }
}
