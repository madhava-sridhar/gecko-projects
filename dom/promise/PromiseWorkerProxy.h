/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_PromiseWorkerProxy_h
#define mozilla_dom_PromiseWorkerProxy_h

#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/workers/bindings/WorkerFeature.h"
#include "nsProxyRelease.h"

namespace mozilla {
namespace dom {

class Promise;

namespace workers {
class WorkerPrivate;
}

// A PromiseHolder holds a worker and a worker-thread Promise alive while stuff
// is done on the main-thread. Main-thread code should then dispatch a Foo
// runnable to the worker, which should resolve/reject the Promise and call
// CleanUp() to release it.
class WorkerPromiseHolder : public workers::WorkerFeature
{
  workers::WorkerPrivate* mWorkerPrivate;
  nsRefPtr<Promise> mWorkerPromise;
  bool mCleanedUp; // To specify if the cleanUp() has been done.
  Mutex mCleanUpLock;

  // This overrides the non-threadsafe refcounting in PromiseNativeHandler.
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WorkerPromiseHolder)

private:
  virtual ~WorkerPromiseHolder();

protected:
  virtual bool Notify(JSContext* aCx, workers::Status aStatus) MOZ_OVERRIDE;

public:
  WorkerPromiseHolder(workers::WorkerPrivate* aWorkerPrivate,
                      Promise* aWorkerPromise);

  workers::WorkerPrivate* GetWorkerPrivate() const;

  Promise* GetWorkerPromise() const;

  void CleanUp(JSContext* aCx);
};

// A proxy to resolve a worker thread Promise from the main thread.
// How to use:
//
//   1. Create a Promise on the worker thread and return it to the content
//      script:
//
//        nsRefPtr<Promise> promise = Promise::Create(workerPrivate->GlobalScope(), aRv);
//        if (aRv.Failed()) {
//          return nullptr;
//        }
//        // Pass |promise| around to the nsIRunnable
//        return promise.forget();
//
//   2. In your main-thread nsIRunnable's ctor, create a PromiseWorkerFoo
//      which holds a nsRefPtr<Promise> to the Promise created at #1.
//
//   3. Call PromiseWorkerFoo::Resolve/Reject at some point on the main-thread.
//
//   4. Then the JS::Value passed to Resolve/Reject will be dispatched via
//      a WorkerRunnable to the worker thread to resolve/reject the Promise
//      created at #1.

class PromiseWorkerFoo : public workers::WorkerFeature
{
  friend class PromiseWorkerFooRunnable;

  // This overrides the non-threadsafe refcounting in PromiseNativeHandler.
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(PromiseWorkerFoo)

public:
  PromiseWorkerFoo(workers::WorkerPrivate* aWorkerPrivate,
                   Promise* aWorkerPromise,
                   JSStructuredCloneCallbacks* aCallbacks = nullptr);

  workers::WorkerPrivate* GetWorkerPrivate() const;

  Promise* GetWorkerPromise() const;

  void StoreISupports(nsISupports* aSupports);

  void CleanUp(JSContext* aCx);

  void Resolve(JSContext* aCx,
               JS::Handle<JS::Value> aValue);

  void Reject(JSContext* aCx,
              JS::Handle<JS::Value> aValue);

protected:
  virtual bool Notify(JSContext* aCx, workers::Status aStatus) MOZ_OVERRIDE;

private:
  virtual ~PromiseWorkerFoo();

  // Function pointer for calling Promise::{ResolveInternal,RejectInternal}.
  typedef void (Promise::*RunCallbackFunc)(JSContext*,
                                           JS::Handle<JS::Value>,
                                           Promise::PromiseTaskSync);

  void RunCallback(JSContext* aCx,
                   JS::Handle<JS::Value> aValue,
                   RunCallbackFunc aFunc);

  workers::WorkerPrivate* mWorkerPrivate;

  // This lives on the worker thread.
  nsRefPtr<Promise> mWorkerPromise;

  bool mCleanedUp; // To specify if the cleanUp() has been done.

  JSStructuredCloneCallbacks* mCallbacks;

  // Aimed to keep objects alive when doing the structured-clone read/write,
  // which can be added by calling StoreISupports() on the main thread.
  nsTArray<nsMainThreadPtrHandle<nsISupports>> mSupportsArray;

  // Ensure the worker and the main thread won't race to access |mCleanedUp|.
  Mutex mCleanUpLock;
};

// A proxy to catch the resolved/rejected Promise's result from the main thread
// and resolve/reject that on the worker thread eventually.
//
// How to use:
//
//   1. Create a Promise on the worker thread and return it to the content
//      script:
//
//        nsRefPtr<Promise> promise = Promise::Create(workerPrivate->GlobalScope(), aRv);
//        if (aRv.Failed()) {
//          return nullptr;
//        }
//        // Pass |promise| around to the WorkerMainThreadRunnable
//        return promise.forget();
//
//   2. In your WorkerMainThreadRunnable's ctor, create a PromiseWorkerProxy
//      which holds a nsRefPtr<Promise> to the Promise created at #1.
//
//   3. In your WorkerMainThreadRunnable::MainThreadRun(), obtain a Promise on
//      the main thread and call its AppendNativeHandler(PromiseNativeHandler*)
//      to bind the PromiseWorkerProxy created at #2.
//
//   4. Then the Promise results returned by ResolvedCallback/RejectedCallback
//      will be dispatched as a WorkerRunnable to the worker thread to
//      resolve/reject the Promise created at #1.
class PromiseWorkerProxy : public PromiseNativeHandler
{
  // This overrides the non-threadsafe refcounting in PromiseNativeHandler.
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(PromiseWorkerProxy)

public:
  PromiseWorkerProxy(workers::WorkerPrivate* aWorkerPrivate,
                     Promise* aWorkerPromise,
                     JSStructuredCloneCallbacks* aCallbacks = nullptr);

  void StoreISupports(nsISupports* aSupports);

protected:
  virtual void ResolvedCallback(JSContext* aCx,
                                JS::Handle<JS::Value> aValue) MOZ_OVERRIDE;

  virtual void RejectedCallback(JSContext* aCx,
                                JS::Handle<JS::Value> aValue) MOZ_OVERRIDE;

private:
  virtual ~PromiseWorkerProxy();

  nsRefPtr<PromiseWorkerFoo> mFoo;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_PromiseWorkerProxy_h
