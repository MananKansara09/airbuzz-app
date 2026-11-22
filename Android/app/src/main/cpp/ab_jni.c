/* JNI bridge between com.tavi.airbuzz.AirNative and the AB_Session C core.
 * Core callbacks run on the Golden Gate loop thread, so each one attaches
 * that thread to the JVM for the duration of the call. */
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include "ab_session.h"

/* Android declares AttachCurrentThread with JNIEnv**, desktop JDKs with void**.
 * The host build lets the bridge be exercised from a plain JVM in tests. */
#ifdef __ANDROID__
#define AB_ATTACH_ARG(env) (env)
#else
#define AB_ATTACH_ARG(env) ((void**)(env))
#endif

typedef struct {
    JavaVM* vm;
    jobject sink;
    jmethodID send, done, log;
    AB_Session* session;
} Bridge;

static JNIEnv* attach(Bridge* b, int* attached) {
    JNIEnv* env = NULL;
    *attached = 0;
    if ((*b->vm)->GetEnv(b->vm, (void**)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*b->vm)->AttachCurrentThread(b->vm, AB_ATTACH_ARG(&env), NULL) != JNI_OK) return NULL;
    *attached = 1;
    return env;
}

static void detach(Bridge* b, JNIEnv* env, int attached) {
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (attached) (*b->vm)->DetachCurrentThread(b->vm);
}

static void on_send(void* raw, const uint8_t* bytes, size_t size) {
    Bridge* b = raw;
    int attached;
    JNIEnv* env = attach(b, &attached);
    if (!env) return;
    jbyteArray array = (*env)->NewByteArray(env, (jsize)size);
    if (array) {
        (*env)->SetByteArrayRegion(env, array, 0, (jsize)size, (const jbyte*)bytes);
        (*env)->CallVoidMethod(env, b->sink, b->send, array);
        (*env)->DeleteLocalRef(env, array);
    }
    detach(b, env, attached);
}

static void on_done(void* raw, int result, int intensity, const char* message) {
    Bridge* b = raw;
    int attached;
    JNIEnv* env = attach(b, &attached);
    if (!env) return;
    jstring text = (*env)->NewStringUTF(env, message ? message : "");
    (*env)->CallVoidMethod(env, b->sink, b->done, result, intensity, text);
    if (text) (*env)->DeleteLocalRef(env, text);
    detach(b, env, attached);
}

static void on_log(void* raw, const char* line) {
    Bridge* b = raw;
    int attached;
    JNIEnv* env = attach(b, &attached);
    if (!env) return;
    jstring text = (*env)->NewStringUTF(env, line ? line : "");
    (*env)->CallVoidMethod(env, b->sink, b->log, text);
    if (text) (*env)->DeleteLocalRef(env, text);
    detach(b, env, attached);
}

JNIEXPORT jlong JNICALL Java_com_tavi_airbuzz_AirNative_nativeCreate(
        JNIEnv* env, jclass cls, jobject sink, jint packet_size) {
    (void)cls;
    Bridge* b = calloc(1, sizeof(*b));
    if (!b) return 0;
    jclass sink_class = (*env)->GetObjectClass(env, sink);
    b->send = (*env)->GetMethodID(env, sink_class, "send", "([B)V");
    b->done = (*env)->GetMethodID(env, sink_class, "done", "(IILjava/lang/String;)V");
    b->log = (*env)->GetMethodID(env, sink_class, "log", "(Ljava/lang/String;)V");
    b->sink = (*env)->NewGlobalRef(env, sink);
    if (!b->send || !b->done || !b->log || !b->sink ||
        (*env)->GetJavaVM(env, &b->vm) != JNI_OK) {
        if (b->sink) (*env)->DeleteGlobalRef(env, b->sink);
        free(b);
        return 0;
    }
    b->session = AB_Session_Create(on_send, on_done, on_log, b, (size_t)packet_size);
    if (!b->session) {
        (*env)->DeleteGlobalRef(env, b->sink);
        free(b);
        return 0;
    }
    return (jlong)(intptr_t)b;
}

JNIEXPORT void JNICALL Java_com_tavi_airbuzz_AirNative_nativeReceive(
        JNIEnv* env, jclass cls, jlong handle, jbyteArray data) {
    (void)cls;
    Bridge* b = (Bridge*)(intptr_t)handle;
    jsize size = (*env)->GetArrayLength(env, data);
    jbyte* bytes = (*env)->GetByteArrayElements(env, data, NULL);
    if (!bytes) return;
    AB_Session_Receive(b->session, (const uint8_t*)bytes, (size_t)size);
    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
}

JNIEXPORT void JNICALL Java_com_tavi_airbuzz_AirNative_nativeLinkLost(
        JNIEnv* env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    AB_Session_LinkLost(((Bridge*)(intptr_t)handle)->session);
}

JNIEXPORT jint JNICALL Java_com_tavi_airbuzz_AirNative_nativeStart(
        JNIEnv* env, jclass cls, jlong handle, jint operation) {
    (void)env; (void)cls;
    return AB_Session_Start(((Bridge*)(intptr_t)handle)->session, (AB_Operation)operation);
}

JNIEXPORT void JNICALL Java_com_tavi_airbuzz_AirNative_nativeAbort(
        JNIEnv* env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    AB_Session_Abort(((Bridge*)(intptr_t)handle)->session);
}

JNIEXPORT jboolean JNICALL Java_com_tavi_airbuzz_AirNative_nativeIsMuted(
        JNIEnv* env, jclass cls, jlong handle) {
    (void)env; (void)cls;
    return AB_Session_IsMuted(((Bridge*)(intptr_t)handle)->session) != 0;
}

/* Joins the loop thread, so it must not run on a core callback thread. */
JNIEXPORT void JNICALL Java_com_tavi_airbuzz_AirNative_nativeDestroy(
        JNIEnv* env, jclass cls, jlong handle) {
    (void)cls;
    Bridge* b = (Bridge*)(intptr_t)handle;
    AB_Session_Destroy(b->session);
    (*env)->DeleteGlobalRef(env, b->sink);
    free(b);
}
