/*
 * mai_jni.c (Android): what goes through Java. The microphone permission
 * (Activity.checkSelfPermission) and AudioManager.getDevices, for
 * devices/1 and for checking an opener's device id. Included by
 * mob_audio_io_nif.c after ../common/mai_nif.c (mob_dev compiles only that
 * file); ported from Humdinger's c_src/hum_audio.c.
 *
 * Callers run on dirty schedulers or the event thread, native threads the
 * JVM doesn't know: jni_env attaches them for the call and jni_done
 * detaches them again.
 */
#include <jni.h>

/* Defined by mob's beam_jni.c (the app's JNI glue). */
extern JavaVM *g_jvm;
extern jobject g_activity;

/* AudioManager.GET_DEVICES_INPUTS / GET_DEVICES_OUTPUTS */
#define GET_DEVICES_INPUTS 1
#define GET_DEVICES_OUTPUTS 2

/* The calling thread's JNIEnv, attaching it if needed; `*attached` says to
 * detach after. NULL without a JVM. */
static JNIEnv *jni_env(bool *attached) {
    *attached = false;
    if (!g_jvm) return NULL;
    JNIEnv *j = NULL;
    jint rc = (*g_jvm)->GetEnv(g_jvm, (void **)&j, JNI_VERSION_1_6);
    if (rc == JNI_OK) return j;
    if (rc != JNI_EDETACHED || (*g_jvm)->AttachCurrentThread(g_jvm, &j, NULL) != JNI_OK) return NULL;
    *attached = true;
    return j;
}

static void jni_done(bool attached) {
    if (attached) (*g_jvm)->DetachCurrentThread(g_jvm);
}

/* A pending exception, cleared. */
static bool jni_failed(JNIEnv *j) {
    if (!(*j)->ExceptionCheck(j)) return false;
    (*j)->ExceptionClear(j);
    return true;
}

/* A method id, or NULL (exception cleared) if the class lacks it. */
static jmethodID jni_method(JNIEnv *j, jclass cls, const char *name, const char *sig) {
    jmethodID id = cls ? (*j)->GetMethodID(j, cls, name, sig) : NULL;
    return jni_failed(j) ? NULL : id;
}

/* Whether the activity holds RECORD_AUDIO; true when it can't be asked
 * (then AAudio's open is the judge). In the caller's local frame. */
