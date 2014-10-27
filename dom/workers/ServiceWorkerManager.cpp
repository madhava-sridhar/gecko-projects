/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerManager.h"

#include "nsIDOMEventTarget.h"
#include "nsIDocument.h"
#include "nsIHttpChannelInternal.h"
#include "nsINetworkInterceptController.h"
#include "nsIScriptSecurityManager.h"
#include "nsIStreamLoader.h"
#include "nsPIDOMWindow.h"
#include "nsDebug.h"

#include "jsapi.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/DOMError.h"
#include "mozilla/dom/ErrorEvent.h"
#include "mozilla/dom/Headers.h"
#include "mozilla/dom/InstallEventBinding.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/Request.h"

#include "nsContentUtils.h"
#include "nsNetUtil.h"
#include "nsProxyRelease.h"
#include "nsTArray.h"

#include "RuntimeService.h"
#include "ServiceWorker.h"
#include "ServiceWorkerClient.h"
#include "ServiceWorkerRegistration.h"
#include "ServiceWorkerEvents.h"
#include "WorkerInlines.h"
#include "WorkerPrivate.h"
#include "WorkerRunnable.h"
#include "WorkerScope.h"

using namespace mozilla;
using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

NS_IMPL_ISUPPORTS0(ServiceWorkerJob)
NS_IMPL_ISUPPORTS0(ServiceWorkerRegistrationInfo)

void
ServiceWorkerJob::Done(nsresult aStatus)
{
  if (NS_WARN_IF(NS_FAILED(aStatus))) {
    // Windows builds complain if the return value of NS_WARN_IF isn't used.
  }

  if (mQueue) {
    mQueue->Done(this);
  }
}

UpdatePromise::UpdatePromise()
  : mState(Pending)
{
  MOZ_COUNT_CTOR(UpdatePromise);
}

UpdatePromise::~UpdatePromise()
{
  MOZ_COUNT_DTOR(UpdatePromise);
}

void
UpdatePromise::AddPromise(Promise* aPromise)
{
  MOZ_ASSERT(mState == Pending);
  mPromises.AppendElement(aPromise);
}

void
UpdatePromise::ResolveAllPromises(const nsACString& aScriptSpec, const nsACString& aScope)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(mState == Pending);
  mState = Resolved;
  RuntimeService* rs = RuntimeService::GetOrCreateService();
  MOZ_ASSERT(rs);

  nsTArray<WeakPtr<Promise>> array;
  array.SwapElements(mPromises);
  for (uint32_t i = 0; i < array.Length(); ++i) {
    WeakPtr<Promise>& pendingPromise = array.ElementAt(i);
    if (pendingPromise) {
      nsRefPtr<Promise> kungfuDeathGrip = pendingPromise.get();
      nsCOMPtr<nsIGlobalObject> go =
        do_QueryInterface(pendingPromise->GetParentObject());
      MOZ_ASSERT(go);

      AutoSafeJSContext cx;
      JS::Rooted<JSObject*> global(cx, go->GetGlobalJSObject());
      JSAutoCompartment ac(cx, global);

      GlobalObject domGlobal(cx, global);

      // The service worker is created and kept alive as a SharedWorker.
      nsRefPtr<ServiceWorker> serviceWorker;
      nsresult rv = rs->CreateServiceWorker(domGlobal,
                                            NS_ConvertUTF8toUTF16(aScriptSpec),
                                            aScope,
                                            getter_AddRefs(serviceWorker));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        pendingPromise->MaybeReject(NS_ERROR_DOM_ABORT_ERR);
        continue;
      }

      // Since ServiceWorkerRegistration is only exposed to windows we can be
      // certain about this cast.
      nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(go);
      nsRefPtr<ServiceWorkerRegistration> swr =
        new ServiceWorkerRegistration(window, NS_ConvertUTF8toUTF16(aScope));

      pendingPromise->MaybeResolve(swr);
    }
  }
}

void
UpdatePromise::RejectAllPromises(nsresult aRv)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(mState == Pending);
  mState = Rejected;

  nsTArray<WeakPtr<Promise>> array;
  array.SwapElements(mPromises);
  for (uint32_t i = 0; i < array.Length(); ++i) {
    WeakPtr<Promise>& pendingPromise = array.ElementAt(i);
    if (pendingPromise) {
      // Since ServiceWorkerRegistration is only exposed to windows we can be
      // certain about this cast.
      nsCOMPtr<nsPIDOMWindow> window =
        do_QueryInterface(pendingPromise->GetParentObject());
      MOZ_ASSERT(window);
      nsRefPtr<DOMError> domError = new DOMError(window, aRv);
      pendingPromise->MaybeRejectBrokenly(domError);
    }
  }
}

void
UpdatePromise::RejectAllPromises(const ErrorEventInit& aErrorDesc)
{
  MOZ_ASSERT(mState == Pending);
  mState = Rejected;

  nsTArray<WeakPtr<Promise>> array;
  array.SwapElements(mPromises);
  for (uint32_t i = 0; i < array.Length(); ++i) {
    WeakPtr<Promise>& pendingPromise = array.ElementAt(i);
    if (pendingPromise) {
      // Since ServiceWorkerRegistration is only exposed to windows we can be
      // certain about this cast.
      nsCOMPtr<nsIGlobalObject> go = do_QueryInterface(pendingPromise->GetParentObject());
      MOZ_ASSERT(go);

      AutoJSAPI jsapi;
      jsapi.Init(go);

      JSContext* cx = jsapi.cx();

      JS::Rooted<JSString*> stack(cx, JS_GetEmptyString(JS_GetRuntime(cx)));

      JS::Rooted<JS::Value> fnval(cx);
      ToJSValue(cx, aErrorDesc.mFilename, &fnval);
      JS::Rooted<JSString*> fn(cx, fnval.toString());

      JS::Rooted<JS::Value> msgval(cx);
      ToJSValue(cx, aErrorDesc.mMessage, &msgval);
      JS::Rooted<JSString*> msg(cx, msgval.toString());

      JS::Rooted<JS::Value> error(cx);
      if (!JS::CreateError(cx, JSEXN_ERR, stack, fn, aErrorDesc.mLineno,
                           aErrorDesc.mColno, nullptr, msg, &error)) {
        pendingPromise->MaybeReject(NS_ERROR_FAILURE);
        continue;
      }

      pendingPromise->MaybeReject(cx, error);
    }
  }
}

void
ServiceWorkerRegistrationInfo::Clear()
{
  if (mInstallingWorker) {
    // FIXME(nsm): Terminate installing worker.
    // Bug 1043701 Set state to redundant.
    // Fire statechange.
    mInstallingWorker = nullptr;
    // FIXME(nsm): Abort any inflight requests from installing worker.
  }

  if (mWaitingWorker) {
    // FIXME(nsm): Bug 1043701 Set state to redundant.
    // Fire statechange.
    mWaitingWorker = nullptr;
    mWaitingToActivate = false;
  }

  if (mActiveWorker) {
    // FIXME(nsm): Bug 1043701 Set state to redundant.
    mActiveWorker = nullptr;
  }

  nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
  MOZ_ASSERT(swm);
  swm->InvalidateServiceWorkerRegistrationWorker(this,
                                                 WhichServiceWorker::INSTALLING_WORKER |
                                                 WhichServiceWorker::WAITING_WORKER |
                                                 WhichServiceWorker::ACTIVE_WORKER);
}

ServiceWorkerRegistrationInfo::ServiceWorkerRegistrationInfo(const nsACString& aScope)
  : mControlledDocumentsCounter(0),
    mScope(aScope),
    mPendingUninstall(false)
{ }

ServiceWorkerRegistrationInfo::~ServiceWorkerRegistrationInfo()
{
  if (IsControllingDocuments()) {
    NS_WARNING("ServiceWorkerRegistrationInfo is still controlling documents. This can be a bug or a leak in ServiceWorker API or in any other API that takes the document alive.");
  }
}

class QueueFireUpdateFoundRunnable MOZ_FINAL : public nsRunnable
{
  nsRefPtr<ServiceWorkerRegistrationInfo> mRegistration;
public:
  QueueFireUpdateFoundRunnable(ServiceWorkerRegistrationInfo* aReg)
    : mRegistration(aReg)
  { }

  NS_IMETHOD
  Run()
  {
    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
    swm->FireEventOnServiceWorkerRegistrations(mRegistration,
                                               NS_LITERAL_STRING("updatefound"));
    return NS_OK;
  }
};

//////////////////////////
// ServiceWorkerManager //
//////////////////////////

NS_IMPL_ADDREF(ServiceWorkerManager)
NS_IMPL_RELEASE(ServiceWorkerManager)

NS_INTERFACE_MAP_BEGIN(ServiceWorkerManager)
  NS_INTERFACE_MAP_ENTRY(nsIServiceWorkerManager)
  if (aIID.Equals(NS_GET_IID(ServiceWorkerManager)))
    foundInterface = static_cast<nsIServiceWorkerManager*>(this);
  else
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIServiceWorkerManager)
NS_INTERFACE_MAP_END

ServiceWorkerManager::ServiceWorkerManager()
{
}

ServiceWorkerManager::~ServiceWorkerManager()
{
  // The map will assert if it is not empty when destroyed.
  mDomainMap.EnumerateRead(CleanupServiceWorkerInformation, nullptr);
  mDomainMap.Clear();
}

