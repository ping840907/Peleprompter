package com.peleprompter.util

import android.util.Xml
import org.xmlpull.v1.XmlPullParser
import java.io.InputStream
import java.net.URLDecoder
import java.util.zip.ZipInputStream

/**
 * Extracts plain text from an EPUB file.
 *
 * EPUB is a ZIP archive. The reading order is declared in an OPF manifest/spine,
 * which is located via META-INF/container.xml. Each spine item is an XHTML file
 * whose tags are stripped to produce readable text.
 *
 * No external dependencies — uses only built-in Android/Java APIs.
 */
object EpubTextExtractor {

    /**
     * Extract all body text from [inputStream] and return it as a single String.
     * Chapters are separated by a blank line.
     */
    fun extract(inputStream: InputStream): String {
        // Read every entry in the ZIP into memory (EPUB files are typically small)
        val entries = mutableMapOf<String, ByteArray>()
        ZipInputStream(inputStream.buffered()).use { zip ->
            var entry = zip.nextEntry
            while (entry != null) {
                if (!entry.isDirectory) entries[entry.name] = zip.readBytes()
                zip.closeEntry()
                entry = zip.nextEntry
            }
        }

        val containerXml = entries["META-INF/container.xml"] ?: return ""
        val opfPath = parseOpfPath(containerXml).takeIf { it.isNotEmpty() } ?: return ""
        val opfDir  = opfPath.substringBeforeLast("/", "")

        val opfBytes = entries[opfPath] ?: return ""
        val (manifest, spine) = parseOpf(opfBytes)

        val sb = StringBuilder()
        for (idref in spine) {
            val rawHref = manifest[idref] ?: continue
            // 去除錨點片段（#chapter1）後再解碼，避免在 entries map 查無此鍵
            val href = URLDecoder.decode(rawHref.substringBefore('#'), "UTF-8")
            // Resolve relative href against the OPF directory，並正規化 ../ 與 ./
            val resolved = normalizePath(if (opfDir.isEmpty()) href else "$opfDir/$href")
            val bytes = entries[resolved]
                ?: entries[href]
                ?: entries[normalizePath(href)]
                ?: continue
            val text  = htmlToText(String(bytes, Charsets.UTF_8))
            if (text.isNotBlank()) {
                sb.append(text)
                sb.append("\n\n")
            }
        }

        return sb.toString().trim()
    }

    /**
     * 正規化 ZIP 內部路徑：解析並移除 "." 與 ".." 片段，
     * 使相對 href（如 "../Text/ch1.xhtml"）能對應到實際的 entry 鍵。
     */
    private fun normalizePath(path: String): String {
        val stack = ArrayList<String>()
        for (seg in path.split('/')) {
            when (seg) {
                "", "." -> {}
                ".."     -> if (stack.isNotEmpty()) stack.removeAt(stack.size - 1)
                else     -> stack.add(seg)
            }
        }
        return stack.joinToString("/")
    }

    // ── XML parsers ───────────────────────────────────────────────────

    /** Parse META-INF/container.xml and return the path to the OPF root file. */
    private fun parseOpfPath(xml: ByteArray): String {
        val parser = Xml.newPullParser()
        parser.setInput(xml.inputStream(), null)
        var event = parser.eventType
        while (event != XmlPullParser.END_DOCUMENT) {
            if (event == XmlPullParser.START_TAG &&
                parser.name.equals("rootfile", ignoreCase = true)) {
                return parser.getAttributeValue(null, "full-path") ?: ""
            }
            event = parser.next()
        }
        return ""
    }

    /**
     * Parse the OPF file and return:
     *  - manifest: map of item id → href (only HTML/XHTML items)
     *  - spine:    ordered list of idrefs
     */
    private fun parseOpf(xml: ByteArray): Pair<Map<String, String>, List<String>> {
        val manifest = mutableMapOf<String, String>()
        val spine    = mutableListOf<String>()
        var inManifest = false
        var inSpine    = false

        val parser = Xml.newPullParser()
        parser.setInput(xml.inputStream(), null)
        var event = parser.eventType
        while (event != XmlPullParser.END_DOCUMENT) {
            when (event) {
                XmlPullParser.START_TAG -> when {
                    parser.name.equals("manifest", ignoreCase = true) -> inManifest = true
                    parser.name.equals("spine",    ignoreCase = true) -> inSpine    = true
                    parser.name.equals("item",     ignoreCase = true) && inManifest -> {
                        val id        = parser.getAttributeValue(null, "id")         ?: ""
                        val href      = parser.getAttributeValue(null, "href")        ?: ""
                        val mediaType = parser.getAttributeValue(null, "media-type")  ?: ""
                        if (id.isNotEmpty() && href.isNotEmpty() &&
                            (mediaType.contains("html", ignoreCase = true) ||
                             href.endsWith(".html",  ignoreCase = true) ||
                             href.endsWith(".xhtml", ignoreCase = true))) {
                            manifest[id] = href
                        }
                    }
                    parser.name.equals("itemref", ignoreCase = true) && inSpine -> {
                        val idref = parser.getAttributeValue(null, "idref")
                        if (!idref.isNullOrEmpty()) spine.add(idref)
                    }
                }
                XmlPullParser.END_TAG -> when {
                    parser.name.equals("manifest", ignoreCase = true) -> inManifest = false
                    parser.name.equals("spine",    ignoreCase = true) -> inSpine    = false
                }
            }
            event = parser.next()
        }

        return manifest to spine
    }

    // ── HTML → plain text ─────────────────────────────────────────────

    private fun htmlToText(html: String): String = html
        // Remove style and script blocks entirely
        .replace(Regex("<style[^>]*>[\\s\\S]*?</style>",   RegexOption.IGNORE_CASE), "")
        .replace(Regex("<script[^>]*>[\\s\\S]*?</script>", RegexOption.IGNORE_CASE), "")
        // Block-level closing tags → newline
        .replace(Regex("<br\\s*/?>", RegexOption.IGNORE_CASE), "\n")
        .replace(Regex("</(p|div|h[1-6]|li|tr|td|th|blockquote|section|article|header|footer)>",
                       RegexOption.IGNORE_CASE), "\n")
        // Strip remaining tags
        .replace(Regex("<[^>]+>"), "")
        // Decode common HTML entities
        .replace("&amp;",  "&")
        .replace("&lt;",   "<")
        .replace("&gt;",   ">")
        .replace("&quot;", "\"")
        .replace("&apos;", "'")
        .replace("&nbsp;", " ")
        .replace(Regex("&#(\\d+);")) { m ->
            m.groupValues[1].toIntOrNull()
                ?.takeIf { it in 32..0xFFFF }
                ?.toChar()?.toString() ?: ""
        }
        .replace(Regex("&[a-zA-Z]+;"), " ")
        // Normalise whitespace
        .replace(Regex("[ \\t]+"), " ")
        .replace(Regex("(\\s*\\n){3,}"), "\n\n")
        .trim()
}
