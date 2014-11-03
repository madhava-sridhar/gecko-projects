/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/Cache.h"

#include "mozilla/dom/Headers.h"
#include "mozilla/dom/InternalResponse.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/CacheBinding.h"
#include "mozilla/dom/cache/CacheChild.h"
#include "mozilla/dom/cache/ReadStream.h"
#include "mozilla/dom/cache/TypeUtils.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Preferences.h"
#include "mozilla/unused.h"
#include "nsIGlobalObject.h"
#include "nsNetUtil.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::ErrorResult;
using mozilla::unused;

NS_IMPL_CYCLE_COLLECTING_ADDREF(mozilla::dom::cache::Cache);
NS_IMPL_CYCLE_COLLECTING_RELEASE(mozilla::dom::cache::Cache);
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(Cache, mOwner, mGlobal)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Cache)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

Cache::Cache(nsISupports* aOwner, nsIGlobalObject* aGlobal,
             const nsACString& aOrigin, const nsACString& aBaseDomain,
             PCacheChild* aActor)
  : mOwner(aOwner)
  , mGlobal(aGlobal)
  , mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mActor(static_cast<CacheChild*>(aActor))
{
  MOZ_ASSERT(mActor);
  mActor->SetListener(*this);
}

// TODO: factor this out to TypeUtils
void
Cache::ToPCacheRequest(PCacheRequest& aOut, const RequestOrScalarValueString& aIn,
                       bool aReadBody, ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  jsapi.Init(mGlobal);
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsGlobal(cx, mGlobal->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);

  TypeUtils::ToPCacheRequest(global, aOut, aIn, aReadBody, aRv);
}

// TODO: factor this out to TypeUtils
void
Cache::ToPCacheRequest(PCacheRequest& aOut, const OwningRequestOrScalarValueString& aIn,
                       bool aReadBody, ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  jsapi.Init(mGlobal);
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsGlobal(cx, mGlobal->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);

  return TypeUtils::ToPCacheRequest(global, aOut, aIn, aReadBody, aRv);
}

// TODO: factor this out to TypeUtils
void
Cache::ToPCacheRequestOrVoid(PCacheRequestOrVoid& aOut,
                             const Optional<RequestOrScalarValueString>& aIn,
                             bool aReadBody, ErrorResult& aRv)
{
  AutoJSAPI jsapi;
  jsapi.Init(mGlobal);
  JSContext* cx = jsapi.cx();
  JS::Rooted<JSObject*> jsGlobal(cx, mGlobal->GetGlobalJSObject());
  JSAutoCompartment ac(cx, jsGlobal);

  GlobalObject global(cx, jsGlobal);

  return TypeUtils::ToPCacheRequestOrVoid(global, aOut, aIn, aReadBody, aRv);
}

