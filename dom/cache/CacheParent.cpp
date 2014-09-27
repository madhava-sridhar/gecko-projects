/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheParent.h"

#include "mozilla/unused.h"
#include "mozilla/dom/cache/SavedTypes.h"
#include "nsCOMPtr.h"

// TODO: remove testing only headers
#include "../../dom/filehandle/MemoryStreams.h"
#include "nsStringStream.h"

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

  // TODO: remove stream test code
  nsCOMPtr<nsIInputStream> requestStream;
  nsresult rv = NS_NewCStringInputStream(getter_AddRefs(requestStream),
                NS_LITERAL_CSTRING("request body stream beep beep boop!"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    OnCachePut(aRequestId, rv, nullptr);
    return true;
  }

  nsCOMPtr<nsIInputStream> responseStream;
  rv = NS_NewCStringInputStream(getter_AddRefs(responseStream),
                NS_LITERAL_CSTRING("response body stream hooray!"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    OnCachePut(aRequestId, rv, nullptr);
    return true;
  }

  mManager->CachePut(this, aRequestId, mCacheId,
                     aRequest, requestStream, aResponse, responseStream);
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
  MOZ_ASSERT(mManager);
  mManager->CacheKeys(this, aRequestId, mCacheId, aRequest, aParams);
  return true;
}

void
CacheParent::OnCacheMatch(RequestId aRequestId, nsresult aRv,
                          const SavedResponse* aSavedResponse)
{
  PCacheResponseOrVoid responseOrVoid;

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

  // TODO: remove stream test code
  nsCOMPtr<nsIOutputStream> stream = MemoryOutputStream::Create(4096);

  mManager->CacheReadBody(mCacheId, aSavedResponse->mBodyId, stream);
  responseOrVoid = aSavedResponse->mValue;
  unused << SendMatchResponse(aRequestId, aRv, responseOrVoid);
}

void
CacheParent::OnCacheMatchAll(RequestId aRequestId, nsresult aRv,
                             const nsTArray<SavedResponse>& aSavedResponses)
{
  nsTArray<PCacheResponse> responses;
  nsTArray<nsCOMPtr<nsIOutputStream>> responseStreams;
  for (uint32_t i = 0; i < aSavedResponses.Length(); ++i) {
    responses.AppendElement(aSavedResponses[i].mValue);

    if (!aSavedResponses[i].mHasBodyId) {
      responseStreams.AppendElement();
    } else {
      // TODO: remove stream test code
      responseStreams.AppendElement(MemoryOutputStream::Create(4096));
      mManager->CacheReadBody(mCacheId, aSavedResponses[i].mBodyId,
                              responseStreams[i]);
    }
  }

  unused << SendMatchAllResponse(aRequestId, aRv, responses);
}

void
CacheParent::OnCachePut(RequestId aRequestId, nsresult aRv,
                        const SavedResponse* aSavedResponse)
{
  PCacheResponseOrVoid responseOrVoid;

  // no match
  if (NS_FAILED(aRv) || !aSavedResponse) {
    responseOrVoid = void_t();
    unused << SendPutResponse(aRequestId, aRv, responseOrVoid);
    return;
  }

  // match without body data to stream
  if (!aSavedResponse->mHasBodyId) {
    responseOrVoid = aSavedResponse->mValue;
    unused << SendPutResponse(aRequestId, aRv, responseOrVoid);
    return;
  }

  // TODO: remove stream test code
  nsCOMPtr<nsIOutputStream> stream = MemoryOutputStream::Create(4096);

  mManager->CacheReadBody(mCacheId, aSavedResponse->mBodyId, stream);
  responseOrVoid = aSavedResponse->mValue;
  unused << SendPutResponse(aRequestId, aRv, responseOrVoid);
}

void
CacheParent::OnCacheDelete(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  unused << SendDeleteResponse(aRequestId, aRv, aSuccess);
}

void
CacheParent::OnCacheKeys(RequestId aRequestId, nsresult aRv,
                         const nsTArray<SavedRequest>& aSavedRequests)
{
  nsTArray<PCacheRequest> requests;
  nsTArray<nsCOMPtr<nsIOutputStream>> requestStreams;
  for (uint32_t i = 0; i < aSavedRequests.Length(); ++i) {
    requests.AppendElement(aSavedRequests[i].mValue);

    if (!aSavedRequests[i].mHasBodyId) {
      requestStreams.AppendElement();
    } else {
      // TODO: remove stream test code
      requestStreams.AppendElement(MemoryOutputStream::Create(4096));
      mManager->CacheReadBody(mCacheId, aSavedRequests[i].mBodyId,
                              requestStreams[i]);
    }
  }

  unused << SendKeysResponse(aRequestId, aRv, requests);
}

} // namespace cache
} // namespace dom
} // namesapce mozilla
