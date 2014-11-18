/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStorageParent.h"

#include "mozilla/dom/cache/CacheParent.h"
#include "mozilla/dom/cache/CacheStreamControlParent.h"
#include "mozilla/dom/cache/Manager.h"
#include "mozilla/dom/cache/ReadStream.h"
#include "mozilla/dom/cache/SavedTypes.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/ipc/PFileDescriptorSetParent.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/unused.h"
#include "nsCOMPtr.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::void_t;
using mozilla::ipc::PFileDescriptorSetParent;

CacheStorageParent::CacheStorageParent(const CacheInitData& aInitData)
  : mInitData(aInitData)
  , mManager(Manager::ForOrigin(aInitData))
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
CacheStorageParent::RecvMatch(const RequestId& aRequestId,
                              const PCacheRequest& aRequest,
                              const PCacheQueryParams& aParams)
{
  mManager->StorageMatch(this, aRequestId, mInitData.namespaceEnum(), aRequest,
                         aParams);
  return true;
}

bool
CacheStorageParent::RecvHas(const RequestId& aRequestId, const nsString& aKey)
{
  mManager->StorageHas(this, aRequestId, mInitData.namespaceEnum(), aKey);
  return true;
}

bool
CacheStorageParent::RecvOpen(const RequestId& aRequestId,
                               const nsString& aKey)
{
  mManager->StorageOpen(this, aRequestId, mInitData.namespaceEnum(), aKey);
  return true;
}

bool
CacheStorageParent::RecvDelete(const RequestId& aRequestId,
                               const nsString& aKey)
{
  mManager->StorageDelete(this, aRequestId, mInitData.namespaceEnum(), aKey);
  return true;
}

bool
CacheStorageParent::RecvKeys(const RequestId& aRequestId)
{
  mManager->StorageKeys(this, aRequestId, mInitData.namespaceEnum());
  return true;
}

void
CacheStorageParent::OnStorageMatch(RequestId aRequestId, nsresult aRv,
                                   const SavedResponse* aSavedResponse,
                                   Manager::StreamList* aStreamList)
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
  unused << SendHasResponse(aRequestId, aRv, aCacheFound);
}

void
CacheStorageParent::OnStorageOpen(RequestId aRequestId, nsresult aRv,
                                  CacheId aCacheId)
{
  if (NS_FAILED(aRv)) {
    unused << SendOpenResponse(aRequestId, aRv, nullptr);
    return;
  }

  CacheParent* actor = new CacheParent(mInitData, aCacheId);
  PCacheParent* base = Manager()->SendPCacheConstructor(actor);
  actor = static_cast<CacheParent*>(base);
  unused << SendOpenResponse(aRequestId, aRv, actor);
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

} // namespace cache
} // namespace dom
} // namespace mozilla
