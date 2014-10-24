/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStorage.h"

#include "mozilla/unused.h"
#include "mozilla/dom/CacheStorageBinding.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/Response.h"
#include "mozilla/dom/cache/Cache.h"
#include "mozilla/dom/cache/CacheStorageChild.h"
#include "mozilla/dom/cache/PCacheChild.h"
#include "mozilla/dom/cache/ReadStream.h"
#include "mozilla/dom/cache/TypeUtils.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "nsIGlobalObject.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::ErrorResult;
using mozilla::ipc::BackgroundChild;
using mozilla::ipc::PBackgroundChild;
using mozilla::ipc::IProtocol;

NS_IMPL_CYCLE_COLLECTING_ADDREF(mozilla::dom::cache::CacheStorage);
NS_IMPL_CYCLE_COLLECTING_RELEASE(mozilla::dom::cache::CacheStorage);
NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(CacheStorage, mOwner,
                                                    mGlobal,
                                                    mRequestPromises)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(CacheStorage)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
  NS_INTERFACE_MAP_ENTRY(nsIIPCBackgroundChildCreateCallback)
NS_INTERFACE_MAP_END

CacheStorage::CacheStorage(Namespace aNamespace,
                           nsISupports* aOwner,
                           nsIGlobalObject* aGlobal,
                           const nsACString& aOrigin,
                           const nsACString& aBaseDomain)
  : mNamespace(aNamespace)
  , mOwner(aOwner)
  , mGlobal(aGlobal)
  , mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mActor(nullptr)
  , mFailedActor(false)
{
  MOZ_ASSERT(mGlobal);

  if (mOrigin.EqualsLiteral("null") || mBaseDomain.EqualsLiteral("")) {
    ActorFailed();
    return;
  }

  PBackgroundChild* actor = BackgroundChild::GetForCurrentThread();
  if (actor) {
    ActorCreated(actor);
  } else {
    bool ok = BackgroundChild::GetOrCreateForCurrentThread(this);
    if (!ok) {
      ActorFailed();
    }
  }
}

already_AddRefed<Promise>
CacheStorage::Match(const RequestOrScalarValueString& aRequest,
                    const QueryParams& aParams, ErrorResult& aRv)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  if (mFailedActor) {
    promise->MaybeReject(NS_ERROR_UNEXPECTED);
    return promise.forget();
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  if (!mActor) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mRequestId = requestId;
    entry->mOp = OP_MATCH;
    entry->mParams = aParams;

    if (aRequest.IsScalarValueString()) {
      *entry->mRequest.SetAsScalarValueString().ToAStringPtr() =
        aRequest.GetAsScalarValueString();
    } else {
      entry->mRequest.SetAsRequest() =
        &aRequest.GetAsRequest();
    }

    return promise.forget();
  }

  PCacheRequest request;
  TypeUtils::ToPCacheRequest(request, aRequest);

  PCacheQueryParams params;
  TypeUtils::ToPCacheQueryParams(params, aParams);

  unused << mActor->SendMatch(requestId, request, params);

  return promise.forget();
}

already_AddRefed<Promise>
CacheStorage::Has(const nsAString& aKey, ErrorResult& aRv)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  if (mFailedActor) {
    promise->MaybeReject(NS_ERROR_UNEXPECTED);
    return promise.forget();
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  if (!mActor) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mRequestId = requestId;
    entry->mOp = OP_HAS;
    entry->mKey = aKey;

    return promise.forget();
  }

  unused << mActor->SendHas(requestId, nsString(aKey));

  return promise.forget();
}

already_AddRefed<Promise>
CacheStorage::Open(const nsAString& aKey, ErrorResult& aRv)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  if (mFailedActor) {
    promise->MaybeReject(NS_ERROR_UNEXPECTED);
    return promise.forget();
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  if (!mActor) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mRequestId = requestId;
    entry->mOp = OP_OPEN;
    entry->mKey = aKey;

    return promise.forget();
  }

  unused << mActor->SendOpen(requestId, nsString(aKey));

  return promise.forget();
}

already_AddRefed<Promise>
CacheStorage::Delete(const nsAString& aKey, ErrorResult& aRv)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  if (mFailedActor) {
    promise->MaybeReject(NS_ERROR_UNEXPECTED);
    return promise.forget();
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  if (!mActor) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mRequestId = requestId;
    entry->mOp = OP_DELETE;
    entry->mKey = aKey;

    return promise.forget();
  }

  unused << mActor->SendDelete(requestId, nsString(aKey));

  return promise.forget();
}

already_AddRefed<Promise>
CacheStorage::Keys(ErrorResult& aRv)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = Promise::Create(mGlobal, aRv);
  if (!promise) {
    return nullptr;
  }

  if (mFailedActor) {
    promise->MaybeReject(NS_ERROR_UNEXPECTED);
    return promise.forget();
  }

  RequestId requestId = AddRequestPromise(promise, aRv);

  if (!mActor) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mRequestId = requestId;
    entry->mOp = OP_KEYS;

    return promise.forget();
  }

  unused << mActor->SendKeys(requestId);

  return promise.forget();
}

