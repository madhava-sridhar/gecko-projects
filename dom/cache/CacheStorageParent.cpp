/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStorageParent.h"

#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/cache/CacheParent.h"
#include "mozilla/dom/cache/CacheStreamControlParent.h"
#include "mozilla/dom/cache/Manager.h"
#include "mozilla/dom/cache/ManagerId.h"
#include "mozilla/dom/cache/ReadStream.h"
#include "mozilla/dom/cache/SavedTypes.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "mozilla/ipc/PFileDescriptorSetParent.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/unused.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::ipc::PBackgroundParent;
using mozilla::ipc::PFileDescriptorSetParent;
using mozilla::ipc::PrincipalInfo;

CacheStorageParent::CacheStorageParent(PBackgroundParent* aManagingActor,
                                       Namespace aNamespace,
                                       const PrincipalInfo& aPrincipalInfo)
  : mNamespace(aNamespace)
{
  MOZ_ASSERT(aManagingActor);

  nsresult rv = PrincipalVerifier::Create(this, aManagingActor, aPrincipalInfo,
                                          getter_AddRefs(mVerifier));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    unused << Send__delete__(this);
  }
}

CacheStorageParent::~CacheStorageParent()
{
  MOZ_ASSERT(!mManager);
}

void
CacheStorageParent::ActorDestroy(ActorDestroyReason aReason)
{
  if (mManager) {
    MOZ_ASSERT(mActiveRequests.Length() > 0);
    mManager->RemoveListener(this);
    mManager = nullptr;
  }
}

bool
CacheStorageParent::RecvMatch(const RequestId& aRequestId,
                              const PCacheRequest& aRequest,
                              const PCacheQueryParams& aParams)
{
  if (!mManagerId) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mOp = OP_MATCH;
    entry->mRequestId = aRequestId;
    entry->mRequest = aRequest;
    entry->mParams = aParams;
    return true;
  }

  cache::Manager* manager = RequestManager(aRequestId);
  manager->StorageMatch(this, aRequestId, mNamespace, aRequest,
                        aParams);

  return true;
}

bool
CacheStorageParent::RecvHas(const RequestId& aRequestId, const nsString& aKey)
{
  if (!mManagerId) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mOp = OP_HAS;
    entry->mRequestId = aRequestId;
    entry->mKey = aKey;
    return true;
  }

  cache::Manager* manager = RequestManager(aRequestId);
  manager->StorageHas(this, aRequestId, mNamespace, aKey);

  return true;
}

bool
CacheStorageParent::RecvOpen(const RequestId& aRequestId, const nsString& aKey)
{
  if (!mManagerId) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mOp = OP_OPEN;
    entry->mRequestId = aRequestId;
    entry->mKey = aKey;
    return true;
  }

  cache::Manager* manager = RequestManager(aRequestId);
  manager->StorageOpen(this, aRequestId, mNamespace, aKey);

  return true;
}

bool
CacheStorageParent::RecvDelete(const RequestId& aRequestId,
                               const nsString& aKey)
{
  if (!mManagerId) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mOp = OP_DELETE;
    entry->mRequestId = aRequestId;
    entry->mKey = aKey;
    return true;
  }

  cache::Manager* manager = RequestManager(aRequestId);
  manager->StorageDelete(this, aRequestId, mNamespace, aKey);

  return true;
}

bool
CacheStorageParent::RecvKeys(const RequestId& aRequestId)
{
  if (!mManagerId) {
    Entry* entry = mPendingRequests.AppendElement();
    entry->mOp = OP_DELETE;
    entry->mRequestId = aRequestId;
    return true;
  }

  cache::Manager* manager = RequestManager(aRequestId);
  manager->StorageKeys(this, aRequestId, mNamespace);

  return true;
}

void
CacheStorageParent::OnPrincipalVerified(nsresult aRv, ManagerId* aManagerId)
{
  if (NS_WARN_IF(NS_FAILED(aRv))) {
    FailPendingRequests(aRv);
    unused << Send__delete__(this);
    return;
  }

  MOZ_ASSERT(mVerifier);
  MOZ_ASSERT(!mManagerId);
  MOZ_ASSERT(!mManager);

  mManagerId = aManagerId;
  mVerifier->ClearListener();
  mVerifier = nullptr;

  RetryPendingRequests();
}

void
CacheStorageParent::OnStorageMatch(RequestId aRequestId, nsresult aRv,
                                   const SavedResponse* aSavedResponse,
                                   Manager::StreamList* aStreamList)
{
  PCacheResponseOrVoid responseOrVoid;

  ReleaseManager(aRequestId);

  // no match
  if (NS_FAILED(aRv) || !aSavedResponse) {
    responseOrVoid = void_t();
    unused << SendMatchResponse(aRequestId, aRv, responseOrVoid);
    return;
  }

  // match without body data to stream
  if (!aSavedResponse->mHasBodyId) {
    responseOrVoid = aSavedResponse->mValue;
    unused << SendMatchResponse(aRequestId, aRv, responseOrVoid);
    return;
  }

  PCacheReadStream readStream;
  SerializeReadStream(nullptr, aSavedResponse->mBodyId, aStreamList,
                      &readStream);

  responseOrVoid = aSavedResponse->mValue;
  responseOrVoid.get_PCacheResponse().body() = readStream;

  unused << SendMatchResponse(aRequestId, aRv, responseOrVoid);
}

void
CacheStorageParent::OnStorageHas(RequestId aRequestId, nsresult aRv,
                                 bool aCacheFound)
{
  ReleaseManager(aRequestId);
  unused << SendHasResponse(aRequestId, aRv, aCacheFound);
}

