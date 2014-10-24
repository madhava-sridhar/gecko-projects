/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStorageChild_h
#define mozilla_dom_cache_CacheStorageChild_h

#include "mozilla/dom/cache/Types.h"
#include "mozilla/dom/cache/PCacheStorageChild.h"

namespace mozilla {
namespace dom {
namespace cache {

class CacheStorageChildListener;
class PCacheChild;

class CacheStorageChild MOZ_FINAL : public PCacheStorageChild
{
public:
  CacheStorageChild(CacheStorageChildListener& aListener);
  virtual ~CacheStorageChild();
  virtual void ActorDestroy(ActorDestroyReason aReason) MOZ_OVERRIDE;
  virtual bool RecvMatchResponse(const RequestId& requestId,
                             const nsresult& aRv,
                             const PCacheResponseOrVoid& response,
                             PCacheStreamControlChild* aStreamControl) MOZ_OVERRIDE;
  virtual bool RecvHasResponse(const cache::RequestId& aRequestId,
                               const nsresult& aRv,
                               const bool& aSuccess) MOZ_OVERRIDE;
  virtual bool RecvOpenResponse(const cache::RequestId& aRequestId,
                                const nsresult& aRv,
                                PCacheChild* aActor) MOZ_OVERRIDE;
  virtual bool RecvDeleteResponse(const cache::RequestId& aRequestId,
                                  const nsresult& aRv,
                                  const bool& aResult) MOZ_OVERRIDE;
  virtual bool RecvKeysResponse(const cache::RequestId& aRequestId,
                                const nsresult& aRv,
                                const nsTArray<nsString>& aKeys) MOZ_OVERRIDE;

  void ClearListener();
private:
  CacheStorageChildListener* mListener;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStorageChild_h
