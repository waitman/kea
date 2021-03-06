// Copyright (C) 2012  Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include "thread.h"
#include "sync.h"

#include <memory>
#include <string>
#include <cstring>
#include <cerrno>

#include <pthread.h>

#include <boost/scoped_ptr.hpp>

using std::string;
using std::exception;
using std::auto_ptr;
using boost::scoped_ptr;

namespace isc {
namespace util {
namespace thread {

// The implementation of the Thread class.
//
// This internal state is not deleted until the thread terminates and is either
// waited for or detached. We could do this with shared_ptr (or, shared_ptr and
// weak_ptr), but we plan on compiling boost without thread support, so it
// might not be safe. Therefore we use an explicit mutex. It is being locked
// only 2-3 times in the lifetime of the thread, which should be negligible
// overhead anyway.
class Thread::Impl {
public:
    Impl(const boost::function<void ()>& main) :
        // Two things to happen before destruction - thread needs to terminate
        // and the creating thread needs to release it.
        waiting_(2),
        main_(main),
        exception_(false)
    {}
    // Another of the waiting events is done. If there are no more, delete
    // impl.
    static void done(Impl* impl) {
        bool should_delete(false);
        { // We need to make sure the mutex is unlocked before it is deleted
            Mutex::Locker locker(impl->mutex_);
            if (--impl->waiting_ == 0) {
                should_delete = true;
            }
        }
        if (should_delete) {
            delete impl;
        }
    }
    // Run the thread. The type of parameter is because the pthread API.
    static void* run(void* impl_raw) {
        Impl* impl = static_cast<Impl*>(impl_raw);
        try {
            impl->main_();
        } catch (const exception& e) {
            impl->exception_ = true;
            impl->exception_text_ = e.what();
        } catch (...) {
            impl->exception_ = true;
            impl->exception_text_ = "Uknown exception";
        }
        done(impl);
        return (NULL);
    }
    // How many events are waiting? One is for the thread to finish, one
    // for the destructor of Thread or wait. Once both happen, this is
    // no longer needed.
    size_t waiting_;
    // The main function of the thread.
    boost::function<void ()> main_;
    // Was there an exception?
    bool exception_;
    string exception_text_;
    // The mutex protects the waiting_ member, which ensures there are
    // no race conditions and collisions when terminating. The other members
    // should be safe, because:
    // * tid_ is read only.
    // * exception_ and exception_text_ is accessed outside of the thread
    //   only after join, by that time the thread must have terminated.
    // * main_ is used in a read-only way here. If there are any shared
    //   resources used inside, it is up to the main_ itself to take care.
    Mutex mutex_;
    // Which thread are we talking about anyway?
    pthread_t tid_;
};

Thread::Thread(const boost::function<void ()>& main) :
    impl_(NULL)
{
    auto_ptr<Impl> impl(new Impl(main));
    const int result = pthread_create(&impl->tid_, NULL, &Impl::run,
                                      impl.get());
    // Any error here?
    switch (result) {
        case 0: // All 0K
            impl_ = impl.release();
            break;
        case EAGAIN:
            throw std::bad_alloc();
        default: // Other errors. They should not happen.
            isc_throw(isc::InvalidOperation, std::strerror(result));
    }
}

Thread::~Thread() {
    if (impl_ != NULL) {
        // In case we didn't call wait yet
        const int result = pthread_detach(impl_->tid_);
        Impl::done(impl_);
        impl_ = NULL;
        // If the detach ever fails, something is screwed rather badly.
        assert(result == 0);
    }
}

void
Thread::wait() {
    if (impl_ == NULL) {
        isc_throw(isc::InvalidOperation,
                  "Wait called and no thread to wait for");
    }

    const int result = pthread_join(impl_->tid_, NULL);
    if (result != 0) {
        isc_throw(isc::InvalidOperation, std::strerror(result));
    }

    // Was there an exception in the thread?
    scoped_ptr<UncaughtException> ex;
    // Something here could in theory throw. But we already terminated the thread, so
    // we need to make sure we are in consistent state even in such situation (like
    // releasing the mutex and impl_).
    try {
        if (impl_->exception_) {
            ex.reset(new UncaughtException(__FILE__, __LINE__,
                                           impl_->exception_text_.c_str()));
        }
    } catch (...) {
        Impl::done(impl_);
        impl_ = NULL;
        // We have eaten the UncaughtException by now, but there's another
        // exception instead, so we have at least something.
        throw;
    }

    Impl::done(impl_);
    impl_ = NULL;
    if (ex.get() != NULL) {
        throw UncaughtException(*ex);
    }
}

}
}
}