/* static */ PLDHashOperator
ServiceWorkerManager::CleanupServiceWorkerInformation(const nsACString& aDomain,
                                                      ServiceWorkerDomainInfo* aDomainInfo,
                                                      void *aUnused)
{
  aDomainInfo->mServiceWorkerRegistrationInfos.Clear();
  return PL_DHASH_NEXT;
}

class ServiceWorkerRegisterJob;

class FinishInstallRunnable MOZ_FINAL : public nsRunnable
{
  nsMainThreadPtrHandle<nsISupports> mJob;
  bool mSuccess;
  bool mActivateImmediately;

public:
  explicit FinishInstallRunnable(
    const nsMainThreadPtrHandle<nsISupports>& aJob,
    bool aSuccess,
    bool aActivateImmediately)
    : mJob(aJob)
    , mSuccess(aSuccess)
    , mActivateImmediately(aActivateImmediately)
  {
    MOZ_ASSERT(!NS_IsMainThread());
  }

  NS_IMETHOD
  Run() MOZ_OVERRIDE;
};

/*
 * Fires 'install' event on the ServiceWorkerGlobalScope. Modifies busy count
 * since it fires the event. This is ok since there can't be nested
 * ServiceWorkers, so the parent thread -> worker thread requirement for
 * runnables is satisfied.
 */
class InstallEventRunnable MOZ_FINAL : public WorkerRunnable
{
  nsMainThreadPtrHandle<nsISupports> mJob;
  nsCString mScope;

public:
  InstallEventRunnable(
    WorkerPrivate* aWorkerPrivate,
    const nsMainThreadPtrHandle<nsISupports>& aJob,
    const nsCString& aScope)
      : WorkerRunnable(aWorkerPrivate, WorkerThreadModifyBusyCount),
        mJob(aJob),
        mScope(aScope)
  {
    AssertIsOnMainThread();
    MOZ_ASSERT(aWorkerPrivate);
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate);
    return DispatchInstallEvent(aCx, aWorkerPrivate);
  }

private:
  bool
  DispatchInstallEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate);

};

class ServiceWorkerRegisterJob MOZ_FINAL : public ServiceWorkerJob,
                                           public nsIStreamLoaderObserver
{
  nsCString mScope;
  nsCString mScriptSpec;
  // We need the global so we can call the Request constructor to set things up
  // for us.
  nsCOMPtr<nsIGlobalObject> mGlobal;
  nsRefPtr<ServiceWorkerRegistrationInfo> mRegistration;
  // The promise "returned" by the call to Update up to
  // navigator.serviceWorker.register().
  nsRefPtr<Promise> mUpdatePromise;

  ~ServiceWorkerRegisterJob()
  { }

  enum JobType
  {
    REGISTER_JOB = 0
  };
  DebugOnly<JobType> mJobType;

public:
  NS_DECL_ISUPPORTS
  ServiceWorkerRegisterJob(ServiceWorkerJobQueue* aQueue,
                           const nsCString& aScope,
                           const nsCString& aScriptSpec,
                           nsIGlobalObject* aGlobal,
                           Promise* aUpdatePromise)
    : ServiceWorkerJob(aQueue)
    , mScope(aScope)
    , mScriptSpec(aScriptSpec)
    , mGlobal(aGlobal)
    , mUpdatePromise(aUpdatePromise)
    , mJobType(REGISTER_JOB)
  {
  }

  void
  Start() MOZ_OVERRIDE
  {
    MOZ_ASSERT(mJobType == REGISTER_JOB);
    nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(mGlobal);

    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
    nsRefPtr<ServiceWorkerManager::ServiceWorkerDomainInfo> domainInfo =
      swm->GetDomainInfo(mScope);
    MOZ_ASSERT(domainInfo);
    mRegistration = domainInfo->GetRegistration(mScope);

    if (mRegistration) {
      nsRefPtr<ServiceWorkerInfo> newest = mRegistration->Newest();
      if (newest && mScriptSpec.Equals(newest->GetScriptSpec()) &&
          mScriptSpec.Equals(mRegistration->mScriptSpec)) {
        mRegistration->mPendingUninstall = false;
        nsRefPtr<ServiceWorkerRegistration> swr =
          new ServiceWorkerRegistration(window,
                                        NS_ConvertUTF8toUTF16(mRegistration->mScope));

        mUpdatePromise->MaybeResolve(swr);
        return Done(NS_OK);
      }
    } else {
      mRegistration = domainInfo->CreateNewRegistration(mScope);
    }

    mRegistration->mScriptSpec = mScriptSpec;
    Update();
  }

  void
  Update()
  {
    nsCOMPtr<nsIRunnable> r =
      NS_NewRunnableMethod(this, &ServiceWorkerRegisterJob::ContinueUpdate);
    NS_DispatchToMainThread(r);
  }

  // Aspects of (actually the whole algorithm) of [[Update]] after
  // "Run the following steps in parallel."
  void
  ContinueUpdate()
  {
    if (mRegistration->mInstallingWorker) {
      // FIXME(nsm): "Terminate installing worker".
      // "Run the update state"
      mRegistration->mInstallingWorker = nullptr;
    }

    // FIXME(nsm): Plug in FetchDriver when it is ready.
    nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(mGlobal);
    MOZ_ASSERT(window);
    MOZ_ASSERT(window->GetExtantDoc());

    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), mRegistration->mScriptSpec, nullptr, nullptr);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return Fail(rv);
    }
    nsCOMPtr<nsIChannel> channel;
    rv = NS_NewChannel(getter_AddRefs(channel),
                       uri,
                       window->GetExtantDoc(),
                       nsILoadInfo::SEC_NORMAL,
                       nsIContentPolicy::TYPE_SCRIPT); // FIXME(nsm): TYPE_SERVICEWORKER
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return Fail(rv);
    }
    // FIXME(nsm): Set redirect limit.

    // Don't let serviceworker intercept.
    nsCOMPtr<nsIHttpChannelInternal> internalChannel = do_QueryInterface(channel);
    if (internalChannel) {
      internalChannel->ForceNoIntercept();
    }

    nsCOMPtr<nsIStreamLoader> loader;
    rv = NS_NewStreamLoader(getter_AddRefs(loader), this);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return Fail(rv);
    }

    rv = channel->AsyncOpen(loader, nullptr);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return Fail(rv);
    }
  }

  void
  Fail(nsresult rv)
  {
    nsRefPtr<Promise> p = mUpdatePromise.forget();
    if (p) {
      p->MaybeReject(rv);
    }
    Done(rv);
  }

  NS_IMETHOD
  OnStreamComplete(nsIStreamLoader* aLoader, nsISupports* aContext,
                   nsresult aStatus, uint32_t aLen,
                   const uint8_t* aString) MOZ_OVERRIDE
  {
    if (NS_WARN_IF(NS_FAILED(aStatus))) {
      Fail(NS_ERROR_DOM_NETWORK_ERR);
      return aStatus;
    }

    nsCOMPtr<nsIRequest> request;
    nsresult rv = aLoader->GetRequest(getter_AddRefs(request));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      Fail(NS_ERROR_DOM_NETWORK_ERR);
      return rv;
    }

    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(request);
    if (!httpChannel) {
      Fail(NS_ERROR_DOM_NETWORK_ERR);
      return NS_ERROR_FAILURE;
    }

    bool requestSucceeded;
    rv = httpChannel->GetRequestSucceeded(&requestSucceeded);
    if (NS_WARN_IF(NS_FAILED(rv) || !requestSucceeded)) {
      Fail(NS_ERROR_DOM_NETWORK_ERR);
      return rv;
    }


    // FIXME(nsm): "Extract mime type..."
    // FIXME(nsm): Byte match to aString.
    NS_WARNING("Byte wise check is disabled, just using new one");
    ContinueInstall();
    return NS_OK;
  }

  void
  ContinueInstall()
  {
    if (mRegistration->mInstallingWorker) {
      // FIXME(nsm): Terminate and stuff
    }

    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
    swm->InvalidateServiceWorkerRegistrationWorker(mRegistration,
                                                   WhichServiceWorker::INSTALLING_WORKER);
    mRegistration->mInstallingWorker = new ServiceWorkerInfo(mRegistration->mScriptSpec);
    mRegistration->mInstallingWorker->UpdateState(ServiceWorkerState::Installing);
    nsRefPtr<Promise> p = mUpdatePromise.forget();
    MOZ_ASSERT(p);

    nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(mGlobal);
    MOZ_ASSERT(window);
    NS_ConvertUTF8toUTF16 scope(mRegistration->mScope);
    nsRefPtr<ServiceWorkerRegistration> swr =
      new ServiceWorkerRegistration(window, scope);
    p->MaybeResolve(swr);

    nsRefPtr<QueueFireUpdateFoundRunnable> upr =
      new QueueFireUpdateFoundRunnable(mRegistration);
    NS_DispatchToMainThread(upr);

    // XXXnsm this leads to double fetches right now, ideally we'll be able to
    // use the persistent cache later.
    nsRefPtr<ServiceWorkerJob> upcasted = this;
    nsMainThreadPtrHandle<nsISupports> handle(
        new nsMainThreadPtrHolder<nsISupports>(upcasted));

    nsRefPtr<ServiceWorker> serviceWorker;
    nsresult rv =
      swm->CreateServiceWorker(mRegistration->mInstallingWorker->GetScriptSpec(),
                               mRegistration->mScope,
                               getter_AddRefs(serviceWorker));

    if (NS_WARN_IF(NS_FAILED(rv))) {
      // FIXME(nsm):
      MOZ_CRASH("Shouldn't happen yet");
    }

    nsRefPtr<InstallEventRunnable> r =
      new InstallEventRunnable(serviceWorker->GetWorkerPrivate(), handle, mRegistration->mScope);

    AutoSafeJSContext cx;
    r->Dispatch(cx);
  }

  void
  ContinueAfterInstallEvent(bool aSuccess, bool aActivateImmediately)
  {
    if (!mRegistration->mInstallingWorker) {
      NS_WARNING("mInstallingWorker was null.");
      return Done(NS_ERROR_DOM_ABORT_ERR);
    }

    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();

    // "If installFailed is true"
    if (!aSuccess) {
      mRegistration->mInstallingWorker->UpdateState(ServiceWorkerState::Redundant);
      mRegistration->mInstallingWorker = nullptr;
      swm->InvalidateServiceWorkerRegistrationWorker(mRegistration,
                                                     WhichServiceWorker::INSTALLING_WORKER);
      return Done(NS_ERROR_DOM_ABORT_ERR);
    }

    // "If registration's waiting worker is not null"
    if (mRegistration->mWaitingWorker) {
      // FIXME(nsm): Terminate
      mRegistration->mWaitingWorker->UpdateState(ServiceWorkerState::Redundant);
    }

    mRegistration->mWaitingWorker = mRegistration->mInstallingWorker.forget();
    mRegistration->mWaitingToActivate = false;
    mRegistration->mWaitingWorker->UpdateState(ServiceWorkerState::Installed);
    swm->InvalidateServiceWorkerRegistrationWorker(mRegistration,
                                                   WhichServiceWorker::INSTALLING_WORKER | WhichServiceWorker::WAITING_WORKER);

    // FIXME(nsm): Bug 982711 Deal with activateImmediately.
    NS_WARN_IF_FALSE(!aActivateImmediately, "Immediate activation using replace() is not supported yet");
    mRegistration->TryToActivate();
    Done(NS_OK);
  }
};

