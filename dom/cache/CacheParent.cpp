/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheParent.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/unused.h"
#include "mozilla/dom/cache/CacheStreamControlParent.h"
#include "mozilla/dom/cache/SavedTypes.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/ipc/FileDescriptorSetParent.h"
#include "mozilla/ipc/PFileDescriptorSetParent.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::void_t;
using mozilla::ipc::FileDescriptorSetParent;
using mozilla::ipc::PFileDescriptorSetParent;

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
  mManager->CacheMatch(this, aRequestId, mCacheId, aRequest,
                       aParams);
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
  unused << SendAddResponse(aRequestId, NS_ERROR_NOT_IMPLEMENTED);
  return true;
}

bool
CacheParent::RecvAddAll(const RequestId& aRequestId,
                        const nsTArray<PCacheRequest>& aRequests)
{
  nsTArray<PCacheResponse> responses;
  unused << SendAddAllResponse(aRequestId, NS_ERROR_NOT_IMPLEMENTED);
  return true;
}

bool
CacheParent::RecvPut(const RequestId& aRequestId, const PCacheRequest& aRequest,
                     const PCacheResponse& aResponse)
{
  MOZ_ASSERT(mManager);

  nsCOMPtr<nsIInputStream> requestStream =
    DeserializeCacheStream(aRequest.body());

  nsCOMPtr<nsIInputStream> responseStream =
    DeserializeCacheStream(aResponse.body());

  mManager->CachePut(this, aRequestId, mCacheId, aRequest, requestStream,
                     aResponse, responseStream);
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
                          const SavedResponse* aSavedResponse,
                          Manager::StreamList* aStreamList)
{
  PCacheResponseOrVoid responseOrVoid;

  // no match
  if (NS_FAILED(aRv) || !aSavedResponse || !aStreamList) {
    responseOrVoid = void_t();
    unused << SendMatchResponse(aRequestId, aRv, responseOrVoid, nullptr);
    return;
  }

  // match without body data to stream
  if (!aSavedResponse->mHasBodyId) {
    responseOrVoid = aSavedResponse->mValue;
    responseOrVoid.get_PCacheResponse().body() = void_t();
    unused << SendMatchResponse(aRequestId, aRv, responseOrVoid, nullptr);
    return;
  }

  PCacheReadStream readStream;
  Manager::StreamControl* streamControl =
    SerializeReadStream(nullptr, aSavedResponse->mBodyId, aStreamList,
                        &readStream);

  responseOrVoid = aSavedResponse->mValue;
  responseOrVoid.get_PCacheResponse().body() = readStream;

  unused << SendMatchResponse(aRequestId, aRv, responseOrVoid, streamControl);
}

void
CacheParent::OnCacheMatchAll(RequestId aRequestId, nsresult aRv,
                             const nsTArray<SavedResponse>& aSavedResponses,
                             Manager::StreamList* aStreamList)
{
  Manager::StreamControl* streamControl = nullptr;
  nsTArray<PCacheResponse> responses;

  for (uint32_t i = 0; i < aSavedResponses.Length(); ++i) {
    PCacheResponse* res = responses.AppendElement();
    *res = aSavedResponses[i].mValue;

    if (!aSavedResponses[i].mHasBodyId) {
      res->body() = void_t();
      continue;
    }

    PCacheReadStream readStream;
    streamControl =
      SerializeReadStream(streamControl, aSavedResponses[i].mBodyId,
                          aStreamList, &readStream);
    res->body() = readStream;
  }

  unused << SendMatchAllResponse(aRequestId, aRv, responses, streamControl);
}

void
CacheParent::OnCachePut(RequestId aRequestId, nsresult aRv)
{
  unused << SendPutResponse(aRequestId, aRv);
  return;
}

void
CacheParent::OnCacheDelete(RequestId aRequestId, nsresult aRv, bool aSuccess)
{
  unused << SendDeleteResponse(aRequestId, aRv, aSuccess);
}

void
CacheParent::OnCacheKeys(RequestId aRequestId, nsresult aRv,
                         const nsTArray<SavedRequest>& aSavedRequests,
                         Manager::StreamList* aStreamList)
{
  Manager::StreamControl* streamControl = nullptr;
  nsTArray<PCacheRequest> requests;

  for (uint32_t i = 0; i < aSavedRequests.Length(); ++i) {
    PCacheRequest* req = requests.AppendElement();
    *req = aSavedRequests[i].mValue;

    if (!aSavedRequests[i].mHasBodyId) {
      req->body() = void_t();
      continue;
    }

    PCacheReadStream readStream;
    streamControl =
      SerializeReadStream(streamControl, aSavedRequests[i].mBodyId,
                          aStreamList, &readStream);
    req->body() = readStream;
  }

  unused << SendKeysResponse(aRequestId, aRv, requests, streamControl);
}

Manager::StreamControl*
CacheParent::SerializeReadStream(Manager::StreamControl *aStreamControl,
                                 const nsID& aId,
                                 Manager::StreamList* aStreamList,
                                 PCacheReadStream* aReadStreamOut)
{
  MOZ_ASSERT(aStreamList);
  MOZ_ASSERT(aReadStreamOut);

  aReadStreamOut->id() = aId;
  nsCOMPtr<nsIInputStream> stream = aStreamList->Extract(aId);
  MOZ_ASSERT(stream);

  nsTArray<FileDescriptor> fds;
  SerializeInputStream(stream, aReadStreamOut->params(), fds);

  PFileDescriptorSetParent* fdSet = nullptr;
  if (!fds.IsEmpty()) {
    fdSet = Manager()->SendPFileDescriptorSetConstructor(fds[0]);
    for (uint32_t i = 1; i < fds.Length(); ++i) {
      unused << fdSet->SendAddFileDescriptor(fds[i]);
    }
  }

  if (fdSet) {
    aReadStreamOut->fds() = fdSet;
  } else {
    aReadStreamOut->fds() = void_t();
  }

  if (!aStreamControl) {
    aStreamControl = new CacheStreamControlParent();
    DebugOnly<PCacheStreamControlParent*> actor =
      Manager()->SendPCacheStreamControlConstructor(aStreamControl);
    MOZ_ASSERT(aStreamControl == actor);
  }

  aStreamList->SetStreamControl(aStreamControl);

  return aStreamControl;
}

already_AddRefed<nsIInputStream>
CacheParent::DeserializeCacheStream(const PCacheReadStreamOrVoid& aStreamOrVoid)
{
  if (aStreamOrVoid.type() == PCacheReadStreamOrVoid::Tvoid_t) {
    return nullptr;
  }

  const PCacheReadStream& readStream = aStreamOrVoid.get_PCacheReadStream();

  nsTArray<FileDescriptor> fds;
  if (readStream.fds().type() ==
      OptionalFileDescriptorSet::TPFileDescriptorSetChild) {

    FileDescriptorSetParent* fdSetActor =
      static_cast<FileDescriptorSetParent*>(readStream.fds().get_PFileDescriptorSetParent());
    MOZ_ASSERT(fdSetActor);

    fdSetActor->ForgetFileDescriptors(fds);
    MOZ_ASSERT(!fds.IsEmpty());

    unused << fdSetActor->Send__delete__(fdSetActor);
  }

  return DeserializeInputStream(readStream.params(), fds);
}

} // namespace cache
} // namespace dom
} // namesapce mozilla
