#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "map/map_impl.hpp"
#include "mapkern/map.hpp"
#include "net/http.hpp"
#include "render/sokol.hpp"

namespace {

JavaVM* vm = nullptr;
jclass httpClass = nullptr;
jmethodID httpRequest = nullptr;
jclass responseClass = nullptr;
jfieldID fieldStatus = nullptr, fieldHeaders = nullptr, fieldBody = nullptr, fieldError = nullptr, fieldTimedOut = nullptr;

struct Env {
  JNIEnv* env = nullptr;
  bool attached = false;
  Env() {
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
      vm->AttachCurrentThread(&env, nullptr);
      attached = true;
    }
  }
  ~Env() {
    if (attached) vm->DetachCurrentThread();
  }
};

std::string str(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out(c);
  env->ReleaseStringUTFChars(s, c);
  return out;
}

class JavaHttpClient : public mapkern::HttpClient {
 public:
  mapkern::HttpResponse perform(const mapkern::HttpRequest& request, const mapkern::CancelToken& cancel) override {
    mapkern::HttpResponse out;
    Env e;
    JNIEnv* env = e.env;
    jstring url = env->NewStringUTF(request.url.c_str());
    jobjectArray headers = env->NewObjectArray(static_cast<jsize>(request.headers.size() * 2), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < request.headers.size(); i++) {
      jstring k = env->NewStringUTF(request.headers[i].first.c_str());
      jstring v = env->NewStringUTF(request.headers[i].second.c_str());
      env->SetObjectArrayElement(headers, static_cast<jsize>(i * 2), k);
      env->SetObjectArrayElement(headers, static_cast<jsize>(i * 2 + 1), v);
      env->DeleteLocalRef(k);
      env->DeleteLocalRef(v);
    }
    const jlong start = request.range ? static_cast<jlong>(request.range->first) : -1;
    const jlong length = request.range ? static_cast<jlong>(request.range->second) : -1;
    jobject res = env->CallStaticObjectMethod(httpClass, httpRequest, url, headers, start, length, static_cast<jint>(request.timeoutMs));
    env->DeleteLocalRef(url);
    env->DeleteLocalRef(headers);
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
      out.networkError = true;
      out.error = "Java HTTP request threw";
      return out;
    }
    if (!res) {
      out.networkError = true;
      out.error = "no response";
      return out;
    }
    out.status = env->GetIntField(res, fieldStatus);
    out.timedOut = env->GetBooleanField(res, fieldTimedOut);
    auto error = static_cast<jstring>(env->GetObjectField(res, fieldError));
    if (error) {
      out.networkError = true;
      out.error = str(env, error);
      env->DeleteLocalRef(error);
    }
    auto hs = static_cast<jobjectArray>(env->GetObjectField(res, fieldHeaders));
    if (hs) {
      const jsize n = env->GetArrayLength(hs);
      for (jsize i = 0; i + 1 < n; i += 2) {
        auto k = static_cast<jstring>(env->GetObjectArrayElement(hs, i));
        auto v = static_cast<jstring>(env->GetObjectArrayElement(hs, i + 1));
        out.headers.emplace_back(str(env, k), str(env, v));
        env->DeleteLocalRef(k);
        env->DeleteLocalRef(v);
      }
      env->DeleteLocalRef(hs);
    }
    auto body = static_cast<jbyteArray>(env->GetObjectField(res, fieldBody));
    if (body) {
      const jsize n = env->GetArrayLength(body);
      out.body.resize(static_cast<size_t>(n));
      env->GetByteArrayRegion(body, 0, n, reinterpret_cast<jbyte*>(out.body.data()));
      env->DeleteLocalRef(body);
    }
    env->DeleteLocalRef(res);
    if (cancel.cancelled()) out.cancelled = true;
    return out;
  }
};

class GpuTimer {
 public:
  void reset() {
    checked_ = false;
    supported_ = false;
    std::fill(std::begin(pending_), std::end(pending_), false);
    active_ = false;
  }

  bool enabled = false;

  void begin() {
    if (!enabled) return;
    if (!checked_) {
      checked_ = true;
      const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
      supported_ = ext && std::strstr(ext, "GL_EXT_disjoint_timer_query");
      if (supported_) glGenQueries(kQueries, queries_);
    }
    if (!supported_) return;
    collect();
    if (pending_[next_]) return;
    glBeginQuery(GL_TIME_ELAPSED_EXT, queries_[next_]);
    active_ = true;
  }

  void end() {
    if (!active_) return;
    glEndQuery(GL_TIME_ELAPSED_EXT);
    pending_[next_] = true;
    next_ = (next_ + 1) % kQueries;
    active_ = false;
  }

  std::string take() {
    char out[160];
    std::snprintf(out, sizeof out, "{\"timer\":%s,\"frames\":%llu,\"avgMs\":%.2f,\"maxMs\":%.2f,\"disjoint\":%llu}", supported_ ? "true" : "false",
                  static_cast<unsigned long long>(count_), count_ ? sumMs_ / static_cast<double>(count_) : 0.0, maxMs_,
                  static_cast<unsigned long long>(disjoint_));
    count_ = 0;
    sumMs_ = 0;
    maxMs_ = 0;
    disjoint_ = 0;
    return out;
  }

