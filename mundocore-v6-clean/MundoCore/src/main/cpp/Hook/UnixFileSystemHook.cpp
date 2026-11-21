//
// Fixed UnixFileSystemHook.cpp
// Solves .pak write Permission denied (errno=13)
// Android 10–15 BlackBox compatible
//

#include <IO.h>
#include "UnixFileSystemHook.h"
#include "JniHook/JniHook.h"
#include "BaseHook.h"
#include "DebugLogger.h"

// Log Java-layer file redirects only when path is interesting (OBB/data/so/apk/pkg).
static inline bool ufs_should_trace(const char *p) {
    if (!p || !p[0]) return false;
    return strstr(p, "/data/") || strstr(p, "/storage/") || strstr(p, "/sdcard") ||
           strstr(p, ".so")   || strstr(p, ".apk")      || strstr(p, ".obb")   ||
           strstr(p, ".dex")  || strstr(p, ".vdex")     || strstr(p, ".odex");
}

// Trace a jstring path; logs only when interesting or redirected.
static void trace_jstr_redirect(JNIEnv *env, const char *method,
                                 jstring orig, jstring redir) {
    if (orig == redir) {
        // No redirect — log only if path is interesting.
        const char *cs = env->GetStringUTFChars(orig, nullptr);
        if (cs) {
            if (ufs_should_trace(cs))
                TRACE("[jfs] %s  %s", method, cs);
            env->ReleaseStringUTFChars(orig, cs);
        }
        return;
    }
    const char *co = env->GetStringUTFChars(orig,  nullptr);
    const char *cr = env->GetStringUTFChars(redir, nullptr);
    TRACE("[jfs-redir] %s  %s  =>  %s", method,
          co ? co : "?", cr ? cr : "?");
    if (co) env->ReleaseStringUTFChars(orig,  co);
    if (cr) env->ReleaseStringUTFChars(redir, cr);
}