NS_IMPL_ISUPPORTS_INHERITED(ServiceWorkerRegisterJob, ServiceWorkerJob, nsIStreamLoaderObserver)

// If we return an error code here, the ServiceWorkerContainer will
// automatically reject the Promise.
NS_IMETHODIMP
ServiceWorkerManager::Register(nsIDOMWindow* aWindow,
                               const nsAString& aScope,
                               const nsAString& aScriptURL,
                               nsISupports** aPromise)
{
  AssertIsOnMainThread();

  // XXXnsm Don't allow chrome callers for now, we don't support chrome
  // ServiceWorkers.
  MOZ_ASSERT(!nsContentUtils::IsCallerChrome());

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  nsCOMPtr<nsIGlobalObject> sgo = do_QueryInterface(window);
  MOZ_ASSERT(sgo, "Register() should only be called from a valid entry settings object!");

  nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();
  if (!doc) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIURI> documentURI = doc->GetBaseURI();

  bool authenticatedOrigin = false;
  // FIXME(nsm): Bug 1003991. Disable check when devtools are open.
  if (Preferences::GetBool("dom.serviceWorkers.testing.enabled")) {
    authenticatedOrigin = true;
  }

  ErrorResult result;
  if (!authenticatedOrigin) {
    bool isHttps, isFile, isApp;
    result = documentURI->SchemeIs("https", &isHttps);
    if (result.Failed()) {
      return NS_ERROR_FAILURE;
    }

    result = documentURI->SchemeIs("file", &isFile);
    if (result.Failed()) {
      return NS_ERROR_FAILURE;
    }

    result = documentURI->SchemeIs("app", &isApp);
    if (result.Failed()) {
      return NS_ERROR_FAILURE;
    }

    authenticatedOrigin = isHttps || isFile || isApp;
  }

  if (!authenticatedOrigin) {
    nsAutoCString host;
    result = documentURI->GetHost(host);
    if (NS_WARN_IF(result.Failed())) {
      return result.ErrorCode();
    }

    if (host.Equals("127.0.0.1") ||
        host.Equals("localhost") ||
        host.Equals("::1")) {
      authenticatedOrigin = true;
    }
  }

  if (!authenticatedOrigin) {
    bool isFile;
    result = documentURI->SchemeIs("file", &isFile);
    if (result.Failed() || !isFile) {
      bool isHttps;
      result = documentURI->SchemeIs("https", &isHttps);
      if (result.Failed() || !isHttps) {
        NS_WARNING("ServiceWorker registration from insecure websites is not allowed.");
        return NS_ERROR_DOM_SECURITY_ERR;
      }
    }
  }

  nsCOMPtr<nsIURI> scriptURI;
  result = NS_NewURI(getter_AddRefs(scriptURI), aScriptURL, nullptr, documentURI);
  if (NS_WARN_IF(result.Failed())) {
    return result.ErrorCode();
  }

  // Data URLs are not allowed.
  nsCOMPtr<nsIPrincipal> documentPrincipal = doc->NodePrincipal();

  result = documentPrincipal->CheckMayLoad(scriptURI, true /* report */,
                                           false /* allowIfInheritsPrincipal */);
  if (result.Failed()) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  nsCOMPtr<nsIURI> scopeURI;
  result = NS_NewURI(getter_AddRefs(scopeURI), aScope, nullptr, documentURI);
  if (NS_WARN_IF(result.Failed())) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  result = documentPrincipal->CheckMayLoad(scopeURI, true /* report */,
                                           false /* allowIfInheritsPrinciple */);
  if (result.Failed()) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  nsCString cleanedScope;
  result = scopeURI->GetSpecIgnoringRef(cleanedScope);
  if (NS_WARN_IF(result.Failed())) {
    return NS_ERROR_FAILURE;
  }

  nsCString spec;
  nsresult rv = scriptURI->GetSpec(spec);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }


  nsRefPtr<ServiceWorkerManager::ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(cleanedScope);
  if (!domainInfo) {
    nsCString domain;
    nsresult rv = scriptURI->GetHost(domain);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    domainInfo = new ServiceWorkerManager::ServiceWorkerDomainInfo;
    mDomainMap.Put(domain, domainInfo);
  }

  nsRefPtr<Promise> promise = Promise::Create(sgo, result);
  if (result.Failed()) {
    return result.ErrorCode();
  }

  ServiceWorkerJobQueue* queue = domainInfo->GetOrCreateJobQueue(cleanedScope);
  MOZ_ASSERT(queue);

  nsRefPtr<ServiceWorkerRegisterJob> job
    = new ServiceWorkerRegisterJob(queue, cleanedScope, spec, sgo, promise);
  queue->Append(job);

  promise.forget(aPromise);
  return NS_OK;
}

NS_IMETHODIMP
FinishInstallRunnable::Run()
{
  AssertIsOnMainThread();
  nsRefPtr<ServiceWorkerJob> job = static_cast<ServiceWorkerJob*>(mJob.get());
  nsRefPtr<ServiceWorkerRegisterJob> upjob = static_cast<ServiceWorkerRegisterJob*>(job.get());
  MOZ_ASSERT(upjob);
  upjob->ContinueAfterInstallEvent(mSuccess, mActivateImmediately);
  return NS_OK;
}

/*
 * Used to handle InstallEvent::waitUntil() and proceed with installation.
 */
class FinishInstallHandler MOZ_FINAL : public PromiseNativeHandler
{
  nsMainThreadPtrHandle<nsISupports> mJob;
  bool mActivateImmediately;

  virtual
  ~FinishInstallHandler()
  { }

public:
  explicit FinishInstallHandler(
    const nsMainThreadPtrHandle<nsISupports>& aJob,
    bool aActivateImmediately)
    : mJob(aJob)
    , mActivateImmediately(aActivateImmediately)
  {
    MOZ_ASSERT(!NS_IsMainThread());
  }

  void
  ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) MOZ_OVERRIDE
  {
    WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(workerPrivate);
    workerPrivate->AssertIsOnWorkerThread();

    nsRefPtr<FinishInstallRunnable> r = new FinishInstallRunnable(mJob, true, mActivateImmediately);
    NS_DispatchToMainThread(r);
  }

  void
  RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) MOZ_OVERRIDE
  {
    nsRefPtr<FinishInstallRunnable> r = new FinishInstallRunnable(mJob, false, mActivateImmediately);
    NS_DispatchToMainThread(r);
  }
};

bool
InstallEventRunnable::DispatchInstallEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
{
  aWorkerPrivate->AssertIsOnWorkerThread();
  MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
  InstallEventInit init;
  init.mBubbles = false;
  init.mCancelable = true;

  // FIXME(nsm): Bug 982787 pass previous active worker.

  // FIXME(nsm): Set error handler so we can grab handler errors.
  nsRefPtr<EventTarget> target = aWorkerPrivate->GlobalScope();
  nsRefPtr<InstallEvent> event =
    InstallEvent::Constructor(target, NS_LITERAL_STRING("install"), init);

  event->SetTrusted(true);

  nsRefPtr<Promise> waitUntilPromise;

  nsresult rv = target->DispatchDOMEvent(nullptr, event, nullptr, nullptr);

  nsCOMPtr<nsIGlobalObject> sgo = aWorkerPrivate->GlobalScope();
  if (NS_SUCCEEDED(rv)) {
    waitUntilPromise = event->GetPromise();
    if (!waitUntilPromise) {
      ErrorResult rv;
      waitUntilPromise =
        Promise::Resolve(sgo,
                         aCx, JS::UndefinedHandleValue, rv);
    }
  } else {
    ErrorResult rv;
    // Continue with a canceled install.
    waitUntilPromise = Promise::Reject(sgo, aCx,
                                       JS::UndefinedHandleValue, rv);
  }
  // FIXME(nsm): handle script errors.

  nsRefPtr<FinishInstallHandler> handler =
    new FinishInstallHandler(mJob, event->ActivateImmediately());
  waitUntilPromise->AppendNativeHandler(handler);
  return true;
}

