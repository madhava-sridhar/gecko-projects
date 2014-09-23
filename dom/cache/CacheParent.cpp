/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheParent.h"

#include "mozilla/unused.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::void_t;

CacheParent::CacheParent(const nsACString& aOrigin,
                         const nsACString& aBaseDomain,
                         CacheId aCacheId)
  : mCacheId(aCacheId)
  , mManager(Manager::ForOrigin(aOrigin, aBaseDomain))
{
  MOZ_ASSERT(mManager);
  mManager->AddRefCacheId(mCacheId);
}

CacheParent::~CacheParent()
{
  MOZ_ASSERT(!mManager);
}

void
CacheParent::ActorDestroy(ActorDestroyReason aReason)
{
  MOZ_ASSERT(mManager);
  mManager->RemoveListener(this);
  mManager->ReleaseCacheId(mCacheId);
  mManager = nullptr;
}

bool
CacheParent::RecvMatch(const RequestId& aRequestId, const PCacheRequest& aRequest,
                       const PCacheQueryParams& aParams)
{
  MOZ_ASSERT(mManager);
  mManager->CacheMatch(this, aRequestId, mCacheId, aRequest, aParams);
  return true;
}

bool
CacheParent::RecvMatchAll(const RequestId& aRequestId,
                          const PCacheRequestOrVoid& aRequest,
                          const PCacheQueryParams& aParams)
{
  MOZ_ASSERT(mManager);
  mManager->CacheMatchAll(this, aRequestId, mCacheId, aRequest, aParams);
  return true;
}

bool
CacheParent::RecvAdd(const RequestId& aRequestId, const PCacheRequest& aRequest)
{
  return false;
}

bool
CacheParent::RecvAddAll(const RequestId& aRequestId,
                        const nsTArray<PCacheRequest>& aRequests)
{
  return false;
}

bool
CacheParent::RecvPut(const RequestId& aRequestId, const PCacheRequest& aRequest,
                     const PCacheResponse& aResponse)
{
  MOZ_ASSERT(mManager);
  mManager->CachePut(this, aRequestId, mCacheId, aRequest, aResponse);
  return true;
}

bool
CacheParent::RecvDelete(const RequestId& aRequestId,
                        const PCacheRequest& aRequest,
                        const PCacheQueryParams& aParams)
{
  MOZ_ASSERT(mManager);
  mManager->CacheDelete(this, aRequestId, mCacheId, aRequest, aParams);
  return true;
}

bool
CacheParent::RecvKeys(const RequestId& aRequestId,
                      const PCacheRequestOrVoid& aRequest,
                      const PCacheQueryParams& aParams)
{
  return false;
}

void
CacheParent::OnCacheMatch(RequestId aRequestId, nsresult aRv,
                          const PCacheResponseOrVoid& aResponse)
{
  unused << SendMatchResponse(aRequestId, aRv, aResponse);
}

void
CacheParent::OnCacheMatchAll(RequestId aRequestId, nsresult aRv,
                             const nsTArray<PCacheResponse>& aResponses)
{
  unused << SendMatchAllResponse(aRequestId, aRv, aResponses);
}

void
CacheParent::OnCachePut(RequestId aRequestId, nsresult aRv,
                        const PCacheResponseOrVoid& aResponseOrVoid)
{
  unused << SendPutResponse(aRequestId, aRv, aResponseOrVoid);
}

void
CacheParent::OnCacheDelete(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  unused << SendDeleteResponse(aRequestId, aRv, aSuccess);
}

} // namespace cache
} // namespace dom
} // namesapce mozilla