// Trace a File-object path redirect via getPath().
static void trace_file_redirect(JNIEnv *env, const char *method,
                                 jobject orig, jobject redir) {
    if (orig == redir) return;
    jclass clazz = env->FindClass("java/io/File");
    if (!clazz) return;
    jmethodID mid = env->GetMethodID(clazz, "getPath", "()Ljava/lang/String;");
    env->DeleteLocalRef(clazz);
    if (!mid) return;
    jstring so = (jstring)env->CallObjectMethod(orig,  mid);
    jstring sr = (jstring)env->CallObjectMethod(redir, mid);
    const char *co = so ? env->GetStringUTFChars(so, nullptr) : nullptr;
    const char *cr = sr ? env->GetStringUTFChars(sr, nullptr) : nullptr;
    TRACE("[jfs-redir] %s  %s  =>  %s", method,
          co ? co : "?", cr ? cr : "?");
    if (co && so) env->ReleaseStringUTFChars(so, co);
    if (cr && sr) env->ReleaseStringUTFChars(sr, cr);
    if (so) env->DeleteLocalRef(so);
    if (sr) env->DeleteLocalRef(sr);
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    canonicalize0
 * Signature: (Ljava/lang/String;)Ljava/lang/String;
 */
HOOK_JNI(jstring, canonicalize0, JNIEnv *env, jobject obj, jstring path) {
    jstring redirect = IO::redirectPath(env, path);
    trace_jstr_redirect(env, "canonicalize0", path, redirect);
    jstring result = orig_canonicalize0(env, obj, redirect);
    if (redirect != path) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    getBooleanAttributes0
 * Signature: (Ljava/lang/String;)I
 */
HOOK_JNI(jint, getBooleanAttributes0, JNIEnv *env, jobject obj, jstring abspath) {
    jstring redirect = IO::redirectPath(env, abspath);
    trace_jstr_redirect(env, "getBooleanAttributes0", abspath, redirect);
    jint result = orig_getBooleanAttributes0(env, obj, redirect);
    if (redirect != abspath) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    getLastModifiedTime0
 * Signature: (Ljava/io/File;)J
 */
HOOK_JNI(jlong, getLastModifiedTime0, JNIEnv *env, jobject obj, jobject path) {
    jobject redirect = IO::redirectPath(env, path);
    trace_file_redirect(env, "getLastModifiedTime0", path, redirect);
    jlong result = orig_getLastModifiedTime0(env, obj, redirect);
    if (redirect != path) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    setPermission0
 * Signature: (Ljava/io/File;IZZ)Z
 */
HOOK_JNI(jboolean, setPermission0, JNIEnv *env, jobject obj, jobject file, jint access,
         jboolean enable, jboolean owneronly) {
    jobject redirect = IO::redirectPath(env, file);
    trace_file_redirect(env, "setPermission0", file, redirect);
    jboolean result = orig_setPermission0(env, obj, redirect, access, enable, owneronly);
    if (redirect != file) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    createFileExclusively0
 * Signature: (Ljava/lang/String;)Z
 */
HOOK_JNI(jboolean, createFileExclusively0, JNIEnv *env, jobject obj, jstring path) {
    jstring redirect = IO::redirectPath(env, path);
    trace_jstr_redirect(env, "createFileExclusively0", path, redirect);
    jboolean result = orig_createFileExclusively0(env, obj, redirect);
    if (redirect != path) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    list0
 * Signature: (Ljava/io/File;)[Ljava/lang/String;
 */
HOOK_JNI(jobjectArray, list0, JNIEnv *env, jobject obj, jobject file) {
    jobject redirect = IO::redirectPath(env, file);
    trace_file_redirect(env, "list0", file, redirect);
    jobjectArray result = orig_list0(env, obj, redirect);
    if (redirect != file) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    createDirectory0
 * Signature: (Ljava/io/File;)Z
 */
HOOK_JNI(jboolean, createDirectory0, JNIEnv *env, jobject obj, jobject path) {
    jobject redirect = IO::redirectPath(env, path);
    trace_file_redirect(env, "createDirectory0", path, redirect);
    jboolean result = orig_createDirectory0(env, obj, redirect);
    if (redirect != path) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    setLastModifiedTime0
 * Signature: (Ljava/io/File;J)Z
 */
HOOK_JNI(jboolean, setLastModifiedTime0, JNIEnv *env, jobject obj, jobject file, jobject time) {
    jobject redirect = IO::redirectPath(env, file);
    trace_file_redirect(env, "setLastModifiedTime0", file, redirect);
    jboolean result = orig_setLastModifiedTime0(env, obj, redirect, time);
    if (redirect != file) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    setReadOnly0
 * Signature: (Ljava/io/File;)Z
 */
HOOK_JNI(jboolean, setReadOnly0, JNIEnv *env, jobject obj, jobject file) {
    jobject redirect = IO::redirectPath(env, file);
    trace_file_redirect(env, "setReadOnly0", file, redirect);
    jboolean result = orig_setReadOnly0(env, obj, redirect);
    if (redirect != file) env->DeleteLocalRef(redirect);
    return result;
}

/*
 * Class:     java_io_UnixFileSystem
 * Method:    getSpace0
 * Signature: (Ljava/io/File;I)J
 */
HOOK_JNI(jboolean, getSpace0, JNIEnv *env, jobject obj, jobject file, jint t) {
    jobject redirect = IO::redirectPath(env, file);
    jboolean result = orig_getSpace0(env, obj, redirect, t);
    if (redirect != file) env->DeleteLocalRef(redirect);
    return result;
}

void UnixFileSystemHook::init(JNIEnv *env) {
    const char *className = "java/io/UnixFileSystem";
  //  JniHook::HookJniFun(env, className, "canonicalize0", "(Ljava/lang/String;)Ljava/lang/String;",(void *) new_canonicalize0, (void **) (&orig_canonicalize0), false);
//    JniHook::HookJniFun(env, className, "getBooleanAttributes0", "(Ljava/lang/String;)I",(void *) new_getBooleanAttributes0,(void **) (&orig_getBooleanAttributes0), false);
    JniHook::HookJniFun(env, className, "getLastModifiedTime0", "(Ljava/io/File;)J",(void *) new_getLastModifiedTime0, (void **) (&orig_getLastModifiedTime0),false);
    JniHook::HookJniFun(env, className, "setPermission0", "(Ljava/io/File;IZZ)Z",(void *) new_setPermission0, (void **) (&orig_setPermission0), false);
    JniHook::HookJniFun(env, className, "createFileExclusively0", "(Ljava/lang/String;)Z",(void *) new_createFileExclusively0,(void **) (&orig_createFileExclusively0), false);
    JniHook::HookJniFun(env, className, "list0", "(Ljava/io/File;)[Ljava/lang/String;",(void *) new_list0, (void **) (&orig_list0), false);
    JniHook::HookJniFun(env, className, "createDirectory0", "(Ljava/io/File;)Z",(void *) new_createDirectory0, (void **) (&orig_createDirectory0), false);
    JniHook::HookJniFun(env, className, "setLastModifiedTime0", "(Ljava/io/File;J)Z",(void *) new_setLastModifiedTime0, (void **) (&orig_setLastModifiedTime0),false);
    JniHook::HookJniFun(env, className, "setReadOnly0", "(Ljava/io/File;)Z",(void *) new_setReadOnly0, (void **) (&orig_setReadOnly0), false);
    JniHook::HookJniFun(env, className, "getSpace0", "(Ljava/io/File;I)J",(void *) new_getSpace0, (void **) (&orig_getSpace0), false);
}