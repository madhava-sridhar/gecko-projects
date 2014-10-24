/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheChild_h
#define mozilla_dom_cache_CacheChild_h

#include "mozilla/dom/cache/PCacheChild.h"

namespace mozilla {
namespace dom {
namespace cache {

class CacheChildListener;

class CacheChild MOZ_FINAL : public PCacheChild
{
  CacheChildListener* mListener;

public:
  CacheChild();
  virtual ~CacheChild();

  virtual void ActorDestroy(ActorDestroyReason aReason) MOZ_OVERRIDE;

  void SetListener(CacheChildListener& aListener);
  void ClearListener();

  // PCacheChild methods
  virtual bool
  RecvMatchResponse(const RequestId& requestId, const nsresult& aRv,
                    const PCacheResponseOrVoid& aResponse,
                    PCacheStreamControlChild* aStreamControl) MOZ_OVERRIDE;
  virtual bool
  RecvMatchAllResponse(const RequestId& requestId, const nsresult& aRv,
                       const nsTArray<PCacheResponse>& responses,
                       PCacheStreamControlChild* aStreamControl) MOZ_OVERRIDE;
  virtual bool
  RecvAddResponse(const RequestId& requestId, const nsresult& aRv) MOZ_OVERRIDE;

  virtual bool
  RecvAddAllResponse(const RequestId& requestId,
                     const nsresult& aRv) MOZ_OVERRIDE;
  virtual bool
  RecvPutResponse(const RequestId& aRequestId,
                  const nsresult& aRv) MOZ_OVERRIDE;
  virtual bool
  RecvDeleteResponse(const RequestId& requestId, const nsresult& aRv,
                     const bool& result) MOZ_OVERRIDE;
  virtual bool
  RecvKeysResponse(const RequestId& requestId, const nsresult& aRv,
                   const nsTArray<PCacheRequest>& requests,
                   PCacheStreamControlChild* aStreamControl) MOZ_OVERRIDE;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheChild_h
