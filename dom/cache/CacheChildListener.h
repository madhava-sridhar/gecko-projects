/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheChildListener_h
#define mozilla_dom_cache_CacheChildListener_h

#include "mozilla/dom/cache/Types.h"
#include "nsTArray.h"

namespace mozilla {

namespace ipc {
  class IProtocol;
}

namespace dom {
namespace cache {

class PCacheRequest;
class PCacheResponse;
class PCacheResponseOrVoid;
class PCacheStreamControlChild;

class CacheChildListener
{
public:
  virtual ~CacheChildListener() { }
  virtual void ActorDestroy(mozilla::ipc::IProtocol& aActor)=0;

  virtual void
  RecvMatchResponse(RequestId aRequestId, nsresult aRv,
                    const PCacheResponseOrVoid& aResponse,
                    PCacheStreamControlChild* aStreamControl)=0;
  virtual void
  RecvMatchAllResponse(RequestId aRequestId, nsresult aRv,
                       const nsTArray<PCacheResponse>& aResponses,
                       PCacheStreamControlChild* aStreamControl)=0;
  virtual void
  RecvAddResponse(RequestId aRequestId, nsresult aRv)=0;

  virtual void
  RecvAddAllResponse(RequestId aRequestId, nsresult aRv)=0;

  virtual void
  RecvPutResponse(RequestId aRequestId, nsresult aRv)=0;

  virtual void
  RecvDeleteResponse(RequestId aRequestId, nsresult aRv,
                     bool aSuccess)=0;
  virtual void
  RecvKeysResponse(RequestId aRequestId, nsresult aRv,
                   const nsTArray<PCacheRequest>& aRequests,
                   PCacheStreamControlChild* aStreamControl)=0;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_CacheChildListener_h
