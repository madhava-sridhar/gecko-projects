/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheStorageParent.h"

#include "mozilla/dom/CacheStorageDBConnection.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/unused.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {

using mozilla::dom::cache::RequestId;

CacheStorageParent::CacheStorageParent(cache::Namespace aNamespace,
                                       const nsACString& aOrigin,
                                       const nsACString& aBaseDomain)
  : mNamespace(aNamespace)
  , mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mDBConnection(new CacheStorageDBConnection(this, mNamespace, mOrigin,
                                               mBaseDomain))
{
  MOZ_ASSERT(mDBConnection);
}

CacheStorageParent::~CacheStorageParent()
{
  MOZ_ASSERT(!mDBConnection);
}

void
CacheStorageParent::ActorDestroy(ActorDestroyReason aReason)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->ClearListener();
  mDBConnection = nullptr;
}

bool
CacheStorageParent::RecvGet(const RequestId& aRequestId, const nsString& aKey)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Get(aRequestId, aKey);
  return true;
}

bool
CacheStorageParent::RecvHas(const RequestId& aRequestId, const nsString& aKey)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Has(aRequestId, aKey);
  return true;
}

bool
CacheStorageParent::RecvCreate(const RequestId& aRequestId,
                               const nsString& aKey)
{
  MOZ_ASSERT(mDBConnection);

  // TODO: perform a Has() check first
  // TODO: create real DB-backed cache object
  // TODO: get uuid from cache object
  nsID uuid;

  mDBConnection->Put(aRequestId, aKey, uuid);

  return true;
}

bool
CacheStorageParent::RecvDelete(const RequestId& aRequestId,
                               const nsString& aKey)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Delete(aRequestId, aKey);
  return true;
}

bool
CacheStorageParent::RecvKeys(const RequestId& aRequestId)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Keys(aRequestId);
  return true;
}

void
CacheStorageParent::OnGet(cache::RequestId aRequestId, nsresult aRv,
                          nsID* aCacheId)
{
  if (NS_FAILED(aRv) || !aCacheId) {
    unused << SendGetResponse(aRequestId, aRv, nullptr);
    return;
  }

  // TODO: create cache parent for given uuid
  CacheParent* actor = new CacheParent(mOrigin, mBaseDomain);
  if (actor) {
    PCacheParent* base = Manager()->SendPCacheConstructor(actor, mOrigin,
                                                          mBaseDomain);
    actor = static_cast<CacheParent*>(base);
  }
  unused << SendGetResponse(aRequestId, aRv, actor);
}

void
CacheStorageParent::OnHas(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  unused << SendHasResponse(aRequestId, aRv, aSuccess);
}

void
CacheStorageParent::OnPut(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  if (NS_FAILED(aRv)) {
    unused << SendCreateResponse(aRequestId, aRv, nullptr);
    return;
  }

  CacheParent* actor = nullptr;
  if (aSuccess) {
    // TODO: retrieve DB-backed actor for uuid generated in RecvCreate()
    actor = new CacheParent(mOrigin, mBaseDomain);
    if (actor) {
      PCacheParent* base = Manager()->SendPCacheConstructor(actor, mOrigin,
                                                            mBaseDomain);
      actor = static_cast<CacheParent*>(base);
    }
  }
  unused << SendCreateResponse(aRequestId, aRv, actor);
}

void
CacheStorageParent::OnDelete(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  unused << SendDeleteResponse(aRequestId, aRv, aSuccess);
}

void
CacheStorageParent::OnKeys(RequestId aRequestId, nsresult aRv,
                           const nsTArray<nsString>& aKeys)
{
  unused << SendKeysResponse(aRequestId, aRv, aKeys);
}

} // namespace dom
} // namespace mozilla
