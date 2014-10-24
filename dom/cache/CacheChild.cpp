/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/CacheChild.h"

#include "mozilla/dom/cache/CacheChildListener.h"

namespace mozilla {
namespace dom {
namespace cache {

CacheChild::CacheChild()
  : mListener(nullptr)
{
}

CacheChild::~CacheChild()
{
  MOZ_ASSERT(!mListener);
}

void
CacheChild::ActorDestroy(ActorDestroyReason aReason)
{
  // If the listener is destroyed before we are, then they will clear
  // their registration.
  if (mListener) {
    mListener->ActorDestroy(*this);
  }
}

void
CacheChild::SetListener(CacheChildListener& aListener)
{
  MOZ_ASSERT(!mListener);
  mListener = &aListener;
}

void
CacheChild::ClearListener()
{
  MOZ_ASSERT(mListener);
  mListener = nullptr;
}

bool
CacheChild::RecvMatchResponse(const RequestId& requestId, const nsresult& aRv,
                              const PCacheResponseOrVoid& aResponse,
                              PCacheStreamControlChild* aStreamControl)
{
  MOZ_ASSERT(mListener);
  mListener->RecvMatchResponse(requestId, aRv, aResponse, aStreamControl);
  return true;
}

bool
CacheChild::RecvMatchAllResponse(const RequestId& requestId, const nsresult& aRv,
                                 const nsTArray<PCacheResponse>& responses,
                                 PCacheStreamControlChild* aStreamControl)
{
  MOZ_ASSERT(mListener);
  mListener->RecvMatchAllResponse(requestId, aRv, responses, aStreamControl);
  return true;
}

bool
CacheChild::RecvAddResponse(const RequestId& requestId, const nsresult& aRv)
{
  MOZ_ASSERT(mListener);
  mListener->RecvAddResponse(requestId, aRv);
  return true;
}

bool
CacheChild::RecvAddAllResponse(const RequestId& requestId, const nsresult& aRv)
{
  MOZ_ASSERT(mListener);
  mListener->RecvAddAllResponse(requestId, aRv);
  return true;
}

bool
CacheChild::RecvPutResponse(const RequestId& aRequestId, const nsresult& aRv)
{
  MOZ_ASSERT(mListener);
  mListener->RecvPutResponse(aRequestId, aRv);
  return true;
}

bool
CacheChild::RecvDeleteResponse(const RequestId& requestId, const nsresult& aRv,
                               const bool& result)
{
  MOZ_ASSERT(mListener);
  mListener->RecvDeleteResponse(requestId, aRv, result);
  return true;
}

bool
CacheChild::RecvKeysResponse(const RequestId& requestId, const nsresult& aRv,
                             const nsTArray<PCacheRequest>& requests,
                             PCacheStreamControlChild* aStreamControl)
{
  MOZ_ASSERT(mListener);
  mListener->RecvKeysResponse(requestId, aRv, requests, aStreamControl);
  return true;
}

} // namespace cache
} // namespace dom
} // namesapce mozilla
