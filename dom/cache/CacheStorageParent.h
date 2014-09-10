/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheStorageParent_h
#define mozilla_dom_cache_CacheStorageParent_h

#include "mozilla/dom/CacheStorageDBListener.h"
#include "mozilla/dom/CacheTypes.h"
#include "mozilla/dom/PCacheStorageParent.h"

template <class T> class nsRefPtr;

namespace mozilla {
namespace dom {

class CacheStorageDBConnection;
class CacheStorageManager;

class CacheStorageParent MOZ_FINAL : public PCacheStorageParent
                                   , public CacheStorageDBListener
{
public:
  CacheStorageParent(cache::Namespace aNamespace, const nsACString& aOrigin,
                     const nsACString& mBaseDomain);
  virtual ~CacheStorageParent();

  // PCacheStorageParent methods
  virtual void ActorDestroy(ActorDestroyReason aReason) MOZ_OVERRIDE;
  virtual bool RecvGet(const cache::RequestId& aRequestId,
                       const nsString& aKey) MOZ_OVERRIDE;
  virtual bool RecvHas(const cache::RequestId& aRequestId,
                       const nsString& aKey) MOZ_OVERRIDE;
  virtual bool RecvCreate(const cache::RequestId& aRequestId,
                          const nsString& aKey) MOZ_OVERRIDE;
  virtual bool RecvDelete(const cache::RequestId& aRequestId,
                          const nsString& aKey) MOZ_OVERRIDE;
  virtual bool RecvKeys(const cache::RequestId& aRequestId) MOZ_OVERRIDE;

  // CacheStorageDBListener
  virtual void OnGet(cache::RequestId aRequestId, nsresult aRv,
                     nsID* aCacheId) MOZ_OVERRIDE;
  virtual void OnHas(cache::RequestId aRequestId, nsresult aRv,
                     bool aSuccess) MOZ_OVERRIDE;
  virtual void OnPut(cache::RequestId aRequestId, nsresult aRv,
                     bool aSuccess) MOZ_OVERRIDE;
  virtual void OnDelete(cache::RequestId aRequestId, nsresult aRv,
                        bool aSuccess) MOZ_OVERRIDE;
  virtual void OnKeys(cache::RequestId aRequestId, nsresult aRv,
                      const nsTArray<nsString>& aKeys) MOZ_OVERRIDE;

private:
  const cache::Namespace mNamespace;
  const nsCString mOrigin;
  const nsCString mBaseDomain;
  nsRefPtr<CacheStorageDBConnection> mDBConnection;
  nsTArray<nsString> mKeys;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheStorageParent_h
