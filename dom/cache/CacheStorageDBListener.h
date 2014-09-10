/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStorageDBListener_h
#define mozilla_dom_cache_CacheStorageDBListener_h

#include "mozilla/dom/CacheTypes.h"

namespace mozilla {
namespace dom {

class CacheStorageDBListener
{
  public:
    virtual ~CacheStorageDBListener() { }
    virtual void OnGet(cache::RequestId aRequestId, nsresult aRv,
                       nsID* aCacheId)=0;
    virtual void OnHas(cache::RequestId aRequestId, nsresult aRv,
                       bool aSuccess)=0;
    virtual void OnPut(cache::RequestId aRequestId, nsresult aRv,
                       bool aSuccess)=0;
    virtual void OnDelete(cache::RequestId aRequestId, nsresult aRv,
                          bool aSuccess)=0;
    virtual void OnKeys(cache::RequestId aRequestId, nsresult aRv,
                        const nsTArray<nsString>& aKeys)=0;

    // TODO: OnConnected
    // TODO: OnError (or pass nsresult in each On*() method?
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStorageDBListener_h