// static
bool
CacheStorage::PrefEnabled(JSContext* aCx, JSObject* aObj)
{
  return Cache::PrefEnabled(aCx, aObj);
}

nsISupports*
CacheStorage::GetParentObject() const
{
  return mOwner;
}

JSObject*
CacheStorage::WrapObject(JSContext* aContext)
{
  return mozilla::dom::CacheStorageBinding::Wrap(aContext, this);
}

void
CacheStorage::ActorCreated(PBackgroundChild* aActor)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);
  MOZ_ASSERT(aActor);

  CacheStorageChild* newActor = new CacheStorageChild(*this);
  if (NS_WARN_IF(!newActor)) {
    ActorFailed();
    return;
  }

  PCacheStorageChild* constructedActor =
    aActor->SendPCacheStorageConstructor(newActor, mNamespace, mOrigin,
                                         mBaseDomain);

  if (NS_WARN_IF(!constructedActor)) {
    ActorFailed();
    return;
  }

  MOZ_ASSERT(constructedActor == newActor);
  mActor = newActor;

  for (uint32_t i = 0; i < mPendingRequests.Length(); ++i) {
    Entry& entry = mPendingRequests[i];
    RequestId requestId = entry.mRequestId;
    switch(entry.mOp) {
      case OP_MATCH:
      {
        PCacheRequest request;
        TypeUtils::ToPCacheRequest(request, entry.mRequest);

        PCacheQueryParams params;
        TypeUtils::ToPCacheQueryParams(params, entry.mParams);

        unused << mActor->SendMatch(requestId, request, params);
        break;
      }
      case OP_HAS:
        unused << mActor->SendHas(requestId, entry.mKey);
        break;
      case OP_OPEN:
        unused << mActor->SendOpen(requestId, entry.mKey);
        break;
      case OP_DELETE:
        unused << mActor->SendDelete(requestId, entry.mKey);
        break;
      case OP_KEYS:
        unused << mActor->SendKeys(requestId);
        break;
    }
  }
  mPendingRequests.Clear();
}

void
CacheStorage::ActorFailed()
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);
  MOZ_ASSERT(!mFailedActor);

  mFailedActor = true;

  for (uint32_t i = 0; i < mPendingRequests.Length(); ++i) {
    RequestId requestId = mPendingRequests[i].mRequestId;
    nsRefPtr<Promise> promise = RemoveRequestPromise(requestId);
    if (!promise) {
      continue;
    }
    promise->MaybeReject(NS_ERROR_UNEXPECTED);
  }
  mPendingRequests.Clear();
}

void
CacheStorage::ActorDestroy(IProtocol& aActor)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);
  MOZ_ASSERT(mActor);
  MOZ_ASSERT(mActor == &aActor);
  mActor->ClearListener();
  mActor = nullptr;
}

void
CacheStorage::RecvMatchResponse(RequestId aRequestId, nsresult aRv,
                                const PCacheResponseOrVoid& aResponse,
                                PCacheStreamControlChild* aStreamControl)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

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
CacheStorage::RecvHasResponse(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

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
CacheStorage::RecvOpenResponse(RequestId aRequestId, nsresult aRv,
                               PCacheChild* aActor)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    if (aActor) {
      PCacheChild::Send__delete__(aActor);
    }
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  if (!aActor) {
    promise->MaybeReject(NS_ERROR_DOM_INVALID_ACCESS_ERR);
    return;
  }

  nsRefPtr<Cache> cache = new Cache(mOwner, mGlobal, mOrigin, mBaseDomain,
                                    aActor);
  promise->MaybeResolve(cache);
}

void
CacheStorage::RecvDeleteResponse(RequestId aRequestId, nsresult aRv,
                                 bool aSuccess)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

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
CacheStorage::RecvKeysResponse(RequestId aRequestId, nsresult aRv,
                               const nsTArray<nsString>& aKeys)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  nsRefPtr<Promise> promise = RemoveRequestPromise(aRequestId);
  if (NS_WARN_IF(!promise)) {
    return;
  }

  if (NS_FAILED(aRv)) {
    promise->MaybeReject(aRv);
    return;
  }

  promise->MaybeResolve(aKeys);
}

CacheStorage::~CacheStorage()
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);

  if (mActor) {
    mActor->ClearListener();
    PCacheStorageChild::Send__delete__(mActor);
    // The actor will be deleted by the IPC manager
    mActor = nullptr;
  }
}

RequestId
CacheStorage::AddRequestPromise(Promise* aPromise, ErrorResult& aRv)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);
  MOZ_ASSERT(aPromise);

  mRequestPromises.AppendElement(aPromise);

  // (Ab)use the promise pointer as our request ID.  This is a fast, thread-safe
  // way to get a unique ID for the promise to be resolved later.
  return reinterpret_cast<RequestId>(aPromise);
}

already_AddRefed<Promise>
CacheStorage::RemoveRequestPromise(RequestId aRequestId)
{
  NS_ASSERT_OWNINGTHREAD(CacheStorage);
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