 private:
  static constexpr int kQueries = 4;

  void collect() {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (int i = 0; i < kQueries; i++) {
      if (!pending_[i]) continue;
      GLuint available = 0;
      glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT_AVAILABLE, &available);
      if (!available) continue;
      GLuint ns = 0;
      glGetQueryObjectuiv(queries_[i], GL_QUERY_RESULT, &ns);
      pending_[i] = false;
      if (disjoint) {
        disjoint_++;
        continue;
      }
      const double ms = static_cast<double>(ns) / 1e6;
      sumMs_ += ms;
      maxMs_ = std::max(maxMs_, ms);
      count_++;
    }
  }

  bool checked_ = false, supported_ = false, active_ = false;
  GLuint queries_[kQueries] = {};
  bool pending_[kQueries] = {};
  int next_ = 0;
  uint64_t count_ = 0, disjoint_ = 0;
  double sumMs_ = 0, maxMs_ = 0;
};

struct Handle {
  std::unique_ptr<mapkern::Map> map;
  jobject view = nullptr;
  jmethodID requestRender = nullptr;
  jmethodID onEvent = nullptr;
  bool graphics = false;
  GpuTimer gpu;
};

jbyteArray bytes(JNIEnv* env, const std::string& s) {
  jbyteArray out = env->NewByteArray(static_cast<jsize>(s.size()));
  env->SetByteArrayRegion(out, 0, static_cast<jsize>(s.size()), reinterpret_cast<const jbyte*>(s.data()));
  return out;
}

