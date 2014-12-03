/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_PrincipalVerifier_h
#define mozilla_dom_cache_PrincipalVerifier_h

#include "mozilla/ipc/PBackgroundSharedTypes.h"
#include "nsThreadUtils.h"

namespace mozilla {

namespace ipc {
  class PBackgroundParent;
}

namespace dom {
namespace cache {

class ManagerId;

class PrincipalVerifier MOZ_FINAL : public nsRunnable
{
public:
  class Listener
  {
  public:
    virtual ~Listener() { }

    virtual void OnPrincipalVerified(nsresult aRv, ManagerId* aManagerId)=0;
  };

  static nsresult
  Create(Listener* aListener, ipc::PBackgroundParent* aActor,
         const ipc::PrincipalInfo& aPrincipalInfo,
         PrincipalVerifier** aVerifierOut);

  void ClearListener();

private:
  PrincipalVerifier(Listener* aListener, ipc::PBackgroundParent* aActor,
                    const ipc::PrincipalInfo& aPrincipalInfo);
  virtual ~PrincipalVerifier();

  void VerifyOnMainThread();
  void CompleteOnInitiatingThread();

  void DispatchToInitiatingThread(nsresult aRv);

  Listener* mListener;
  nsRefPtr<ContentParent> mActor;
  const ipc::PrincipalInfo mPrincipalInfo;
  nsCOMPtr<nsIThread> mInitiatingThread;
  nsresult mResult;
  nsRefPtr<ManagerId> mManagerId;

public:
  NS_DECL_NSIRUNNABLE
};

} // namesapce cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_PrincipalVerifier_h
