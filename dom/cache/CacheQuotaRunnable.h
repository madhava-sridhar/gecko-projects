/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_CacheQuotaRunnable_h
#define mozilla_dom_CacheQuotaRunnable_h

#include "nsIRunnable.h"
#include "nsISupportsImpl.h"

class nsIThread;

namespace mozilla {
namespace dom {

class CacheQuotaRunnable : public nsIRunnable
{
public:
  CacheQuotaRunnable(const nsACString& aOrigin,
                     const nsACString& aBaseDomain,
                     const nsACString& aQuotaId);

  nsresult Dispatch();

protected:
  virtual ~CacheQuotaRunnable();
  virtual void RunOnQuotaIOThread(const nsACString& aOrigin,
                                  const nsACString& aBaseDomain,
                                  nsIFile* aQuotaDir)=0;
  virtual void CompleteOnInitiatingThread(nsresult aRv)=0;

private:
  enum State
  {
    STATE_INIT,
    STATE_CALL_WAIT_FOR_OPEN_ALLOWED,
    STATE_WAIT_FOR_OPEN_ALLOWED,
    STATE_ENSURE_ORIGIN_INITIALIZED,
    STATE_ALLOW_NEXT_SYNCHRONIZED_OP,
    STATE_COMPLETING,
    STATE_COMPLETE
  };

  void DispatchError(nsresult aRv);

  const nsCString mOrigin;
  const nsCString mBaseDomain;
  const nsCString mQuotaId;
  nsCOMPtr<nsIThread> mInitiatingThread;
  State mState;
  nsresult mResult;

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIRUNNABLE
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_CacheQuotaRunnable_h
