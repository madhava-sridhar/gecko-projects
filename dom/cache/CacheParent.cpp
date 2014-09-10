/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheParent.h"

#include "mozilla/dom/CacheDBConnection.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {

using mozilla::void_t;
using mozilla::dom::cache::RequestId;

CacheParent::CacheParent(const nsACString& aOrigin,
                         const nsACString& aBaseDomain)
  : mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mDBConnection(CacheDBConnection::Create(this, aOrigin, aBaseDomain))
{
}

CacheParent::CacheParent(const nsACString& aOrigin,
                         const nsACString& aBaseDomain,
                         const nsID& aExistingCacheId)
  : mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mDBConnection(new CacheDBConnection(this, aOrigin, aBaseDomain,
                                        aExistingCacheId))
{
}

CacheParent::~CacheParent()
{
  MOZ_ASSERT(!mDBConnection);
}

void
CacheParent::ActorDestroy(ActorDestroyReason aReason)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->ClearListener();
  mDBConnection = nullptr;
}

bool
CacheParent::RecvMatch(const RequestId& aRequestId, const PCacheRequest& aRequest,
                       const PCacheQueryParams& aParams)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Match(aRequestId, aRequest, aParams);
  return true;
}

bool
CacheParent::RecvMatchAll(const RequestId& aRequestId,
                          const PCacheRequestOrVoid& aRequest,
                          const PCacheQueryParams& aParams)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->MatchAll(aRequestId, aRequest, aParams);
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
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Put(aRequestId, aRequest, aResponse);
  return true;
}

bool
CacheParent::RecvDelete(const RequestId& aRequestId,
                        const PCacheRequest& aRequest,
                        const PCacheQueryParams& aParams)
{
  MOZ_ASSERT(mDBConnection);
  mDBConnection->Delete(aRequestId, aRequest, aParams);
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
CacheParent::OnMatch(cache::RequestId aRequestId, nsresult aRv,
                     const PCacheResponseOrVoid& aResponse)
{
  unused << SendMatchResponse(aRequestId, aRv, aResponse);
}

void
CacheParent::OnMatchAll(cache::RequestId aRequestId, nsresult aRv,
                        const nsTArray<PCacheResponse>& aResponses)
{
  unused << SendMatchAllResponse(aRequestId, aRv, aResponses);
}

void
CacheParent::OnPut(RequestId aRequestId, nsresult aRv,
                   const PCacheResponseOrVoid& aResponse)
{
  unused << SendPutResponse(aRequestId, aRv, aResponse);
}

void
CacheParent::OnDelete(RequestId aRequestId, nsresult aRv,
                      bool aSuccess)
{
  unused << SendDeleteResponse(aRequestId, aRv, aSuccess);
}

} // namespace dom
} // namesapce mozilla