already_AddRefed<Promise>
ServiceWorkerManager::Update(nsIGlobalObject* aGlobal,
                             ServiceWorkerRegistrationInfo* aRegistration)
{
  MOZ_ASSERT(false, "DONT CALL THIS YET");
  return nullptr;
}

class FinishActivationRunnable : public nsRunnable
{
  bool mSuccess;
  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> mRegistration;

public:
  explicit FinishActivationRunnable(
      bool aSuccess,
      const nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo>& aRegistration)
    : mSuccess(aSuccess)
    , mRegistration(aRegistration)
  {
    MOZ_ASSERT(!NS_IsMainThread());
  }

  NS_IMETHODIMP
  Run()
  {
    AssertIsOnMainThread();

    mRegistration->FinishActivate(mSuccess);
    return NS_OK;
  }
};

class FinishActivateHandler : public PromiseNativeHandler
{
  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> mRegistration;

public:
  explicit FinishActivateHandler(const nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo>& aRegistration)
    : mRegistration(aRegistration)
  {
    MOZ_ASSERT(!NS_IsMainThread());
  }

  virtual
  ~FinishActivateHandler()
  { }

  void
  ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) MOZ_OVERRIDE
  {
    nsRefPtr<FinishActivationRunnable> r = new FinishActivationRunnable(true /* success */, mRegistration);
    NS_DispatchToMainThread(r);
  }

  void
  RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) MOZ_OVERRIDE
  {
    nsRefPtr<FinishActivationRunnable> r = new FinishActivationRunnable(false /* success */, mRegistration);
    NS_DispatchToMainThread(r);
  }
};

class ActivateEventRunnable : public WorkerRunnable
{
  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> mRegistration;

public:
  ActivateEventRunnable(WorkerPrivate* aWorkerPrivate,
                        const nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo>& aRegistration)
      : WorkerRunnable(aWorkerPrivate, WorkerThreadModifyBusyCount),
        mRegistration(aRegistration)
  {
    MOZ_ASSERT(aWorkerPrivate);
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate);
    return DispatchActivateEvent(aCx, aWorkerPrivate);
  }

private:
  bool
  DispatchActivateEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
    nsRefPtr<EventTarget> target = do_QueryObject(aWorkerPrivate->GlobalScope());

    // FIXME(nsm): Set activeWorker to the correct thing.
    EventInit init;
    init.mBubbles = false;
    init.mCancelable = true;
    nsRefPtr<InstallPhaseEvent> event =
      InstallPhaseEvent::Constructor(target, NS_LITERAL_STRING("activate"), init);

    event->SetTrusted(true);

    nsRefPtr<Promise> waitUntilPromise;

    // FIXME(nsm): Install error handler for any listener errors.
    nsresult rv = target->DispatchDOMEvent(nullptr, event, nullptr, nullptr);
    if (NS_SUCCEEDED(rv)) {
      waitUntilPromise = event->GetPromise();
      if (!waitUntilPromise) {
        ErrorResult rv;
        nsCOMPtr<nsIGlobalObject> global =
          do_QueryObject(aWorkerPrivate->GlobalScope());
        waitUntilPromise =
          Promise::Resolve(global,
                           aCx, JS::UndefinedHandleValue, rv);
      }
    } else {
      ErrorResult rv;
      nsCOMPtr<nsIGlobalObject> global =
        do_QueryObject(aWorkerPrivate->GlobalScope());
      // Continue with a canceled install.
      waitUntilPromise = Promise::Reject(global, aCx,
                                         JS::UndefinedHandleValue, rv);
    }

    nsRefPtr<FinishActivateHandler> handler = new FinishActivateHandler(mRegistration);
    waitUntilPromise->AppendNativeHandler(handler);
    return true;
  }
};

void
ServiceWorkerRegistrationInfo::TryToActivate()
{
  mWaitingToActivate = true;
  if (!IsControllingDocuments()) {
    Activate();
  }
}

void
ServiceWorkerRegistrationInfo::Activate()
{
  MOZ_ASSERT(mWaitingToActivate);
  mWaitingToActivate = false;

  nsRefPtr<ServiceWorkerInfo> activatingWorker = mWaitingWorker;
  nsRefPtr<ServiceWorkerInfo> exitingWorker = mActiveWorker;

  nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
  swm->InvalidateServiceWorkerRegistrationWorker(this, WhichServiceWorker::WAITING_WORKER | WhichServiceWorker::ACTIVE_WORKER);
  if (!activatingWorker) {
    NS_WARNING("No activatingWorker!");
    return;
  }

  if (exitingWorker) {
    // FIXME(nsm): Wait for worker.
    // Terminate worker
    exitingWorker->UpdateState(ServiceWorkerState::Redundant);
  }

  mActiveWorker = activatingWorker.forget();
  mWaitingWorker = nullptr;
  mActiveWorker->UpdateState(ServiceWorkerState::Activating);

  swm->CheckPendingReadyPromises();

  // "Queue a task to fire a simple event named controllerchange..."
  nsCOMPtr<nsIRunnable> controllerChangeRunnable =
    NS_NewRunnableMethodWithArg<ServiceWorkerRegistrationInfo*>(swm, &ServiceWorkerManager::FireControllerChange, this);
  NS_DispatchToMainThread(controllerChangeRunnable);

  // XXXnsm I have my doubts about this. Leaving the main thread means that
  // subsequent calls to Activate() not from a Register() call, i.e. due to all
  // controlled documents going away, may lead to two or more calls being
  // interleaved.
  MOZ_ASSERT(mActiveWorker);
  nsRefPtr<ServiceWorker> serviceWorker;
  nsresult rv =
    swm->CreateServiceWorker(mActiveWorker->GetScriptSpec(),
                             mScope,
                             getter_AddRefs(serviceWorker));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    // FIXME(nsm): Do the same thing as when "activate" event failed to
    // dispatch.
  }

  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> handle(
    new nsMainThreadPtrHolder<ServiceWorkerRegistrationInfo>(this));

  nsRefPtr<ActivateEventRunnable> r =
    new ActivateEventRunnable(serviceWorker->GetWorkerPrivate(), handle);

  AutoSafeJSContext cx;
  r->Dispatch(cx);
}

/*
 * Implements the async aspects of the getRegistrations algorithm.
 */
class GetRegistrationsRunnable : public nsRunnable
{
  nsCOMPtr<nsPIDOMWindow> mWindow;
  nsRefPtr<Promise> mPromise;
public:
  GetRegistrationsRunnable(nsPIDOMWindow* aWindow, Promise* aPromise)
    : mWindow(aWindow), mPromise(aPromise)
  { }

  NS_IMETHODIMP
  Run()
  {
    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();

    nsIDocument* doc = mWindow->GetExtantDoc();
    if (!doc) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    nsCOMPtr<nsIURI> docURI = doc->GetDocumentURI();
    if (!docURI) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    nsCOMPtr<nsIPrincipal> principal = doc->NodePrincipal();
    if (!principal) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    nsTArray<nsRefPtr<ServiceWorkerRegistration>> array;

    nsRefPtr<ServiceWorkerManager::ServiceWorkerDomainInfo> domainInfo =
      swm->GetDomainInfo(docURI);

    if (!domainInfo) {
      mPromise->MaybeResolve(array);
      return NS_OK;
    }

    for (uint32_t i = 0; i < domainInfo->mOrderedScopes.Length(); ++i) {
      NS_ConvertUTF8toUTF16 scope(domainInfo->mOrderedScopes[i]);
      nsRefPtr<ServiceWorkerRegistration> swr =
        new ServiceWorkerRegistration(mWindow, scope);

      array.AppendElement(swr);
    }

    mPromise->MaybeResolve(array);
    return NS_OK;
  }
};

// If we return an error code here, the ServiceWorkerContainer will
// automatically reject the Promise.
NS_IMETHODIMP
ServiceWorkerManager::GetRegistrations(nsIDOMWindow* aWindow,
                                       nsISupports** aPromise)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aWindow);

  // XXXnsm Don't allow chrome callers for now, we don't support chrome
  // ServiceWorkers.
  MOZ_ASSERT(!nsContentUtils::IsCallerChrome());

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  if (!window) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIGlobalObject> sgo = do_QueryInterface(window);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(sgo, result);
  if (result.Failed()) {
    return result.ErrorCode();
  }

  nsRefPtr<nsIRunnable> runnable =
    new GetRegistrationsRunnable(window, promise);
  promise.forget(aPromise);
  return NS_DispatchToCurrentThread(runnable);
}

/*
 * Implements the async aspects of the getRegistration algorithm.
 */