already_AddRefed<Promise>
Cache::Match(const RequestOrScalarValueString& aRequest,
             const QueryParams& aParams, ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  PCacheRequest request;
  ToPCacheRequest(request, aRequest, false, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  PCacheQueryParams params;
  TypeUtils::ToPCacheQueryParams(params, aParams);

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendMatch(requestId, request, params);

  return promise.forget();
}

already_AddRefed<Promise>
Cache::MatchAll(const Optional<RequestOrScalarValueString>& aRequest,
                const QueryParams& aParams, ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  PCacheRequestOrVoid request;
  ToPCacheRequestOrVoid(request, aRequest, false, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  PCacheQueryParams params;
  TypeUtils::ToPCacheQueryParams(params, aParams);

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendMatchAll(requestId, request, params);

  return promise.forget();
}

already_AddRefed<Promise>
Cache::Add(const RequestOrScalarValueString& aRequest, ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  if (aRequest.IsRequest()) {
    nsAutoCString method;
    aRequest.GetAsRequest().GetMethod(method);
    if (!method.LowerCaseEqualsLiteral("get")) {
      NS_ConvertUTF8toUTF16 label(method);
      aRv.ThrowTypeError(MSG_INVALID_REQUEST_METHOD, &label);
      return nullptr;
    }
  }

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  PCacheRequest request;
  ToPCacheRequest(request, aRequest, true, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendAdd(requestId, request);

  return promise.forget();
}

already_AddRefed<Promise>
Cache::AddAll(const Sequence<OwningRequestOrScalarValueString>& aRequests,
              ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  nsTArray<PCacheRequest> requests;
  for(uint32_t i = 0; i < aRequests.Length(); ++i) {
    if (aRequests[i].IsRequest()) {
      nsAutoCString method;
      aRequests[i].GetAsRequest().get()->GetMethod(method);
      if (!method.LowerCaseEqualsLiteral("get")) {
        NS_ConvertUTF8toUTF16 label(method);
        aRv.ThrowTypeError(MSG_INVALID_REQUEST_METHOD, &label);
        return nullptr;
      }
    }

    PCacheRequest* request = requests.AppendElement();
    ToPCacheRequest(*request, aRequests[i], true, aRv);
    if (aRv.Failed()) {
      return nullptr;
    }
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendAddAll(requestId, requests);

  return promise.forget();
}

already_AddRefed<Promise>
Cache::Put(const RequestOrScalarValueString& aRequest, Response& aResponse,
           ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  if (aRequest.IsRequest()) {
    nsAutoCString method;
    aRequest.GetAsRequest().GetMethod(method);
    if (!method.LowerCaseEqualsLiteral("get")) {
      NS_ConvertUTF8toUTF16 label(method);
      aRv.ThrowTypeError(MSG_INVALID_REQUEST_METHOD, &label);
      return nullptr;
    }
  }

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  PCacheRequest request;
  ToPCacheRequest(request, aRequest, true, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  PCacheResponse response;
  TypeUtils::ToPCacheResponse(response, aResponse, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendPut(requestId, request, response);

  return promise.forget();
}

already_AddRefed<Promise>
Cache::Delete(const RequestOrScalarValueString& aRequest,
              const QueryParams& aParams, ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  PCacheRequest request;
  ToPCacheRequest(request, aRequest, false, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  PCacheQueryParams params;
  TypeUtils::ToPCacheQueryParams(params, aParams);

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendDelete(requestId, request, params);

  return promise.forget();
}

already_AddRefed<Promise>
Cache::Keys(const Optional<RequestOrScalarValueString>& aRequest,
            const QueryParams& aParams, ErrorResult& aRv)
{
  MOZ_ASSERT(mActor);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  PCacheRequestOrVoid request;
  ToPCacheRequestOrVoid(request, aRequest, false, aRv);
  if (aRv.Failed()) {
    return nullptr;
  }

  PCacheQueryParams params;
  TypeUtils::ToPCacheQueryParams(params, aParams);

  RequestId requestId = AddRequestPromise(promise, aRv);

  unused << mActor->SendKeys(requestId, request, params);

  return promise.forget();
}

// static
bool
Cache::PrefEnabled(JSContext* aCx, JSObject* aObj)
{
  using mozilla::dom::workers::WorkerPrivate;
  using mozilla::dom::workers::GetWorkerPrivateFromContext;

  // In the long term we want to support Cache on main-thread, so
  // allow it to be exposed there via a pref.
  if (NS_IsMainThread()) {
    bool enabled;
    nsresult rv = Preferences::GetBool("dom.window-caches.enabled", &enabled);
    if (NS_FAILED(rv)) {
      return false;
    }
    return enabled;
  }

  WorkerPrivate* workerPrivate = GetWorkerPrivateFromContext(aCx);
  if (!workerPrivate) {
    return false;
  }

  // Otherwise expose on ServiceWorkers.  Also expose on others workers if
  // pref enabled.
  return workerPrivate->IsServiceWorker() || workerPrivate->DOMCachesEnabled();
}

nsISupports*
Cache::GetParentObject() const
{
  return mOwner;
}

JSObject*
Cache::WrapObject(JSContext* aContext)
{
  return CacheBinding::Wrap(aContext, this);
}

void
Cache::ActorDestroy(mozilla::ipc::IProtocol& aActor)
{
  MOZ_ASSERT(mActor);
  MOZ_ASSERT(mActor == &aActor);
  mActor->ClearListener();
  mActor = nullptr;
}

void
Cache::RecvMatchResponse(RequestId aRequestId, nsresult aRv,
                         const PCacheResponseOrVoid& aResponse,
                         PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  if (aResponse.type() == PCacheResponseOrVoid::Tvoid_t) {
    promise->MaybeReject(NS_ERROR_DOM_NOT_FOUND_ERR);
    return;
  }

  nsRefPtr<Response> response = TypeUtils::ToResponse(mGlobal, aResponse,
                                                      aStreamControl);
  promise->MaybeResolve(response);
}

void
Cache::RecvMatchAllResponse(RequestId aRequestId, nsresult aRv,
                            const nsTArray<PCacheResponse>& aResponses,
                            PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  nsTArray<nsRefPtr<Response>> responses;
  for (uint32_t i = 0; i < aResponses.Length(); ++i) {
    nsRefPtr<Response> response = TypeUtils::ToResponse(mGlobal, aResponses[i],
                                                        aStreamControl);
    responses.AppendElement(response.forget());
  }
  promise->MaybeResolve(responses);
}

void
Cache::RecvAddResponse(RequestId aRequestId, nsresult aRv)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  promise->MaybeResolve(JS::UndefinedHandleValue);
}

void
Cache::RecvAddAllResponse(RequestId aRequestId, nsresult aRv)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  promise->MaybeResolve(JS::UndefinedHandleValue);
}

void
Cache::RecvPutResponse(RequestId aRequestId, nsresult aRv)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  promise->MaybeResolve(JS::UndefinedHandleValue);
}

void
Cache::RecvDeleteResponse(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  promise->MaybeResolve(aSuccess);
}

void
Cache::RecvKeysResponse(RequestId aRequestId, nsresult aRv,
                        const nsTArray<PCacheRequest>& aRequests,
                        PCacheStreamControlChild* aStreamControl)
{
  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  nsTArray<nsRefPtr<Request>> requests;
  for (uint32_t i = 0; i < aRequests.Length(); ++i) {
    // TODO: Should mOwner and mGlobal be just one field? Right now mOwner can
    //       be null (when on a worker), but mGlobal is always provided.
    nsRefPtr<Request> request = TypeUtils::ToRequest(mGlobal, aRequests[i],
                                                     aStreamControl);
    requests.AppendElement(request.forget());
  }
  promise->MaybeResolve(requests);
}


Cache::~Cache()
{
  if (mActor) {
    mActor->ClearListener();
    PCacheChild::Send__delete__(mActor);
    // The actor will be deleted by the IPC manager
    mActor = nullptr;
  }
}

RequestId
Cache::AddRequestPromise(Promise* aPromise, ErrorResult& aRv)
{
  MOZ_ASSERT(aPromise);

  nsRefPtr<Promise>* ref = mRequestPromises.AppendElement();
  *ref = aPromise;

  // (Ab)use the promise pointer as our request ID.  This is a fast, thread-safe
  // way to get a unique ID for the promise to be resolved later.
  return reinterpret_cast<RequestId>(aPromise);
}

already_AddRefed<Promise>
Cache::RemoveRequestPromise(RequestId aRequestId)
{
  MOZ_ASSERT(aRequestId != INVALID_REQUEST_ID);

  for (uint32_t i = 0; i < mRequestPromises.Length(); ++i) {
    nsRefPtr<Promise>& promise = mRequestPromises.ElementAt(i);
    // To be safe, only cast promise pointers to our integer RequestId
    // type and never cast an integer to a pointer.
    if (aRequestId == reinterpret_cast<RequestId>(promise.get())) {
      nsRefPtr<Promise> ref;
      ref.swap(promise);
      mRequestPromises.RemoveElementAt(i);
      return ref.forget();
    }
  }
  return nullptr;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