static bool mic_permitted(JNIEnv *j) {
    jmethodID check =
        jni_method(j, (*j)->GetObjectClass(j, g_activity), "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring name = (*j)->NewStringUTF(j, "android.permission.RECORD_AUDIO");
    if (jni_failed(j) || !check || !name) return true;
    jint r = (*j)->CallIntMethod(j, g_activity, check, name);
    if (jni_failed(j)) return true;
    return r == 0; /* PackageManager.PERMISSION_GRANTED */
}

typedef struct {
    jobjectArray list;
    jmethodID get_id, get_type, get_name, to_string;
} device_list;

/* AudioManager.getDevices(flags) and AudioDeviceInfo's getters; false if
 * Java can't say. Local references in the caller's frame. */
static bool get_devices(JNIEnv *j, jint flags, device_list *d) {
    if (!g_activity) return false;
    jmethodID get_service = jni_method(j, (*j)->GetObjectClass(j, g_activity), "getSystemService",
                                       "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring audio = (*j)->NewStringUTF(j, "audio");
    if (jni_failed(j) || !get_service || !audio) return false;
    jobject manager = (*j)->CallObjectMethod(j, g_activity, get_service, audio);
    if (jni_failed(j) || !manager) return false;
    jclass info = (*j)->FindClass(j, "android/media/AudioDeviceInfo");
    if (jni_failed(j) || !info) return false;
    jclass object = (*j)->FindClass(j, "java/lang/Object");
    if (jni_failed(j) || !object) return false;
    jmethodID get_devices =
        jni_method(j, (*j)->GetObjectClass(j, manager), "getDevices", "(I)[Landroid/media/AudioDeviceInfo;");
    d->get_id = jni_method(j, info, "getId", "()I");
    d->get_type = jni_method(j, info, "getType", "()I");
    d->get_name = jni_method(j, info, "getProductName", "()Ljava/lang/CharSequence;");
    d->to_string = jni_method(j, object, "toString", "()Ljava/lang/String;");
    if (!get_devices || !d->get_id || !d->get_type || !d->get_name || !d->to_string) return false;
    d->list = (jobjectArray)(*j)->CallObjectMethod(j, manager, get_devices, flags);
    return !jni_failed(j) && d->list;
}

/* 1: `id` is one of AudioManager's devices of `flags`; 0: it isn't; -1:
 * Java can't say. */
static int device_listed(JNIEnv *j, jint flags, int32_t id) {
    device_list d;
    if ((*j)->PushLocalFrame(j, 16) != JNI_OK) {
        jni_failed(j);
        return -1;
    }
    int found = -1;
    if (get_devices(j, flags, &d)) {
        found = 0;
        jsize n = (*j)->GetArrayLength(j, d.list);
        for (jsize i = 0; i < n && !found; i++) {
            jobject dev = (*j)->GetObjectArrayElement(j, d.list, i);
            if (jni_failed(j) || !dev) continue;
            jint dev_id = (*j)->CallIntMethod(j, dev, d.get_id);
            if (!jni_failed(j) && dev_id == id) found = 1;
            (*j)->DeleteLocalRef(j, dev);
        }
    }
    (*j)->PopLocalFrame(j, NULL);
    return found;
}

/* AudioDeviceInfo.TYPE_* to the plugin's kind; NULL for the system's
 * internal routes (telephony, remote submix, echo reference, tuners, the
 * "safe" speaker for alarms). */
static const char *device_kind(jint type) {
    switch (type) {
    case 1: return "builtin_earpiece";        /* BUILTIN_EARPIECE */
    case 2: return "builtin_speaker";         /* BUILTIN_SPEAKER */
    case 3: return "wired_headset";           /* WIRED_HEADSET */
    case 4: return "wired_headphones";        /* WIRED_HEADPHONES */
    case 7: return "bluetooth";               /* BLUETOOTH_SCO */
    case 8: return "bluetooth_a2dp";          /* BLUETOOTH_A2DP */
    case 9: case 10: case 29: return "hdmi";  /* HDMI, HDMI_ARC, HDMI_EARC */
    case 11: case 12: case 22: return "usb";  /* USB_DEVICE, USB_ACCESSORY, USB_HEADSET */
    case 15: return "builtin_mic";            /* BUILTIN_MIC */
    case 21: return "car";                    /* BUS (Android Automotive) */
    case 26: case 27: case 30: return "bluetooth"; /* BLE_HEADSET, BLE_SPEAKER, BLE_BROADCAST */
    case 14: case 16: case 17: case 18: case 24: case 25: case 28:
        /* FM, FM_TUNER, TV_TUNER, TELEPHONY, BUILTIN_SPEAKER_SAFE, REMOTE_SUBMIX, ECHO_REFERENCE */
        return NULL;
    default: return "other";
    }
}

/* A Java string (or NULL) as a UTF-8 binary. Not GetStringUTFChars: that
 * is modified UTF-8, which turns an emoji in a headset's name into bytes
 * that aren't valid UTF-8. */
static ERL_NIF_TERM jstring_term(ErlNifEnv *env, JNIEnv *j, jstring s) {
    ERL_NIF_TERM term;
    jsize n = s ? (*j)->GetStringLength(j, s) : 0;
    const jchar *u = n > 0 ? (*j)->GetStringChars(j, s, NULL) : NULL;
    ErlNifBinary bin;
    if (!u || !enif_alloc_binary((size_t)n * 3, &bin)) { /* at most 3 bytes per UTF-16 unit */
        if (u) (*j)->ReleaseStringChars(j, s, u);
        enif_make_new_binary(env, 0, &term);
        return term;
    }
    size_t w = 0;
    for (jsize i = 0; i < n; i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n && u[i + 1] >= 0xDC00 && u[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00u);
        else if (c >= 0xD800 && c < 0xE000)
            c = 0xFFFD; /* a lone surrogate */
        if (c < 0x80) {
            bin.data[w++] = (unsigned char)c;
        } else if (c < 0x800) {
            bin.data[w++] = (unsigned char)(0xC0 | (c >> 6));
            bin.data[w++] = (unsigned char)(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            bin.data[w++] = (unsigned char)(0xE0 | (c >> 12));
            bin.data[w++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
            bin.data[w++] = (unsigned char)(0x80 | (c & 0x3F));
        } else {
            bin.data[w++] = (unsigned char)(0xF0 | (c >> 18));
            bin.data[w++] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
            bin.data[w++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
            bin.data[w++] = (unsigned char)(0x80 | (c & 0x3F));
        }
    }
    (*j)->ReleaseStringChars(j, s, u);
    enif_realloc_binary(&bin, w);
    return enif_make_binary(env, &bin);
}

/* (no lock; a dirty IO scheduler) The devices AudioManager lists for a
 * direction, in its order: [%{id, name, kind}], or {:error, :unavailable}
 * when Java can't be reached. */
static ERL_NIF_TERM be_devices(ErlNifEnv *env, int dir) {
    bool attached;
    JNIEnv *j = jni_env(&attached);
    if (!j) return error_atom(env, "unavailable");
    if ((*j)->PushLocalFrame(j, 16) != JNI_OK) {
        jni_failed(j);
        jni_done(attached);
        return error_atom(env, "unavailable");
    }
    device_list d;
    ERL_NIF_TERM result;
    if (!get_devices(j, dir == MAI_DIR_IN ? GET_DEVICES_INPUTS : GET_DEVICES_OUTPUTS, &d)) {
        result = error_atom(env, "unavailable");
    } else {
        ERL_NIF_TERM keys[3] = {atom(env, "id"), atom(env, "name"), atom(env, "kind")};
        result = enif_make_list(env, 0);
        for (jsize i = (*j)->GetArrayLength(j, d.list) - 1; i >= 0; i--) {
            jobject dev = (*j)->GetObjectArrayElement(j, d.list, i);
            if (jni_failed(j) || !dev) continue;
            jint type = (*j)->CallIntMethod(j, dev, d.get_type);
            const char *kind = jni_failed(j) ? NULL : device_kind(type);
            jint id = kind ? (*j)->CallIntMethod(j, dev, d.get_id) : 0;
            if (kind && !jni_failed(j)) {
                jobject chars = (*j)->CallObjectMethod(j, dev, d.get_name);
                if (jni_failed(j)) chars = NULL;
                jstring name = chars ? (jstring)(*j)->CallObjectMethod(j, chars, d.to_string) : NULL;
                if (jni_failed(j)) name = NULL;
                ERL_NIF_TERM vals[3] = {enif_make_int(env, id), jstring_term(env, j, name), atom(env, kind)};
                result = enif_make_list_cell(env, map_of(env, keys, vals, 3), result);
                if (name) (*j)->DeleteLocalRef(j, name);
                if (chars) (*j)->DeleteLocalRef(j, chars);
            }
            (*j)->DeleteLocalRef(j, dev);
        }
    }
    (*j)->PopLocalFrame(j, NULL);
    jni_done(attached);
    return result;
}
