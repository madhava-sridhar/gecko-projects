/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_QuotaClient_h
#define mozilla_dom_cache_QuotaClient_h

#include "mozilla/Attributes.h"
#include "mozilla/dom/quota/Client.h"

namespace mozilla {
namespace dom {
namespace cache {

class QuotaClient MOZ_FINAL : public quota::Client
{
public:
  static quota::Client*
  Create();

  // quota::Client methods
  Type GetType() MOZ_OVERRIDE;

  nsresult InitOrigin(quota::PersistenceType aPersistenceType,
                      const nsACString& aGroup, const nsACString& aOrigin,
                      quota::UsageInfo* aUsageInfo) MOZ_OVERRIDE;

  nsresult GetUsageForOrigin(quota::PersistenceType aPersistenceType,
                             const nsACString& aGroup,
                             const nsACString& aOrigin,
                             quota::UsageInfo* aUsageInfo) MOZ_OVERRIDE;

  void OnOriginClearCompleted(quota::PersistenceType aPersistenceType,
                              const nsACString& aOrigin) MOZ_OVERRIDE;

  void ReleaseIOThreadObjects() MOZ_OVERRIDE;

  bool IsFileServiceUtilized() MOZ_OVERRIDE;

  bool IsTransactionServiceActivated() MOZ_OVERRIDE;

  void WaitForStoragesToComplete(nsTArray<nsIOfflineStorage*>& aStorages,
                                 nsIRunnable* aCallback) MOZ_OVERRIDE;

  void ShutdownTransactionService() MOZ_OVERRIDE;

private:
  QuotaClient();
  ~QuotaClient();

public:
  NS_INLINE_DECL_REFCOUNTING(mozilla::dom::cache::QuotaClient)
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_QuotaClient_h
