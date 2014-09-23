/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStorageParent.h"

#include "mozilla/dom/cache/CacheParent.h"
#include "mozilla/dom/cache/Manager.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/unused.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {
namespace cache {

CacheStorageParent::CacheStorageParent(Namespace aNamespace,
                                       const nsACString& aOrigin,
                                       const nsACString& aBaseDomain)
  : mNamespace(aNamespace)
  , mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mManager(Manager::ForOrigin(aOrigin, aBaseDomain))
{
  MOZ_ASSERT(mManager);
}

CacheStorageParent::~CacheStorageParent()
{
  MOZ_ASSERT(!mManager);
}

void
CacheStorageParent::ActorDestroy(ActorDestroyReason aReason)
{
  MOZ_ASSERT(mManager);
  mManager->RemoveListener(this);
  mManager = nullptr;
}

bool
CacheStorageParent::RecvGet(const RequestId& aRequestId, const nsString& aKey)
{
  mManager->StorageGet(this, aRequestId, mNamespace, aKey);
  return true;
}

bool
CacheStorageParent::RecvHas(const RequestId& aRequestId, const nsString& aKey)
{
  mManager->StorageHas(this, aRequestId, mNamespace, aKey);
  return true;
}

bool
CacheStorageParent::RecvCreate(const RequestId& aRequestId,
                               const nsString& aKey)
{
  mManager->StorageCreate(this, aRequestId, mNamespace, aKey);
  return true;
}

bool
CacheStorageParent::RecvDelete(const RequestId& aRequestId,
                               const nsString& aKey)
{
  mManager->StorageDelete(this, aRequestId, mNamespace, aKey);
  return true;
}

bool
CacheStorageParent::RecvKeys(const RequestId& aRequestId)
{
  mManager->StorageKeys(this, aRequestId, mNamespace);
  return true;
}

void
CacheStorageParent::OnStorageGet(RequestId aRequestId, nsresult aRv,
                                 bool aCacheFound, CacheId aCacheId)
{
  if (NS_FAILED(aRv) || !aCacheFound) {
    unused << SendGetResponse(aRequestId, aRv, nullptr);
    return;
  }

  CacheParent* actor = new CacheParent(mOrigin, mBaseDomain, aCacheId);
  PCacheParent* base = Manager()->SendPCacheConstructor(actor);
  actor = static_cast<CacheParent*>(base);
  unused << SendGetResponse(aRequestId, aRv, actor);
}

void
CacheStorageParent::OnStorageHas(RequestId aRequestId, nsresult aRv,
                                 bool aCacheFound)
{
  unused << SendHasResponse(aRequestId, aRv, aCacheFound);
}

void
CacheStorageParent::OnStorageCreate(RequestId aRequestId, nsresult aRv,
                                    CacheId aCacheId)
{
  if (NS_FAILED(aRv)) {
    unused << SendCreateResponse(aRequestId, aRv, nullptr);
    return;
  }

  CacheParent* actor = new CacheParent(mOrigin, mBaseDomain, aCacheId);
  PCacheParent* base = Manager()->SendPCacheConstructor(actor);
  actor = static_cast<CacheParent*>(base);
  unused << SendCreateResponse(aRequestId, aRv, actor);
}

void
CacheStorageParent::OnStorageDelete(RequestId aRequestId, nsresult aRv,
                                    bool aCacheDeleted)
{
  unused << SendDeleteResponse(aRequestId, aRv, aCacheDeleted);
}

void
CacheStorageParent::OnStorageKeys(RequestId aRequestId, nsresult aRv,
                                  const nsTArray<nsString>& aKeys)
{
  unused << SendKeysResponse(aRequestId, aRv, aKeys);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