class GetRegistrationRunnable : public nsRunnable
{
  nsCOMPtr<nsPIDOMWindow> mWindow;
  nsRefPtr<Promise> mPromise;
  nsString mDocumentURL;

public:
  GetRegistrationRunnable(nsPIDOMWindow* aWindow, Promise* aPromise,
                          const nsAString& aDocumentURL)
    : mWindow(aWindow), mPromise(aPromise), mDocumentURL(aDocumentURL)
  { }

  NS_IMETHODIMP
  Run()
  {
    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();

    nsIDocument* doc = mWindow->GetExtantDoc();
    if (!doc) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    nsCOMPtr<nsIURI> docURI = doc->GetDocumentURI();
    if (!docURI) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    nsCOMPtr<nsIURI> uri;
    nsresult rv = NS_NewURI(getter_AddRefs(uri), mDocumentURL, nullptr, docURI);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mPromise->MaybeReject(rv);
      return NS_OK;
    }

    nsCOMPtr<nsIPrincipal> principal = doc->NodePrincipal();
    if (!principal) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    rv = principal->CheckMayLoad(uri, true /* report */,
                                 false /* allowIfInheritsPrinciple */);
    if (NS_FAILED(rv)) {
      mPromise->MaybeReject(NS_ERROR_DOM_SECURITY_ERR);
      return NS_OK;
    }

    nsRefPtr<ServiceWorkerRegistrationInfo> registration =
      swm->GetServiceWorkerRegistrationInfo(uri);

    if (!registration) {
      mPromise->MaybeResolve(JS::UndefinedHandleValue);
      return NS_OK;
    }

    NS_ConvertUTF8toUTF16 scope(registration->mScope);
    nsRefPtr<ServiceWorkerRegistration> swr =
      new ServiceWorkerRegistration(mWindow, scope);
    mPromise->MaybeResolve(swr);

    return NS_OK;
  }
};

// If we return an error code here, the ServiceWorkerContainer will
// automatically reject the Promise.
NS_IMETHODIMP
ServiceWorkerManager::GetRegistration(nsIDOMWindow* aWindow,
                                      const nsAString& aDocumentURL,
                                      nsISupports** aPromise)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aWindow);

  // XXXnsm Don't allow chrome callers for now, we don't support chrome
  // ServiceWorkers.
  MOZ_ASSERT(!nsContentUtils::IsCallerChrome());

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  if (!window) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIGlobalObject> sgo = do_QueryInterface(window);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(sgo, result);
  if (result.Failed()) {
    return result.ErrorCode();
  }

  nsRefPtr<nsIRunnable> runnable =
    new GetRegistrationRunnable(window, promise, aDocumentURL);
  promise.forget(aPromise);
  return NS_DispatchToCurrentThread(runnable);
}

class GetReadyPromiseRunnable : public nsRunnable
{
  nsCOMPtr<nsPIDOMWindow> mWindow;
  nsRefPtr<Promise> mPromise;

public:
  GetReadyPromiseRunnable(nsPIDOMWindow* aWindow, Promise* aPromise)
    : mWindow(aWindow), mPromise(aPromise)
  { }

  NS_IMETHODIMP
  Run()
  {
    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();

    nsIDocument* doc = mWindow->GetExtantDoc();
    if (!doc) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    nsCOMPtr<nsIURI> docURI = doc->GetDocumentURI();
    if (!docURI) {
      mPromise->MaybeReject(NS_ERROR_UNEXPECTED);
      return NS_OK;
    }

    if (!swm->CheckReadyPromise(mWindow, docURI, mPromise)) {
      swm->StorePendingReadyPromise(mWindow, docURI, mPromise);
    }

    return NS_OK;
  }
};

NS_IMETHODIMP
ServiceWorkerManager::GetReadyPromise(nsIDOMWindow* aWindow,
                                      nsISupports** aPromise)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aWindow);

  // XXXnsm Don't allow chrome callers for now, we don't support chrome
  // ServiceWorkers.
  MOZ_ASSERT(!nsContentUtils::IsCallerChrome());

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  if (!window) {
    return NS_ERROR_FAILURE;
  }

  MOZ_ASSERT(!mPendingReadyPromises.Contains(window));

  nsCOMPtr<nsIGlobalObject> sgo = do_QueryInterface(window);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(sgo, result);
  if (result.Failed()) {
    return result.ErrorCode();
  }

  nsRefPtr<nsIRunnable> runnable =
    new GetReadyPromiseRunnable(window, promise);
  promise.forget(aPromise);
  return NS_DispatchToCurrentThread(runnable);
}

NS_IMETHODIMP
ServiceWorkerManager::RemoveReadyPromise(nsIDOMWindow* aWindow)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aWindow);

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  if (!window) {
    return NS_ERROR_FAILURE;
  }

  mPendingReadyPromises.Remove(aWindow);
  return NS_OK;
}

void
ServiceWorkerManager::StorePendingReadyPromise(nsPIDOMWindow* aWindow,
                                               nsIURI* aURI,
                                               Promise* aPromise)
{
  PendingReadyPromise* data;

  // We should not have 2 pending promises for the same window.
  MOZ_ASSERT(!mPendingReadyPromises.Get(aWindow, &data));

  data = new PendingReadyPromise(aURI, aPromise);
  mPendingReadyPromises.Put(aWindow, data);
}

void
ServiceWorkerManager::CheckPendingReadyPromises()
{
  mPendingReadyPromises.Enumerate(CheckPendingReadyPromisesEnumerator, this);
}

PLDHashOperator
ServiceWorkerManager::CheckPendingReadyPromisesEnumerator(
                                          nsISupports* aSupports,
                                          nsAutoPtr<PendingReadyPromise>& aData,
                                          void* aPtr)
{
  ServiceWorkerManager* aSwm = static_cast<ServiceWorkerManager*>(aPtr);

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aSupports);

  if (aSwm->CheckReadyPromise(window, aData->mURI, aData->mPromise)) {
    return PL_DHASH_REMOVE;
  }

  return PL_DHASH_NEXT;
}

bool
ServiceWorkerManager::CheckReadyPromise(nsPIDOMWindow* aWindow,
                                        nsIURI* aURI, Promise* aPromise)
{
  nsRefPtr<ServiceWorkerRegistrationInfo> registration =
    GetServiceWorkerRegistrationInfo(aURI);

  if (registration && registration->mActiveWorker) {
    NS_ConvertUTF8toUTF16 scope(registration->mScope);
    nsRefPtr<ServiceWorkerRegistration> swr =
      new ServiceWorkerRegistration(aWindow, scope);
    aPromise->MaybeResolve(swr);
    return true;
  }

  return false;
}

NS_IMETHODIMP
ServiceWorkerManager::Unregister(nsIServiceWorkerUnregisterCallback* aCallback,
                                 const nsAString& aScope)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aCallback);

  nsCOMPtr<nsIURI> scopeURI;
  nsresult rv = NS_NewURI(getter_AddRefs(scopeURI), aScope, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  /*
   * Implements the async aspects of the unregister algorithm.
   */
  class UnregisterRunnable : public nsRunnable
  {
    nsCOMPtr<nsIServiceWorkerUnregisterCallback> mCallback;
    nsCOMPtr<nsIURI> mScopeURI;

  public:
    UnregisterRunnable(nsIServiceWorkerUnregisterCallback* aCallback,
                       nsIURI* aScopeURI)
      : mCallback(aCallback), mScopeURI(aScopeURI)
    {
      AssertIsOnMainThread();
    }

    NS_IMETHODIMP
    Run()
    {
      AssertIsOnMainThread();

      nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();

      nsRefPtr<ServiceWorkerManager::ServiceWorkerDomainInfo> domainInfo =
        swm->GetDomainInfo(mScopeURI);
      MOZ_ASSERT(domainInfo);

      nsCString spec;
      nsresult rv = mScopeURI->GetSpecIgnoringRef(spec);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return mCallback->UnregisterFailed();
      }

      nsRefPtr<ServiceWorkerRegistrationInfo> registration;
      if (!domainInfo->mServiceWorkerRegistrationInfos.Get(spec,
                                                           getter_AddRefs(registration))) {
        return mCallback->UnregisterSucceeded(false);
      }

      MOZ_ASSERT(registration);

      registration->mPendingUninstall = true;
      rv = mCallback->UnregisterSucceeded(true);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      // The "Wait until no document is using registration" can actually be
      // handled by [[HandleDocumentUnload]] in Bug 1041340, so we simply check
      // if the document is currently in use here.
      if (!registration->IsControllingDocuments()) {
        if (!registration->mPendingUninstall) {
          return NS_OK;
        }

        registration->Clear();
        domainInfo->RemoveRegistration(registration);
      }

      return NS_OK;
    }
  };

  nsRefPtr<nsIRunnable> unregisterRunnable =
    new UnregisterRunnable(aCallback, scopeURI);
  return NS_DispatchToCurrentThread(unregisterRunnable);
}

/* static */
already_AddRefed<ServiceWorkerManager>
ServiceWorkerManager::GetInstance()
{
  nsCOMPtr<nsIServiceWorkerManager> swm = mozilla::services::GetServiceWorkerManager();
  nsRefPtr<ServiceWorkerManager> concrete = do_QueryObject(swm);
  return concrete.forget();
}

void
ServiceWorkerManager::FinishFetch(ServiceWorkerRegistrationInfo* aRegistration)
{
}

