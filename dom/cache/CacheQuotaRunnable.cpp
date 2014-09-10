/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheQuotaRunnable.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/dom/quota/OriginOrPatternString.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace dom {

using mozilla::dom::quota::OriginOrPatternString;
using mozilla::dom::quota::QuotaManager;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;

NS_IMPL_ISUPPORTS(CacheQuotaRunnable, nsIRunnable);

CacheQuotaRunnable::CacheQuotaRunnable(const nsACString& aOrigin,
                                       const nsACString& aBaseDomain,
                                       const nsACString& aQuotaId)
  : mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mQuotaId(aQuotaId)
  , mInitiatingThread(NS_GetCurrentThread())
  , mState(STATE_INIT)
  , mResult(NS_OK)
{
  MOZ_ASSERT(mInitiatingThread);
}

nsresult
CacheQuotaRunnable::Dispatch()
{
  MOZ_ASSERT(mState == STATE_INIT);
  MOZ_ASSERT(NS_GetCurrentThread() == mInitiatingThread);

  mState = STATE_CALL_WAIT_FOR_OPEN_ALLOWED;
  nsresult rv = NS_DispatchToMainThread(this, nsIThread::DISPATCH_NORMAL);
  if (NS_FAILED(rv)) {
    mState = STATE_COMPLETE;
  }
  return rv;
}

NS_IMETHODIMP
CacheQuotaRunnable::Run()
{
  QuotaManager* qm;
  nsresult rv;
  nsCOMPtr<nsIFile> quotaDir;

  switch(mState) {
    case STATE_CALL_WAIT_FOR_OPEN_ALLOWED:
      MOZ_ASSERT(NS_IsMainThread());
      qm = QuotaManager::GetOrCreate();
      if (!qm) {
        DispatchError(NS_ERROR_FAILURE);
        return NS_OK;
      }
      mState = STATE_WAIT_FOR_OPEN_ALLOWED;
      rv = qm->WaitForOpenAllowed(OriginOrPatternString::FromOrigin(mOrigin),
                                  Nullable<PersistenceType>(PERSISTENCE_TYPE_PERSISTENT),
                                  mQuotaId, this);
      if (NS_FAILED(rv)) {
        DispatchError(rv);
        return NS_OK;
      }
      break;
    case STATE_WAIT_FOR_OPEN_ALLOWED:
      qm = QuotaManager::Get();
      MOZ_ASSERT(qm);
      mState = STATE_ENSURE_ORIGIN_INITIALIZED;
      rv = qm->IOThread()->Dispatch(this, nsIThread::DISPATCH_NORMAL);
      if (NS_FAILED(rv)) {
        DispatchError(rv);
        return NS_OK;
      }
      break;
    case STATE_ENSURE_ORIGIN_INITIALIZED:
      // TODO: MOZ_ASSERT(NS_GetCurrentThread() == QuotaManager::Get()->IOThread());
      qm = QuotaManager::Get();
      MOZ_ASSERT(qm);
      rv = qm->EnsureOriginIsInitialized(PERSISTENCE_TYPE_PERSISTENT,
                                         mBaseDomain,
                                         mOrigin,
                                         true, // aTrackQuota
                                         getter_AddRefs(quotaDir));
      if (NS_FAILED(rv)) {
        DispatchError(rv);
        return NS_OK;
      }
      RunOnQuotaIOThread(mOrigin, mBaseDomain, quotaDir);
      mState = STATE_ALLOW_NEXT_SYNCHRONIZED_OP;
      rv = NS_DispatchToMainThread(this, nsIThread::DISPATCH_NORMAL);
      if (NS_FAILED(rv)) {
        DispatchError(rv);
        return NS_OK;
      }
      break;
    case STATE_ALLOW_NEXT_SYNCHRONIZED_OP:
      MOZ_ASSERT(NS_IsMainThread());
      qm = QuotaManager::Get();
      MOZ_ASSERT(qm);
      qm->AllowNextSynchronizedOp(OriginOrPatternString::FromOrigin(mOrigin),
                                  Nullable<PersistenceType>(PERSISTENCE_TYPE_PERSISTENT),
                                  mQuotaId);

      mState = STATE_COMPLETING;
      rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
      break;
    case STATE_COMPLETING:
      MOZ_ASSERT(NS_GetCurrentThread() == mInitiatingThread);
      CompleteOnInitiatingThread(mResult);
      mState = STATE_COMPLETE;
      break;
    default:
      break;
  }

  return NS_OK;
}

CacheQuotaRunnable::~CacheQuotaRunnable()
{
  MOZ_ASSERT(mState == STATE_COMPLETE);
}

void
CacheQuotaRunnable::DispatchError(nsresult aRv)
{
  mResult = aRv;
  mState = STATE_COMPLETE;
  DebugOnly<nsresult> rv;
  rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
  MOZ_ASSERT(NS_SUCCEEDED(rv));
}

} // namespace dom
} // namespace mozilla
