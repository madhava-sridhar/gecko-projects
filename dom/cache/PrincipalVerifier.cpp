/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/PrincipalVerifier.h"

#include "mozilla/AppProcessChecker.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/cache/ManagerId.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "nsIPrincipal.h"

namespace {

using mozilla::dom::ContentParent;

class ReleaseContentParentRunnable : public nsRunnable
{
public:
  ReleaseContentParentRunnable(already_AddRefed<ContentParent> aActor)
    : mActor(aActor)
  { }

  NS_IMETHOD Run() MOZ_OVERRIDE
  {
    MOZ_ASSERT(NS_IsMainThread());
    mActor = nullptr;
    return NS_OK;
  }

private:
  ~ReleaseContentParentRunnable() { }

  nsRefPtr<ContentParent> mActor;
};

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::ipc::BackgroundParent;
using mozilla::ipc::PBackgroundParent;
using mozilla::ipc::PrincipalInfo;
using mozilla::ipc::PrincipalInfoToPrincipal;

// static
nsresult
PrincipalVerifier::Create(Listener* aListener, PBackgroundParent* aActor,
                          const PrincipalInfo& aPrincipalInfo,
                          PrincipalVerifier** aVerifierOut)
{
  nsRefPtr<PrincipalVerifier> verifier = new PrincipalVerifier(aListener,
                                                               aActor,
                                                               aPrincipalInfo);

  nsresult rv = NS_DispatchToMainThread(verifier);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  verifier.forget(aVerifierOut);

  return NS_OK;
}

void
PrincipalVerifier::ClearListener()
{
  MOZ_ASSERT(NS_GetCurrentThread() == mInitiatingThread);
  MOZ_ASSERT(mListener);
  mListener = nullptr;
}

PrincipalVerifier::PrincipalVerifier(Listener* aListener,
                                     PBackgroundParent* aActor,
                                     const PrincipalInfo& aPrincipalInfo)
  : mListener(aListener)
  , mActor(BackgroundParent::GetContentParent(aActor))
  , mPrincipalInfo(aPrincipalInfo)
  , mInitiatingThread(NS_GetCurrentThread())
  , mResult(NS_OK)
{
  MOZ_ASSERT(mListener);
  MOZ_ASSERT(mInitiatingThread);
}

PrincipalVerifier::~PrincipalVerifier()
{
  MOZ_ASSERT(!mListener);

  if (!mActor || NS_IsMainThread()) {
    return;
  }

  nsCOMPtr<nsIRunnable> runnable =
    new ReleaseContentParentRunnable(mActor.forget());

  nsresult rv = NS_DispatchToMainThread(runnable);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch ManagerId release runnable.");
  }
}

NS_IMETHODIMP
PrincipalVerifier::Run()
{
  if (NS_IsMainThread()) {
    VerifyOnMainThread();
    return NS_OK;
  }
  CompleteOnInitiatingThread();
  return NS_OK;
}

void
PrincipalVerifier::VerifyOnMainThread()
{
  MOZ_ASSERT(NS_IsMainThread());

  nsresult rv;
  nsRefPtr<nsIPrincipal> principal = PrincipalInfoToPrincipal(mPrincipalInfo,
                                                              &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    DispatchToInitiatingThread(rv);
    return;
  }

  if (NS_WARN_IF(mActor && !AssertAppPrincipal(mActor, principal))) {
    DispatchToInitiatingThread(NS_ERROR_FAILURE);
    return;
  }
  mActor = nullptr;

  rv = ManagerId::Create(principal, getter_AddRefs(mManagerId));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    DispatchToInitiatingThread(rv);
    return;
  }

  DispatchToInitiatingThread(NS_OK);
}

void
PrincipalVerifier::CompleteOnInitiatingThread()
{
  MOZ_ASSERT(NS_GetCurrentThread() == mInitiatingThread);

  if (!mListener) {
    return;
  }
  mListener->OnPrincipalVerified(mResult, mManagerId);
}

void
PrincipalVerifier::DispatchToInitiatingThread(nsresult aRv)
{
  mResult = aRv;
  nsresult rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch PrincipalVerifier to initiating thread.");
  }
}

} // namesapce cache
} // namespace dom
} // namespace mozilla