Handle* handle(jlong ptr) { return reinterpret_cast<Handle*>(ptr); }

}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* v, void*) {
  vm = v;
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return -1;
  jclass http = env->FindClass("com/grangedevgroup/mapkern/Http");
  httpClass = static_cast<jclass>(env->NewGlobalRef(http));
  httpRequest = env->GetStaticMethodID(httpClass, "request", "(Ljava/lang/String;[Ljava/lang/String;JJI)Lcom/grangedevgroup/mapkern/Http$Response;");
  jclass response = env->FindClass("com/grangedevgroup/mapkern/Http$Response");
  responseClass = static_cast<jclass>(env->NewGlobalRef(response));
  fieldStatus = env->GetFieldID(responseClass, "status", "I");
  fieldHeaders = env->GetFieldID(responseClass, "headers", "[Ljava/lang/String;");
  fieldBody = env->GetFieldID(responseClass, "body", "[B");
  fieldError = env->GetFieldID(responseClass, "error", "Ljava/lang/String;");
  fieldTimedOut = env->GetFieldID(responseClass, "timedOut", "Z");
  mapkern::setHttpClientFactory([] { return std::make_shared<JavaHttpClient>(); });
  mapkern::setUserAgent("Mapkern Native/0.1 (Android)");
  return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeCreate(JNIEnv* env, jobject view, jstring style, jstring cache,
                                                                                        jdouble lng, jdouble lat, jdouble zoom, jboolean globe,
                                                                                        jdouble ratio, jboolean controls) {
  auto* h = new Handle();
  mapkern::MapOptions o;
  o.style = str(env, style);
  o.cacheDirectory = str(env, cache);
  o.lng = lng;
  o.lat = lat;
  o.zoom = zoom;
  o.globe = globe;
  o.pixelRatio = ratio;
  o.controls = controls;
  o.touchControls = true;
  o.view = globe ? "globe" : "terrain";
  h->map = std::make_unique<mapkern::Map>(o);
  h->view = env->NewGlobalRef(view);
  jclass cls = env->GetObjectClass(view);
  h->requestRender = env->GetMethodID(cls, "requestRender", "()V");
  h->onEvent = env->GetMethodID(cls, "onNativeEvent", "(Ljava/lang/String;Ljava/lang/String;)V");
  jobject ref = h->view;
  jmethodID rr = h->requestRender;
  h->map->setInvalidateCallback([ref, rr] {
    Env e;
    e.env->CallVoidMethod(ref, rr);
    if (e.env->ExceptionCheck()) e.env->ExceptionClear();
  });
  jmethodID ev = h->onEvent;
  h->map->on([ref, ev](const mapkern::MapEvent& event) {
    Env e;
    jstring t = e.env->NewStringUTF(event.type.c_str());
    jstring m = e.env->NewStringUTF(event.message.c_str());
    e.env->CallVoidMethod(ref, ev, t, m);
    if (e.env->ExceptionCheck()) e.env->ExceptionClear();
    e.env->DeleteLocalRef(t);
    e.env->DeleteLocalRef(m);
  });
  return reinterpret_cast<jlong>(h);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeDestroy(JNIEnv* env, jobject, jlong ptr) {
  Handle* h = handle(ptr);
  if (!h) return;
  h->map->setInvalidateCallback(nullptr);
  h->map.reset();
  env->DeleteGlobalRef(h->view);
  delete h;
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSurfaceCreated(JNIEnv*, jobject, jlong ptr) {
  Handle* h = handle(ptr);
  if (mapkern::Gpu::get().ready()) mapkern::Gpu::get().shutdown();
  sg_environment env{};
  env.defaults.color_format = SG_PIXELFORMAT_RGBA8;
  env.defaults.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
  env.defaults.sample_count = 1;
  h->map->impl().shutdownGraphics();
  h->map->impl().initGraphicsNative(env);
  h->graphics = true;
  h->gpu.reset();
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeResize(JNIEnv*, jobject, jlong ptr, jint w, jint hgt, jdouble ratio) {
  handle(ptr)->map->resize(w / ratio, hgt / ratio, ratio);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeRender(JNIEnv*, jobject, jlong ptr, jint w, jint hgt) {
  Handle* h = handle(ptr);
  if (!h->graphics) return false;
  sg_swapchain sc{};
  sc.width = w;
  sc.height = hgt;
  sc.sample_count = 1;
  sc.color_format = SG_PIXELFORMAT_RGBA8;
  sc.depth_format = SG_PIXELFORMAT_DEPTH_STENCIL;
  sc.gl.framebuffer = 0;
  h->gpu.begin();
  h->map->impl().renderSwapchain(sc);
  h->gpu.end();
  return h->map->needsRender();
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeDiagnostics(JNIEnv* env, jobject, jlong ptr) {
  Handle* h = handle(ptr);
  const std::string out = "{\"engine\":" + h->map->diagnostics() + ",\"gpu\":" + h->gpu.take() + "}";
  return bytes(env, out);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSetGpuTiming(JNIEnv*, jobject, jlong ptr, jboolean enabled) {
  handle(ptr)->gpu.enabled = enabled;
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSetHud(JNIEnv* env, jobject, jlong ptr, jstring text) {
  std::vector<std::string> lines;
  const std::string all = str(env, text);
  size_t start = 0;
  while (start < all.size()) {
    const size_t end = all.find('\n', start);
    lines.push_back(all.substr(start, end == std::string::npos ? std::string::npos : end - start));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  handle(ptr)->map->setHud(std::move(lines));
}

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeGlInfo(JNIEnv* env, jobject) {
  auto get = [](GLenum name) {
    const char* s = reinterpret_cast<const char*>(glGetString(name));
    return std::string(s ? s : "");
  };
  GLint maxTexture = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
  const std::string out = get(GL_VENDOR) + "\n" + get(GL_RENDERER) + "\n" + get(GL_VERSION) + "\n" + get(GL_SHADING_LANGUAGE_VERSION) + "\n" +
                          std::to_string(maxTexture) + "\n" + get(GL_EXTENSIONS);
  return bytes(env, out);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativePointer(JNIEnv*, jobject, jlong ptr, jint action, jint id, jdouble x,
                                                                                       jdouble y, jdouble time) {
  auto& map = *handle(ptr)->map;
  if (action == 0) map.pointerDown(id, x, y, time);
  else if (action == 1) map.pointerMove(id, x, y, time);
  else if (action == 2) map.pointerUp(id, x, y, time);
  else map.pointerCancel(id);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeZoomBy(JNIEnv*, jobject, jlong ptr, jdouble delta, jdouble x, jdouble y) {
  handle(ptr)->map->zoomBy(delta, x, y, true);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSetStyle(JNIEnv* env, jobject, jlong ptr, jstring style) {
  handle(ptr)->map->setStyle(str(env, style));
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeFlyTo(JNIEnv*, jobject, jlong ptr, jdouble lng, jdouble lat, jdouble zoom,
                                                                                      jdouble bearing, jdouble pitch) {
  mapkern::CameraOptions c;
  c.lng = lng;
  c.lat = lat;
  c.zoom = zoom;
  c.bearing = bearing;
  c.pitch = pitch;
  handle(ptr)->map->flyTo(c);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSetProjection(JNIEnv*, jobject, jlong ptr, jboolean globe) {
  handle(ptr)->map->setProjection(globe);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSetOffline(JNIEnv*, jobject, jlong ptr, jboolean offline) {
  handle(ptr)->map->setOffline(offline);
}

extern "C" JNIEXPORT void JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeSetReachable(JNIEnv*, jclass, jboolean reachable) {
  mapkern::setNetworkReachable(reachable);
}

extern "C" JNIEXPORT jdoubleArray JNICALL Java_com_grangedevgroup_mapkern_MapView_nativeCamera(JNIEnv* env, jobject, jlong ptr) {
  auto& map = *handle(ptr)->map;
  const jdouble values[5] = {map.centerLng(), map.centerLat(), map.zoom(), map.bearing(), map.pitch()};
  jdoubleArray out = env->NewDoubleArray(5);
  env->SetDoubleArrayRegion(out, 0, 5, values);
  return out;
}
