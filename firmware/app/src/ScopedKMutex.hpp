#pragma once

#include <zephyr/kernel.h>

namespace qcbridge {

// RAII guard around k_mutex_lock()/k_mutex_unlock(), per CODING_STANDARDS.md's
// guidance to wrap Zephyr lock/unlock pairs in a scope guard rather than
// pairing them by hand. Always waits K_FOREVER - every caller in this
// codebase needs the lock, not a timed/optional attempt.
class ScopedKMutex {
public:
    explicit ScopedKMutex(k_mutex& mutex) : mutex_(mutex) { k_mutex_lock(&mutex_, K_FOREVER); }
    ~ScopedKMutex() { k_mutex_unlock(&mutex_); }

    ScopedKMutex(const ScopedKMutex&) = delete;
    ScopedKMutex& operator=(const ScopedKMutex&) = delete;

private:
    k_mutex& mutex_;
};

}  // namespace qcbridge