void
CacheStorageParent::OnStorageOpen(RequestId aRequestId, nsresult aRv,
                                  CacheId aCacheId)
{
  if (NS_FAILED(aRv)) {
    ReleaseManager(aRequestId);
    unused << SendOpenResponse(aRequestId, aRv, nullptr);
    return;
  }

  MOZ_ASSERT(mManager);
  CacheParent* actor = new CacheParent(mManager, aCacheId);

  ReleaseManager(aRequestId);

  PCacheParent* base = Manager()->SendPCacheConstructor(actor);
  actor = static_cast<CacheParent*>(base);
  unused << SendOpenResponse(aRequestId, aRv, actor);
}

void
CacheStorageParent::OnStorageDelete(RequestId aRequestId, nsresult aRv,
                                    bool aCacheDeleted)
{
  ReleaseManager(aRequestId);
  unused << SendDeleteResponse(aRequestId, aRv, aCacheDeleted);
}

void
CacheStorageParent::OnStorageKeys(RequestId aRequestId, nsresult aRv,
                                  const nsTArray<nsString>& aKeys)
{
  ReleaseManager(aRequestId);
  unused << SendKeysResponse(aRequestId, aRv, aKeys);
}

Manager::StreamControl*
CacheStorageParent::SerializeReadStream(Manager::StreamControl *aStreamControl,
                                        const nsID& aId,
                                        Manager::StreamList* aStreamList,
                                        PCacheReadStream* aReadStreamOut)
{
  MOZ_ASSERT(aStreamList);
  MOZ_ASSERT(aReadStreamOut);

  nsCOMPtr<nsIInputStream> stream = aStreamList->Extract(aId);
  MOZ_ASSERT(stream);

  if (!aStreamControl) {
    aStreamControl = new CacheStreamControlParent();
    DebugOnly<PCacheStreamControlParent*> actor =
      Manager()->SendPCacheStreamControlConstructor(aStreamControl);
    MOZ_ASSERT(aStreamControl == actor);
  }

  aStreamList->SetStreamControl(aStreamControl);

  nsRefPtr<ReadStream> readStream = ReadStream::Create(aStreamControl,
                                                       aId, stream);
  readStream->Serialize(aReadStreamOut);

  return aStreamControl;
}

void
CacheStorageParent::RetryPendingRequests()
{
  for (uint32_t i = 0; i < mPendingRequests.Length(); ++i) {
    Entry& entry = mPendingRequests[i];
    switch(entry.mOp) {
      case OP_MATCH:
        RecvMatch(entry.mRequestId, entry.mRequest, entry.mParams);
        break;
      case OP_HAS:
        RecvHas(entry.mRequestId, entry.mKey);
        break;
      case OP_OPEN:
        RecvOpen(entry.mRequestId, entry.mKey);
        break;
      case OP_DELETE:
        RecvDelete(entry.mRequestId, entry.mKey);
        break;
      case OP_KEYS:
        RecvKeys(entry.mRequestId);
        break;
      default:
        MOZ_ASSERT_UNREACHABLE("Pending request within unknown op");
    }
  }
  mPendingRequests.Clear();
}

void
CacheStorageParent::FailPendingRequests(nsresult aRv)
{
  MOZ_ASSERT(NS_FAILED(aRv));

  for (uint32_t i = 0; i < mPendingRequests.Length(); ++i) {
    Entry& entry = mPendingRequests[i];
    switch(entry.mOp) {
      case OP_MATCH:
      {
        PCacheResponseOrVoid responseOrVoid;
        responseOrVoid = void_t();
        unused << SendMatchResponse(entry.mRequestId, aRv, responseOrVoid);
        break;
      }
      case OP_HAS:
        unused << SendHasResponse(entry.mRequestId, aRv, false);
        break;
      case OP_OPEN:
        unused << SendOpenResponse(entry.mRequestId, aRv, nullptr);
        break;
      case OP_DELETE:
        unused << SendDeleteResponse(entry.mRequestId, aRv, false);
        break;
      case OP_KEYS:
      {
        const nsTArray<nsString> emptyKeys;
        unused << SendKeysResponse(entry.mRequestId, aRv, emptyKeys);
        break;
      }
      default:
        MOZ_ASSERT_UNREACHABLE("Pending request within unknown op");
    }
  }
  mPendingRequests.Clear();
}

cache::Manager*
CacheStorageParent::RequestManager(RequestId aRequestId)
{
  MOZ_ASSERT(!mActiveRequests.Contains(aRequestId));
  if (!mManager) {
    MOZ_ASSERT(mActiveRequests.Length() < 1);
    mManager = Manager::GetOrCreate(mManagerId);
    MOZ_ASSERT(mManager);
  }
  mActiveRequests.AppendElement(aRequestId);
  return mManager;
}

void
CacheStorageParent::ReleaseManager(RequestId aRequestId)
{
  // Note that if the child process dies we also clean up the mManager in
  // ActorDestroy().  There is no race with this method, however, because
  // ActorDestroy removes this object from the Manager's listener list.
  // Therefore ReleaseManager() should never be called after ActorDestroy()
  // runs.
  MOZ_ASSERT(mManager);
  MOZ_ASSERT(mActiveRequests.Length() > 0);

  DebugOnly<bool> removed = mActiveRequests.RemoveElement(aRequestId);
  MOZ_ASSERT(removed);

  if (mActiveRequests.Length() < 1) {
    mManager->RemoveListener(this);
    mManager = nullptr;
  }
}

} // namespace cache
} // namespace dom
} // namespace mozilla