void
ServiceWorkerManager::HandleError(JSContext* aCx,
                                  const nsACString& aScope,
                                  const nsAString& aWorkerURL,
                                  nsString aMessage,
                                  nsString aFilename,
                                  nsString aLine,
                                  uint32_t aLineNumber,
                                  uint32_t aColumnNumber,
                                  uint32_t aFlags)
{
  AssertIsOnMainThread();

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri), aScope, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return;
  }

  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(uri);
  if (!domainInfo) {
    return;
  }

  nsCString scope;
  scope.Assign(aScope);
  nsRefPtr<ServiceWorkerRegistrationInfo> registration = domainInfo->GetRegistration(scope);
  MOZ_ASSERT(registration);

  RootedDictionary<ErrorEventInit> init(aCx);
  init.mMessage = aMessage;
  init.mFilename = aFilename;
  init.mLineno = aLineNumber;
  init.mColno = aColumnNumber;

  MOZ_CRASH("FIX THIS");
}

void
ServiceWorkerRegistrationInfo::FinishActivate(bool aSuccess)
{
  MOZ_ASSERT(mActiveWorker);
  if (aSuccess) {
    mActiveWorker->UpdateState(ServiceWorkerState::Activated);
  } else {
    mActiveWorker->UpdateState(ServiceWorkerState::Redundant);
    mActiveWorker = nullptr;
  }
}

NS_IMETHODIMP
ServiceWorkerManager::CreateServiceWorkerForWindow(nsPIDOMWindow* aWindow,
                                                   const nsACString& aScriptSpec,
                                                   const nsACString& aScope,
                                                   ServiceWorker** aServiceWorker)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aWindow);

  RuntimeService* rs = RuntimeService::GetOrCreateService();
  nsRefPtr<ServiceWorker> serviceWorker;

  nsCOMPtr<nsIGlobalObject> sgo = do_QueryInterface(aWindow);

  AutoSafeJSContext cx;
  JS::Rooted<JSObject*> jsGlobal(cx, sgo->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);
  nsresult rv = rs->CreateServiceWorker(global,
                                        NS_ConvertUTF8toUTF16(aScriptSpec),
                                        aScope,
                                        getter_AddRefs(serviceWorker));

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  serviceWorker.forget(aServiceWorker);
  return rv;
}

already_AddRefed<ServiceWorkerRegistrationInfo>
ServiceWorkerManager::GetServiceWorkerRegistrationInfo(nsPIDOMWindow* aWindow)
{
  nsCOMPtr<nsIDocument> document = aWindow->GetExtantDoc();
  return GetServiceWorkerRegistrationInfo(document);
}

already_AddRefed<ServiceWorkerRegistrationInfo>
ServiceWorkerManager::GetServiceWorkerRegistrationInfo(nsIDocument* aDoc)
{
  nsCOMPtr<nsIURI> documentURI = aDoc->GetDocumentURI();
  return GetServiceWorkerRegistrationInfo(documentURI);
}

already_AddRefed<ServiceWorkerRegistrationInfo>
ServiceWorkerManager::GetServiceWorkerRegistrationInfo(nsIURI* aURI)
{
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(aURI);
  if (!domainInfo) {
    return nullptr;
  }

  nsCString spec;
  nsresult rv = aURI->GetSpec(spec);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  nsCString scope = FindScopeForPath(domainInfo->mOrderedScopes, spec);
  if (scope.IsEmpty()) {
    return nullptr;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> registration;
  domainInfo->mServiceWorkerRegistrationInfos.Get(scope, getter_AddRefs(registration));
  // ordered scopes and registrations better be in sync.
  MOZ_ASSERT(registration);

  if (registration->mPendingUninstall) {
    return nullptr;
  }
  return registration.forget();
}

/* static */ void
ServiceWorkerManager::AddScope(nsTArray<nsCString>& aList, const nsACString& aScope)
{
  for (uint32_t i = 0; i < aList.Length(); ++i) {
    const nsCString& current = aList[i];

    // Perfect match!
    if (aScope.Equals(current)) {
      return;
    }

    // Sort by length, with longest match first.
    // /foo/bar should be before /foo/
    // Similarly /foo/b is between the two.
    if (StringBeginsWith(aScope, current)) {
      aList.InsertElementAt(i, aScope);
      return;
    }
  }

  aList.AppendElement(aScope);
}

/* static */ nsCString
ServiceWorkerManager::FindScopeForPath(nsTArray<nsCString>& aList, const nsACString& aPath)
{
  nsCString match;

  for (uint32_t i = 0; i < aList.Length(); ++i) {
    const nsCString& current = aList[i];
    if (StringBeginsWith(aPath, current)) {
      match = current;
      break;
    }
  }

  return match;
}

/* static */ void
ServiceWorkerManager::RemoveScope(nsTArray<nsCString>& aList, const nsACString& aScope)
{
  aList.RemoveElement(aScope);
}

already_AddRefed<ServiceWorkerManager::ServiceWorkerDomainInfo>
ServiceWorkerManager::GetDomainInfo(nsIDocument* aDoc)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aDoc);
  nsCOMPtr<nsIURI> documentURI = aDoc->GetDocumentURI();
  return GetDomainInfo(documentURI);
}

already_AddRefed<ServiceWorkerManager::ServiceWorkerDomainInfo>
ServiceWorkerManager::GetDomainInfo(nsIURI* aURI)
{
  AssertIsOnMainThread();
  MOZ_ASSERT(aURI);

  nsCString domain;
  nsresult rv = aURI->GetHost(domain);
  if (NS_FAILED(rv)) {
    return nullptr;
  }

  nsRefPtr<ServiceWorkerDomainInfo> domainInfo;
  mDomainMap.Get(domain, getter_AddRefs(domainInfo));
  return domainInfo.forget();
}

already_AddRefed<ServiceWorkerManager::ServiceWorkerDomainInfo>
ServiceWorkerManager::GetDomainInfo(const nsCString& aURL)
{
  AssertIsOnMainThread();
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri), aURL, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  return GetDomainInfo(uri);
}

void
ServiceWorkerManager::MaybeStartControlling(nsIDocument* aDoc)
{
  AssertIsOnMainThread();
  if (!Preferences::GetBool("dom.serviceWorkers.enabled")) {
    return;
  }

  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(aDoc);
  if (!domainInfo) {
    return;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> registration =
    GetServiceWorkerRegistrationInfo(aDoc);
  if (registration) {
    NS_WARNING("NSM started controlling document");
    MOZ_ASSERT(!domainInfo->mControlledDocuments.Contains(aDoc));
    registration->StartControllingADocument();
    // Use the already_AddRefed<> form of Put to avoid the addref-deref since
    // we don't need the registration pointer in this function anymore.
    domainInfo->mControlledDocuments.Put(aDoc, registration.forget());
  }
}

class ServiceWorkerActivateAfterUnloadingJob MOZ_FINAL : public ServiceWorkerJob
{
  nsRefPtr<ServiceWorkerRegistrationInfo> mRegistration;
public:
  ServiceWorkerActivateAfterUnloadingJob(ServiceWorkerJobQueue* aQueue,
                                         ServiceWorkerRegistrationInfo* aReg)
    : ServiceWorkerJob(aQueue)
    , mRegistration(aReg)
  { }

  void
  Start()
  {
    nsRefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
    nsRefPtr<ServiceWorkerManager::ServiceWorkerDomainInfo> domainInfo =
      swm->GetDomainInfo(mRegistration->mScope);

    if (mRegistration->mPendingUninstall) {
      mRegistration->Clear();
      domainInfo->RemoveRegistration(mRegistration);
      return Done(NS_OK);
    }

    mRegistration->TryToActivate();
    Done(NS_OK);
  }
};

void
ServiceWorkerManager::MaybeStopControlling(nsIDocument* aDoc)
{
  MOZ_ASSERT(aDoc);
  if (!Preferences::GetBool("dom.serviceWorkers.enabled")) {
    return;
  }

  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(aDoc);
  if (!domainInfo) {
    return;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> registration;
  domainInfo->mControlledDocuments.Remove(aDoc, getter_AddRefs(registration));
  // A document which was uncontrolled does not maintain that state itself, so
  // it will always call MaybeStopControlling() even if there isn't an
  // associated registration. So this check is required.
  if (registration) {
    registration->StopControllingADocument();
    ServiceWorkerJobQueue* queue = domainInfo->GetOrCreateJobQueue(registration->mScope);
    nsRefPtr<ServiceWorkerActivateAfterUnloadingJob> job = new ServiceWorkerActivateAfterUnloadingJob(queue, registration);
    queue->Append(job);
  }
}

NS_IMETHODIMP
ServiceWorkerManager::GetScopeForUrl(const nsAString& aUrl, nsAString& aScope)
{
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri), aUrl, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return NS_ERROR_FAILURE;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> r = GetServiceWorkerRegistrationInfo(uri);
  if (!r) {
      return NS_ERROR_FAILURE;
  }

  aScope = NS_ConvertUTF8toUTF16(r->mScope);
  return NS_OK;
}

NS_IMETHODIMP
ServiceWorkerManager::AddRegistrationEventListener(const nsAString& aScope, nsIDOMEventTarget* aListener)
{
  AssertIsOnMainThread();
  nsCString scope = NS_ConvertUTF16toUTF8(aScope);
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(scope);
  if (!domainInfo) {
    nsCOMPtr<nsIURI> scopeAsURI;
    nsresult rv = NS_NewURI(getter_AddRefs(scopeAsURI), scope, nullptr, nullptr);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    nsCString domain;
    rv = scopeAsURI->GetHost(domain);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    domainInfo = new ServiceWorkerDomainInfo;
    mDomainMap.Put(domain, domainInfo);
  }

  MOZ_ASSERT(domainInfo);

  // TODO: this is very very bad:
  ServiceWorkerRegistration* registration = static_cast<ServiceWorkerRegistration*>(aListener);
  MOZ_ASSERT(!domainInfo->mServiceWorkerRegistrations.Contains(registration));
#ifdef DEBUG
  // Ensure a registration is only listening for it's own scope.
  nsString regScope;
  registration->GetScope(regScope);
  MOZ_ASSERT(!regScope.IsEmpty());
  MOZ_ASSERT(scope.Equals(NS_ConvertUTF16toUTF8(regScope)));
#endif
  domainInfo->mServiceWorkerRegistrations.AppendElement(registration);
  return NS_OK;
}

NS_IMETHODIMP
ServiceWorkerManager::RemoveRegistrationEventListener(const nsAString& aScope, nsIDOMEventTarget* aListener)
{
  AssertIsOnMainThread();
  nsCString scope = NS_ConvertUTF16toUTF8(aScope);
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(scope);
  if (!domainInfo) {
    return NS_OK;
  }

  ServiceWorkerRegistration* registration = static_cast<ServiceWorkerRegistration*>(aListener);
  MOZ_ASSERT(domainInfo->mServiceWorkerRegistrations.Contains(registration));
#ifdef DEBUG
  // Ensure a registration is unregistering for it's own scope.
  nsString regScope;
  registration->GetScope(regScope);
  MOZ_ASSERT(!regScope.IsEmpty());
  MOZ_ASSERT(scope.Equals(NS_ConvertUTF16toUTF8(regScope)));
#endif
  domainInfo->mServiceWorkerRegistrations.RemoveElement(registration);
  return NS_OK;
}

void
ServiceWorkerManager::FireEventOnServiceWorkerRegistrations(
  ServiceWorkerRegistrationInfo* aRegistration,
  const nsAString& aName)
{
  AssertIsOnMainThread();
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo =
    GetDomainInfo(aRegistration->mScope);

  if (domainInfo) {
    nsTObserverArray<ServiceWorkerRegistration*>::ForwardIterator it(domainInfo->mServiceWorkerRegistrations);
    while (it.HasMore()) {
      nsRefPtr<ServiceWorkerRegistration> target = it.GetNext();
      nsString regScope;
      target->GetScope(regScope);
      MOZ_ASSERT(!regScope.IsEmpty());

      nsCString utf8Scope = NS_ConvertUTF16toUTF8(regScope);
      if (utf8Scope.Equals(aRegistration->mScope)) {
        nsresult rv = target->DispatchTrustedEvent(aName);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          // Just for the warning.
        }
      }
    }
  }
}

