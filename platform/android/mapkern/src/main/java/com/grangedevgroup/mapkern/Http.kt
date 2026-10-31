package com.grangedevgroup.mapkern

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.net.URL

object Http {
    class Response {
        @JvmField var status: Int = 0
        @JvmField var headers: Array<String> = emptyArray()
        @JvmField var body: ByteArray? = null
        @JvmField var error: String? = null
        @JvmField var timedOut: Boolean = false
    }

    @Volatile var userAgent: String = "Mapkern Native/0.1 (Android)"

    @JvmStatic
    fun request(url: String, headers: Array<String>, rangeStart: Long, rangeLength: Long, timeoutMs: Int): Response {
        val out = Response()
        var connection: HttpURLConnection? = null
        try {
            connection = URL(url).openConnection() as HttpURLConnection
            connection.connectTimeout = timeoutMs
            connection.readTimeout = timeoutMs
            connection.instanceFollowRedirects = true
            connection.useCaches = false
            connection.setRequestProperty("User-Agent", userAgent)
            connection.setRequestProperty("Accept-Encoding", "gzip")
            var i = 0
            while (i + 1 < headers.size) {
                connection.setRequestProperty(headers[i], headers[i + 1])
                i += 2
            }
            if (rangeStart >= 0 && rangeLength > 0) {
                connection.setRequestProperty("Range", "bytes=$rangeStart-${rangeStart + rangeLength - 1}")
            }
            out.status = connection.responseCode
            val list = ArrayList<String>()
            for ((name, values) in connection.headerFields) {
                if (name == null) continue
                list.add(name)
                list.add(values.joinToString(", "))
            }
            out.headers = list.toTypedArray()
            val stream = if (out.status >= 400) connection.errorStream else connection.inputStream
            if (stream != null) {
                val gzip = connection.contentEncoding.equals("gzip", ignoreCase = true)
                val input = if (gzip) java.util.zip.GZIPInputStream(stream) else stream
                input.use {
                    val buffer = ByteArrayOutputStream()
                    val chunk = ByteArray(65536)
                    while (true) {
                        val n = it.read(chunk)
                        if (n < 0) break
                        buffer.write(chunk, 0, n)
                    }
                    out.body = buffer.toByteArray()
                }
            }
        } catch (e: SocketTimeoutException) {
            out.timedOut = true
            out.error = "timed out"
        } catch (e: IOException) {
            out.error = e.message ?: e.javaClass.simpleName
        } catch (e: RuntimeException) {
            out.error = e.message ?: e.javaClass.simpleName
        } finally {
            connection?.disconnect()
        }
        return out
    }
}
