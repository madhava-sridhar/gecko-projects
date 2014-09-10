/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CacheChildListener_h
#define mozilla_dom_CacheChildListener_h

#include "mozilla/dom/CacheTypes.h"
#include "nsTArray.h"

namespace mozilla {

namespace ipc {
  class IProtocol;
}

namespace dom {

class PCacheRequest;
class PCacheResponse;
class PCacheResponseOrVoid;

class CacheChildListener
{
public:
  virtual ~CacheChildListener() { }
  virtual void ActorDestroy(mozilla::ipc::IProtocol& aActor)=0;

  virtual void
  RecvMatchResponse(cache::RequestId aRequestId, nsresult aRv,
                    const PCacheResponseOrVoid& aResponse)=0;
  virtual void
  RecvMatchAllResponse(cache::RequestId aRequestId, nsresult aRv,
                       const nsTArray<PCacheResponse>& aResponses)=0;
  virtual void
  RecvAddResponse(cache::RequestId aRequestId, nsresult aRv,
                  const PCacheResponse& aResponse)=0;
  virtual void
  RecvAddAllResponse(cache::RequestId aRequestId, nsresult aRv,
                     const nsTArray<PCacheResponse>& aResponses)=0;
  virtual void
  RecvPutResponse(cache::RequestId aRequestId, nsresult aRv,
                  const PCacheResponseOrVoid& aResponse)=0;
  virtual void
  RecvDeleteResponse(cache::RequestId aRequestId, nsresult aRv,
                     bool aSuccess)=0;
  virtual void
  RecvKeysResponse(cache::RequestId aRequestId, nsresult aRv,
                   const nsTArray<PCacheRequest>& aRequests)=0;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_CacheChildListener_h
