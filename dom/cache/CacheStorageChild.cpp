/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheStorageChild.h"

#include "mozilla/dom/cache/CacheStorageChildListener.h"

namespace mozilla {
namespace dom {
namespace cache {

CacheStorageChild::CacheStorageChild(CacheStorageChildListener& aListener)
  : mListener(&aListener)
{
}

CacheStorageChild::~CacheStorageChild()
{
  MOZ_ASSERT(!mListener);
}

void
CacheStorageChild::ActorDestroy(ActorDestroyReason aReason)
{
  // If the listener is destroyed before we are, then they will clear
  // their registration.
  if (mListener) {
    mListener->ActorDestroy(*this);
  }
}

bool
CacheStorageChild::RecvMatchResponse(const RequestId& aRequestId,
                                     const nsresult& aRv,
                                     const PCacheResponseOrVoid& aResponseOrVoid,
                                     PCacheStreamControlChild* aStreamControl)
{
  MOZ_ASSERT(mListener);
  mListener->RecvMatchResponse(aRequestId, aRv, aResponseOrVoid, aStreamControl);
  return true;
}

bool
CacheStorageChild::RecvHasResponse(const RequestId& aRequestId,
                                   const nsresult& aRv,
                                   const bool& aSuccess)
{
  MOZ_ASSERT(mListener);
  mListener->RecvHasResponse(aRequestId, aRv, aSuccess);
  return true;
}

bool
CacheStorageChild::RecvOpenResponse(const RequestId& aRequestId,
                                    const nsresult& aRv,
                                    PCacheChild* aActor)
{
  MOZ_ASSERT(mListener);
  mListener->RecvOpenResponse(aRequestId, aRv, aActor);
  return true;
}

bool
CacheStorageChild::RecvDeleteResponse(const RequestId& aRequestId,
                                      const nsresult& aRv,
                                      const bool& aResult)
{
  MOZ_ASSERT(mListener);
  mListener->RecvDeleteResponse(aRequestId, aRv, aResult);
  return true;
}

bool
CacheStorageChild::RecvKeysResponse(const RequestId& aRequestId,
                                    const nsresult& aRv,
                                    const nsTArray<nsString>& aKeys)
{
  MOZ_ASSERT(mListener);
  mListener->RecvKeysResponse(aRequestId, aRv, aKeys);
  return true;
}

void
CacheStorageChild::ClearListener()
{
  MOZ_ASSERT(mListener);
  mListener = nullptr;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
