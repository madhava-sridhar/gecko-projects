/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerEvents.h"
#include "ServiceWorkerClient.h"

#include "nsINetworkInterceptController.h"
#include "nsIOutputStream.h"
#include "nsContentUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsStreamUtils.h"
#include "nsNetCID.h"

#include "mozilla/dom/Promise.h"
#include "mozilla/dom/PromiseNativeHandler.h"
#include "mozilla/dom/Request.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/WorkerScope.h"
#include "mozilla/dom/workers/bindings/ServiceWorker.h"
#include "mozilla/dom/ServiceWorkerGlobalScopeBinding.h"

using namespace mozilla::dom;

BEGIN_WORKERS_NAMESPACE

bool
ServiceWorkerEventsVisible(JSContext* aCx, JSObject* aObj)
{
  ServiceWorkerGlobalScope* scope = nullptr;
  nsresult rv = UnwrapObject<prototypes::id::ServiceWorkerGlobalScope_workers,
                             mozilla::dom::ServiceWorkerGlobalScopeBinding_workers::NativeType>(aObj, scope);
  return NS_SUCCEEDED(rv) && scope;
}

FetchEvent::FetchEvent(EventTarget* aOwner)
  : Event(aOwner, nullptr, nullptr)
  , mWindowId(0)
  , mIsReload(false)
  , mWaitToRespond(false)
  , mRespondWithEntered(false)
  , mRespondWithError(false)
{
}

FetchEvent::~FetchEvent()
{
}

void
FetchEvent::PostInit(nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel, uint64_t aWindowId)
{
  mChannel = aChannel;
  mWindowId = aWindowId;
}

/*static*/ already_AddRefed<FetchEvent>
FetchEvent::Constructor(const GlobalObject& aGlobal,
                        const nsAString& aType,
                        const FetchEventInit& aOptions,
                        ErrorResult& aRv)
{
  nsRefPtr<EventTarget> owner = do_QueryObject(aGlobal.GetAsSupports());
  nsRefPtr<FetchEvent> e = new FetchEvent(owner);
  bool trusted = e->Init(owner);
  e->InitEvent(aType, aOptions.mBubbles, aOptions.mCancelable);
  e->SetTrusted(trusted);
  e->mRequest = aOptions.mRequest.WasPassed() ?
      &aOptions.mRequest.Value() : nullptr;;
  e->mContext = aOptions.mContext.WasPassed() ?
      aOptions.mContext.Value() : Context::Connect;
  e->mIsReload = aOptions.mIsReload.WasPassed() ?
      aOptions.mIsReload.Value() : false;
  e->mClient = aOptions.mClient.WasPassed() ?
      &aOptions.mClient.Value() : nullptr;
  return e.forget();
}

namespace {

class FinishResponse : public nsRunnable {
  nsMainThreadPtrHandle<nsIInterceptedChannel> mChannel;
 public:
  explicit FinishResponse(nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel)
  : mChannel(aChannel)
  {
  }

  NS_IMETHOD
  Run()
  {
    AssertIsOnMainThread();
    nsresult rv = mChannel->FinishSynthesizedResponse();
    NS_WARN_IF_FALSE(NS_SUCCEEDED(rv), "Failed to finish synthesized response");
    return rv;
  }
};

class RespondWithHandler MOZ_FINAL : public PromiseNativeHandler {
  nsMainThreadPtrHandle<nsIInterceptedChannel> mInterceptedChannel;
public:
  RespondWithHandler(nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel)
  : mInterceptedChannel(aChannel)
  {
  }

  void
  ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) MOZ_OVERRIDE;

  void
  RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue) MOZ_OVERRIDE;
};

struct RespondWithClosure {
  nsMainThreadPtrHandle<nsIInterceptedChannel> mInterceptedChannel;

  RespondWithClosure(nsMainThreadPtrHandle<nsIInterceptedChannel>& aChannel)
  : mInterceptedChannel(aChannel)
  {
  }
};

void RespondWithCopyComplete(void* closure, nsresult status)
{
  nsAutoPtr<RespondWithClosure> data(static_cast<RespondWithClosure*>(closure));
  nsRefPtr<FinishResponse> event = new FinishResponse(data->mInterceptedChannel);
  NS_DispatchToMainThread(event);
}