/*
 * This is used for installing, waiting and active.
 */
NS_IMETHODIMP
ServiceWorkerManager::GetServiceWorkerForScope(nsIDOMWindow* aWindow,
                                               const nsAString& aScope,
                                               WhichServiceWorker aWhichWorker,
                                               nsISupports** aServiceWorker)
{
  AssertIsOnMainThread();

  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  if (!window) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();
  MOZ_ASSERT(doc);

  ///////////////////////////////////////////
  // Security check
  nsCString scope = NS_ConvertUTF16toUTF8(aScope);
  nsCOMPtr<nsIURI> scopeURI;
  // We pass nullptr as the base URI since scopes obtained from
  // ServiceWorkerRegistrations MUST be fully qualified URIs.
  nsresult rv = NS_NewURI(getter_AddRefs(scopeURI), scope, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  nsCOMPtr<nsIPrincipal> documentPrincipal = doc->NodePrincipal();
  rv = documentPrincipal->CheckMayLoad(scopeURI, true /* report */,
                                       false /* allowIfInheritsPrinciple */);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }
  ////////////////////////////////////////////

  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(scope);
  if (!domainInfo) {
    return NS_ERROR_FAILURE;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> registration =
    domainInfo->GetRegistration(scope);
  if (!registration) {
    return NS_ERROR_FAILURE;
  }

  nsRefPtr<ServiceWorkerInfo> info;
  if (aWhichWorker == WhichServiceWorker::INSTALLING_WORKER) {
    info = registration->mInstallingWorker;
  } else if (aWhichWorker == WhichServiceWorker::WAITING_WORKER) {
    info = registration->mWaitingWorker;
  } else if (aWhichWorker == WhichServiceWorker::ACTIVE_WORKER) {
    info = registration->mActiveWorker;
  } else {
    MOZ_CRASH("Invalid worker type");
  }

  if (!info) {
    return NS_ERROR_DOM_NOT_FOUND_ERR;
  }

  nsRefPtr<ServiceWorker> serviceWorker;
  rv = CreateServiceWorkerForWindow(window,
                                    info->GetScriptSpec(),
                                    registration->mScope,
                                    getter_AddRefs(serviceWorker));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  serviceWorker.forget(aServiceWorker);
  return NS_OK;
}

class FetchEventRunnable : public WorkerRunnable {
  nsMainThreadPtrHandle<nsIInterceptedChannel> mInterceptedChannel;
  uint64_t mWindowId;
  nsCString mSpec;
  nsCString mMethod;
  bool mIsReload;
 public:
  explicit FetchEventRunnable(WorkerPrivate* aWorkerPrivate,
                              nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel,
                              uint64_t aWindowId)
  : WorkerRunnable(aWorkerPrivate, WorkerThreadModifyBusyCount)
  , mInterceptedChannel(aChannel)
  , mWindowId(aWindowId)
  {
    MOZ_ASSERT(aWorkerPrivate);
  }

  nsresult
  Init()
  {
    nsCOMPtr<nsIChannel> channel;
    nsresult rv = mInterceptedChannel->GetChannel(getter_AddRefs(channel));
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIURI> uri;
    rv = channel->GetURI(getter_AddRefs(uri));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = uri->GetSpec(mSpec);
    NS_ENSURE_SUCCESS(rv, rv);

    nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(channel);
    rv = httpChannel->GetRequestMethod(mMethod);
    NS_ENSURE_SUCCESS(rv, rv);

    uint32_t loadFlags;
    rv = channel->GetLoadFlags(&loadFlags);
    NS_ENSURE_SUCCESS(rv, rv);

    //XXXjdm not sure this is a good heuristic - we should probably include reload-ness
    //       in the loadinfo or as a separate load flag
    //mIsReload = loadFlags & (HttpBaseChannel::LOAD_BYPASS_CACHE | HttpBaseChannel::LOAD_FRESH_CONNECTION);
    mIsReload = false;

    return NS_OK;
  }

  bool
  WorkerRun(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate);
    return DispatchFetchEvent(aCx, aWorkerPrivate);
  }

private:
  class ResumeRequest : public nsRunnable {
    nsMainThreadPtrHandle<nsIInterceptedChannel> mChannel;
  public:
    explicit ResumeRequest(nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel)
      : mChannel(aChannel)
    {
    }

    NS_IMETHOD
    Run()
    {
      AssertIsOnMainThread();
      nsresult rv = mChannel->ResetInterception();
      NS_WARN_IF_FALSE(NS_SUCCEEDED(rv), "Failed to resume intercepted network request");
      return rv;
    }
  };

  bool
  DispatchFetchEvent(JSContext* aCx, WorkerPrivate* aWorkerPrivate)
  {
    MOZ_ASSERT(aWorkerPrivate->IsServiceWorker());
    GlobalObject globalObj(aCx, aWorkerPrivate->GlobalScope()->GetWrapper());

    RequestOrScalarValueString requestInfo;
    *requestInfo.SetAsScalarValueString().ToAStringPtr() = NS_ConvertUTF8toUTF16(mSpec);

    RequestInit reqInit;
    reqInit.mMethod.Construct(mMethod);

    //XXXjdm get headers from the channel
    nsRefPtr<InternalHeaders> internalHeaders = new InternalHeaders();
    nsRefPtr<Headers> headers = new Headers(globalObj.GetAsSupports(), internalHeaders);
    reqInit.mHeaders.Construct();
    reqInit.mHeaders.Value().SetAsHeaders() = headers;

    reqInit.mBody.Construct();
    reqInit.mBody.Value().SetAsScalarValueString() = EmptyString(); //XXXjdm get this from the channel

    reqInit.mMode.Construct(RequestMode::Same_origin); //XXXjdm how to find this out?
    reqInit.mCredentials.Construct(RequestCredentials::Same_origin); //XXXjdm how to find this out?

    ErrorResult rv;
    nsRefPtr<Request> request = Request::Constructor(globalObj, requestInfo, reqInit, rv);
    if (rv.Failed()) {
      return false;
    }

    FetchEventInit init;
    init.mRequest.Construct();
    init.mRequest.Value() = request;
    init.mBubbles = false;
    init.mCancelable = true;
    init.mContext.Construct(dom::Context::Connect); //XXXjdm get this from channel loadinfo
    init.mIsReload.Construct(mIsReload);
    nsRefPtr<FetchEvent> event =
        FetchEvent::Constructor(globalObj, NS_LITERAL_STRING("fetch"), init, rv);
    if (rv.Failed()) {
      return false;
    }

    event->PostInit(mInterceptedChannel, mWindowId);
    event->SetTrusted(true);

    nsRefPtr<EventTarget> target = do_QueryObject(aWorkerPrivate->GlobalScope());
    nsresult rv2 = target->DispatchDOMEvent(nullptr, event, nullptr, nullptr);
    if (NS_FAILED(rv2) || !event->WaitToRespond()) {
      nsCOMPtr<nsIRunnable> runnable = new ResumeRequest(mInterceptedChannel);
      NS_DispatchToMainThread(runnable);
    }
    return true;
  }
};

