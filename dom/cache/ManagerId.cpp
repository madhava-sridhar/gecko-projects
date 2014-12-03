/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/ManagerId.h"
#include "nsIPrincipal.h"
#include "nsRefPtr.h"
#include "nsThreadUtils.h"

namespace {

class ReleasePrincipalRunnable : public nsRunnable
{
public:
  ReleasePrincipalRunnable(already_AddRefed<nsIPrincipal> aPrincipal)
    : mPrincipal(aPrincipal)
  { }

  NS_IMETHOD Run() MOZ_OVERRIDE
  {
    MOZ_ASSERT(NS_IsMainThread());
    mPrincipal = nullptr;
    return NS_OK;
  }

private:
  ~ReleasePrincipalRunnable() { }

  nsCOMPtr<nsIPrincipal> mPrincipal;
};

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

// static
nsresult
ManagerId::Create(nsIPrincipal* aPrincipal, ManagerId** aManagerIdOut)
{
  MOZ_ASSERT(NS_IsMainThread());

  // The QuotaManager::GetInfoFromPrincipal() has special logic for system
  // and about: principals.  We currently don't need the system principal logic
  // because ManagerId only uses the origin for in memory comparisons.  We
  // also don't do any special logic to host the same Cache for different about:
  // pages, so we don't need those checks either.

  nsAutoCString origin;
  nsresult rv = aPrincipal->GetOrigin(getter_Copies(origin));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  uint32_t appId;
  rv = aPrincipal->GetAppId(&appId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool inBrowserElement;
  rv = aPrincipal->GetIsInBrowserElement(&inBrowserElement);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsRefPtr<ManagerId> ref = new ManagerId(aPrincipal, origin, appId,
                                          inBrowserElement);
  ref.forget(aManagerIdOut);

  return NS_OK;
}

already_AddRefed<nsIPrincipal>
ManagerId::Principal() const
{
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsIPrincipal> ref = mPrincipal;
  return ref.forget();
}

ManagerId::ManagerId(nsIPrincipal* aPrincipal, const nsACString& aOrigin,
                     uint32_t aAppId, bool aInBrowserElement)
    : mPrincipal(aPrincipal)
    , mOrigin(aOrigin)
    , mAppId(aAppId)
    , mInBrowserElement(aInBrowserElement)
{
  MOZ_ASSERT(mPrincipal);
}

ManagerId::~ManagerId()
{
  if (NS_IsMainThread()) {
    return;
  }

  nsCOMPtr<nsIRunnable> runnable =
    new ReleasePrincipalRunnable(mPrincipal.forget());

  nsresult rv = NS_DispatchToMainThread(runnable);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch ManagerId release runnable.");
  }
}

} // namespace cache
} // namespace dom
} // namespace mozilla