void
RespondWithHandler::ResolvedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue)
{
  if (!aValue.isObject()) {
    //https://github.com/slightlyoff/ServiceWorker/issues/454
    return;
  }

  nsRefPtr<Response> response;
  nsresult rv = UNWRAP_OBJECT(Response, &aValue.toObject(), response);
  if (NS_FAILED(rv)) {
    //https://github.com/slightlyoff/ServiceWorker/issues/454
    return;
  }

  nsCOMPtr<nsIInputStream> body;
  response->GetBody(getter_AddRefs(body));

  nsCOMPtr<nsIOutputStream> responseBody;
  rv = mInterceptedChannel->GetResponseBody(getter_AddRefs(responseBody));
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  nsAutoPtr<RespondWithClosure> closure(new RespondWithClosure(mInterceptedChannel));

  nsCOMPtr<nsIEventTarget> stsThread = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID, &rv);
  rv = NS_AsyncCopy(body, responseBody, stsThread, NS_ASYNCCOPY_VIA_READSEGMENTS, 4096,
                    RespondWithCopyComplete, closure.forget());
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

void
RespondWithHandler::RejectedCallback(JSContext* aCx, JS::Handle<JS::Value> aValue)
{
  //https://github.com/slightlyoff/ServiceWorker/issues/454
}

} // anonymous namespace

void
FetchEvent::RespondWith(Promise& aPromise, ErrorResult& aRv)
{
  if (mRespondWithEntered) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  mWaitToRespond = true;
  mRespondWithEntered = true;
  nsRefPtr<RespondWithHandler> handler = new RespondWithHandler(mChannel);
  aPromise.AppendNativeHandler(handler);
}

already_AddRefed<ServiceWorkerClient>
FetchEvent::Client()
{
  if (!mClient) {
    mClient = new ServiceWorkerClient(GetParentObject(), mWindowId);
  }
  nsRefPtr<ServiceWorkerClient> client = mClient;
  return client.forget();
}

already_AddRefed<Promise>
FetchEvent::ForwardTo(const nsAString& aUrl)
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

already_AddRefed<Promise>
FetchEvent::Default()
{
  nsCOMPtr<nsIGlobalObject> global = do_QueryInterface(GetParentObject());
  MOZ_ASSERT(global);
  ErrorResult result;
  nsRefPtr<Promise> promise = Promise::Create(global, result);
  if (result.Failed()) {
    return nullptr;
  }

  promise->MaybeReject(NS_ERROR_NOT_AVAILABLE);
  return promise.forget();
}

NS_IMPL_ADDREF_INHERITED(FetchEvent, Event)
NS_IMPL_RELEASE_INHERITED(FetchEvent, Event)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(FetchEvent)
NS_INTERFACE_MAP_END_INHERITING(Event)

NS_IMPL_CYCLE_COLLECTION_INHERITED(FetchEvent, Event, mRequest, mClient)

InstallPhaseEvent::InstallPhaseEvent(EventTarget* aOwner)
  : Event(aOwner, nullptr, nullptr)
{
}

void
InstallPhaseEvent::WaitUntil(Promise& aPromise)
{
  MOZ_ASSERT(!NS_IsMainThread());

  // Only first caller counts.
  if (EventPhase() == AT_TARGET && !mPromise) {
    mPromise = &aPromise;
  }
}

NS_IMPL_ADDREF_INHERITED(InstallPhaseEvent, Event)
NS_IMPL_RELEASE_INHERITED(InstallPhaseEvent, Event)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(InstallPhaseEvent)
NS_INTERFACE_MAP_END_INHERITING(Event)

NS_IMPL_CYCLE_COLLECTION_INHERITED(InstallPhaseEvent, Event, mPromise)

InstallEvent::InstallEvent(EventTarget* aOwner)
  : InstallPhaseEvent(aOwner)
{
}

NS_IMPL_ADDREF_INHERITED(InstallEvent, InstallPhaseEvent)
NS_IMPL_RELEASE_INHERITED(InstallEvent, InstallPhaseEvent)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(InstallEvent)
NS_INTERFACE_MAP_END_INHERITING(InstallPhaseEvent)

NS_IMPL_CYCLE_COLLECTION_INHERITED(InstallEvent, InstallPhaseEvent, mActiveWorker)

END_WORKERS_NAMESPACE
