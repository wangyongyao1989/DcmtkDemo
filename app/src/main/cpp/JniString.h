#ifndef DCMTKDEMO_JNISTRING_H
#define DCMTKDEMO_JNISTRING_H

#include <jni.h>

// RAII helper for JNI strings: acquires UTF chars on construction and
// releases them on destruction. Null-safe (a null jstring yields a null c_str).
class JniString {
public:
    JniString(JNIEnv *env, jstring str) : env_(env), jstr_(str), c_str_(nullptr) {
        if (jstr_) c_str_ = env_->GetStringUTFChars(jstr_, nullptr);
    }

    ~JniString() {
        if (c_str_) env_->ReleaseStringUTFChars(jstr_, c_str_);
    }

    const char *c_str() const { return c_str_; }

    operator const char *() const { return c_str_; }

private:
    JNIEnv *env_;
    jstring jstr_;
    const char *c_str_;
};

#endif // DCMTKDEMO_JNISTRING_H