NS_IMETHODIMP
ServiceWorkerManager::DispatchFetchEvent(nsIDocument* aDoc, nsIInterceptedChannel* aChannel)
{
  nsCOMPtr<nsISupports> serviceWorker;
  nsresult rv = GetDocumentController(aDoc->GetWindow(), getter_AddRefs(serviceWorker));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  uint64_t windowId = aDoc->GetInnerWindow()->WindowID();

  nsRefPtr<ServiceWorker> sw = static_cast<ServiceWorker*>(serviceWorker.get());
  nsMainThreadPtrHandle<nsIInterceptedChannel> handle(
      new nsMainThreadPtrHolder<nsIInterceptedChannel>(aChannel, false));

  nsRefPtr<FetchEventRunnable> event =
      new FetchEventRunnable(sw->GetWorkerPrivate(), handle, windowId);
  rv = event->Init();
  NS_ENSURE_SUCCESS(rv, rv);

  AutoSafeJSContext cx;
  if (!event->Dispatch(cx)) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

NS_IMETHODIMP
ServiceWorkerManager::IsControlled(nsIDocument* aDoc, bool* aIsControlled)
{
  nsRefPtr<ServiceWorkerRegistrationInfo> registration;
  GetDocumentRegistration(aDoc, getter_AddRefs(registration));
  *aIsControlled = !!registration;
  return NS_OK;
}

nsresult
ServiceWorkerManager::GetDocumentRegistration(nsIDocument* aDoc,
                                              ServiceWorkerRegistrationInfo** aRegistrationInfo)
{
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(aDoc);
  if (!domainInfo) {
    return NS_ERROR_FAILURE;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> registration;
  if (!domainInfo->mControlledDocuments.Get(aDoc, getter_AddRefs(registration))) {
    return NS_ERROR_FAILURE;
  }

  registration.forget(aRegistrationInfo);
  return NS_OK;
}

/*
 * The .controller is for the registration associated with the document when
 * the document was loaded.
 */
NS_IMETHODIMP
ServiceWorkerManager::GetDocumentController(nsIDOMWindow* aWindow, nsISupports** aServiceWorker)
{
  nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aWindow);
  MOZ_ASSERT(window);
  if (!window || !window->GetExtantDoc()) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIDocument> doc = window->GetExtantDoc();

  nsRefPtr<ServiceWorkerRegistrationInfo> registration;
  nsresult rv = GetDocumentRegistration(doc, getter_AddRefs(registration));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (!registration->mActiveWorker) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsRefPtr<ServiceWorker> serviceWorker;
  rv = CreateServiceWorkerForWindow(window,
                                    registration->mActiveWorker->GetScriptSpec(),
                                    registration->mScope,
                                    getter_AddRefs(serviceWorker));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  serviceWorker.forget(aServiceWorker);
  return NS_OK;
}

NS_IMETHODIMP
ServiceWorkerManager::GetInstalling(nsIDOMWindow* aWindow,
                                    const nsAString& aScope,
                                    nsISupports** aServiceWorker)
{
  return GetServiceWorkerForScope(aWindow, aScope,
                                  WhichServiceWorker::INSTALLING_WORKER,
                                  aServiceWorker);
}

NS_IMETHODIMP
ServiceWorkerManager::GetWaiting(nsIDOMWindow* aWindow,
                                 const nsAString& aScope,
                                 nsISupports** aServiceWorker)
{
  return GetServiceWorkerForScope(aWindow, aScope,
                                  WhichServiceWorker::WAITING_WORKER,
                                  aServiceWorker);
}

NS_IMETHODIMP
ServiceWorkerManager::GetActive(nsIDOMWindow* aWindow,
                                const nsAString& aScope,
                                nsISupports** aServiceWorker)
{
  return GetServiceWorkerForScope(aWindow, aScope,
                                  WhichServiceWorker::ACTIVE_WORKER,
                                  aServiceWorker);
}

NS_IMETHODIMP
ServiceWorkerManager::CreateServiceWorker(const nsACString& aScriptSpec,
                                          const nsACString& aScope,
                                          ServiceWorker** aServiceWorker)
{
  AssertIsOnMainThread();

  WorkerPrivate::LoadInfo info;
  nsresult rv = NS_NewURI(getter_AddRefs(info.mBaseURI), aScriptSpec, nullptr, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  info.mResolvedScriptURI = info.mBaseURI;

  rv = info.mBaseURI->GetHost(info.mDomain);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // FIXME(nsm): Create correct principal based on app-ness.
  // Would it make sense to store the nsIPrincipal of the first register() in
  // the ServiceWorkerRegistrationInfo and use that?
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  rv = ssm->GetNoAppCodebasePrincipal(info.mBaseURI, getter_AddRefs(info.mPrincipal));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  AutoSafeJSContext cx;

  nsRefPtr<ServiceWorker> serviceWorker;
  RuntimeService* rs = RuntimeService::GetService();
  if (!rs) {
    return NS_ERROR_FAILURE;
  }

  rv = rs->CreateServiceWorkerFromLoadInfo(cx, info, NS_ConvertUTF8toUTF16(aScriptSpec), aScope,
                                           getter_AddRefs(serviceWorker));

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  serviceWorker.forget(aServiceWorker);
  return NS_OK;
}

void
ServiceWorkerManager::InvalidateServiceWorkerRegistrationWorker(ServiceWorkerRegistrationInfo* aRegistration,
                                                                WhichServiceWorker aWhichOnes)
{
  AssertIsOnMainThread();
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo =
    GetDomainInfo(aRegistration->mScriptSpec);

  if (domainInfo) {
    nsTObserverArray<ServiceWorkerRegistration*>::ForwardIterator it(domainInfo->mServiceWorkerRegistrations);
    while (it.HasMore()) {
      nsRefPtr<ServiceWorkerRegistration> target = it.GetNext();
      nsString regScope;
      target->GetScope(regScope);
      MOZ_ASSERT(!regScope.IsEmpty());

      nsCString utf8Scope = NS_ConvertUTF16toUTF8(regScope);

      if (utf8Scope.Equals(aRegistration->mScope)) {
        target->InvalidateWorkerReference(aWhichOnes);
      }
    }
  }
}

NS_IMETHODIMP
ServiceWorkerManager::Update(const nsAString& aScope)
{
  NS_ConvertUTF16toUTF8 scope(aScope);

  nsRefPtr<ServiceWorkerManager::ServiceWorkerDomainInfo> domainInfo =
    GetDomainInfo(scope);
  if (NS_WARN_IF(!domainInfo)) {
    return NS_OK;
  }

  nsRefPtr<ServiceWorkerRegistrationInfo> registration;
  domainInfo->mServiceWorkerRegistrationInfos.Get(scope,
                                                  getter_AddRefs(registration));
  if (NS_WARN_IF(!registration)) {
    return NS_OK;
  }

  if (registration->mPendingUninstall) {
    return NS_OK;
  }

  if (registration->mInstallingWorker) {
    return NS_OK;
  }

  // FIXME(nsm): Pass a relevant global.
  Update(nullptr, registration);
  return NS_OK;
}

namespace {

class MOZ_STACK_CLASS FilterRegistrationData
{
public:
  FilterRegistrationData(nsTArray<uint64_t>* aDocuments,
                     ServiceWorkerRegistrationInfo* aRegistration)
  : mDocuments(aDocuments),
    mRegistration(aRegistration)
  {
  }

  nsTArray<uint64_t>* mDocuments;
  nsRefPtr<ServiceWorkerRegistrationInfo> mRegistration;
};

static PLDHashOperator
EnumControlledDocuments(nsISupports* aKey,
                        ServiceWorkerRegistrationInfo* aRegistration,
                        void* aData)
{
  FilterRegistrationData* data = static_cast<FilterRegistrationData*>(aData);
  if (data->mRegistration != aRegistration) {
    return PL_DHASH_NEXT;
  }
  nsCOMPtr<nsIDocument> document = do_QueryInterface(aKey);
  if (!document || !document->GetInnerWindow()) {
      return PL_DHASH_NEXT;
  }

  data->mDocuments->AppendElement(document->GetInnerWindow()->WindowID());
  return PL_DHASH_NEXT;
}

} // anonymous namespace

void
ServiceWorkerManager::GetServicedClients(const nsCString& aScope,
                                     nsTArray<uint64_t>* aControlledDocuments)
{
  nsRefPtr<ServiceWorkerDomainInfo> domainInfo = GetDomainInfo(aScope);
  nsRefPtr<ServiceWorkerRegistrationInfo> registration =
    domainInfo->GetRegistration(aScope);
  MOZ_ASSERT(registration);
  FilterRegistrationData data(aControlledDocuments, registration);

  domainInfo->mControlledDocuments.EnumerateRead(EnumControlledDocuments,
                                                 &data);
}

void
ServiceWorkerManager::FireControllerChange(ServiceWorkerRegistrationInfo* aRegistration)
{
  // FIXME(nsm): Fill this out.
}
END_WORKERS_NAMESPACE
