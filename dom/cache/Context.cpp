/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/Context.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/dom/cache/Action.h"
#include "mozilla/dom/cache/ManagerId.h"
#include "mozilla/dom/quota/OriginOrPatternString.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "nsIFile.h"
#include "nsIPrincipal.h"
#include "nsIRunnable.h"
#include "nsThreadUtils.h"

namespace {

using mozilla::dom::Nullable;
using mozilla::dom::cache::QuotaInfo;
using mozilla::dom::quota::OriginOrPatternString;
using mozilla::dom::quota::QuotaManager;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;

class QuotaReleaseRunnable MOZ_FINAL : public nsIRunnable
{
public:
  QuotaReleaseRunnable(const QuotaInfo& aQuotaInfo, const nsACString& aQuotaId)
    : mQuotaInfo(aQuotaInfo)
    , mQuotaId(aQuotaId)
  { }

private:
  ~QuotaReleaseRunnable() { }

  const QuotaInfo mQuotaInfo;
  const nsCString mQuotaId;

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIRUNNABLE
};

NS_IMPL_ISUPPORTS(QuotaReleaseRunnable, nsIRunnable);

NS_IMETHODIMP
QuotaReleaseRunnable::Run()
{
  MOZ_ASSERT(NS_IsMainThread());
  QuotaManager* qm = QuotaManager::Get();
  MOZ_ASSERT(qm);
  qm->AllowNextSynchronizedOp(OriginOrPatternString::FromOrigin(mQuotaInfo.mOrigin),
                              Nullable<PersistenceType>(PERSISTENCE_TYPE_PERSISTENT),
                              mQuotaId);
  return NS_OK;
}

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::DebugOnly;
using mozilla::dom::quota::OriginOrPatternString;
using mozilla::dom::quota::QuotaManager;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;

class Context::QuotaInitRunnable MOZ_FINAL : public nsIRunnable
                                           , public Action::Resolver
{
public:
  QuotaInitRunnable(Context* aContext,
                    ManagerId* aManagerId,
                    const nsACString& aQuotaId,
                    Action* aQuotaIOThreadAction)
    : mContext(aContext)
    , mManagerId(aManagerId)
    , mQuotaId(aQuotaId)
    , mQuotaIOThreadAction(aQuotaIOThreadAction)
    , mInitiatingThread(NS_GetCurrentThread())
    , mState(STATE_INIT)
    , mResult(NS_OK)
  {
    MOZ_ASSERT(mContext);
    MOZ_ASSERT(mManagerId);
    MOZ_ASSERT(mInitiatingThread);
  }

  nsresult Dispatch()
  {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    MOZ_ASSERT(mState == STATE_INIT);

    mState = STATE_CALL_WAIT_FOR_OPEN_ALLOWED;
    nsresult rv = NS_DispatchToMainThread(this, nsIThread::DISPATCH_NORMAL);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mState = STATE_COMPLETE;
      Clear();
    }
    return rv;
  }

  virtual void Resolve(nsresult aRv) MOZ_OVERRIDE
  {
    MOZ_ASSERT(mState == STATE_RUNNING || NS_FAILED(aRv));
    mResult = aRv;
    mState = STATE_COMPLETING;
    nsresult rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
    if (NS_FAILED(rv)) {
      MOZ_CRASH("Failed to dispatch QuotaInitRunnable to initiating thread.");
    }
  }

protected:
  virtual ~QuotaInitRunnable()
  {
    MOZ_ASSERT(mState == STATE_COMPLETE);
    MOZ_ASSERT(!mContext);
    MOZ_ASSERT(!mQuotaIOThreadAction);
  }

private:
  enum State
  {
    STATE_INIT,
    STATE_CALL_WAIT_FOR_OPEN_ALLOWED,
    STATE_WAIT_FOR_OPEN_ALLOWED,
    STATE_ENSURE_ORIGIN_INITIALIZED,
    STATE_RUNNING,
    STATE_COMPLETING,
    STATE_COMPLETE
  };

  void Clear()
  {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    MOZ_ASSERT(mContext);
    mContext = nullptr;
    mQuotaIOThreadAction = nullptr;
  }

  nsRefPtr<Context> mContext;
  nsRefPtr<ManagerId> mManagerId;
  const nsCString mQuotaId;
  nsRefPtr<Action> mQuotaIOThreadAction;
  nsCOMPtr<nsIThread> mInitiatingThread;
  State mState;
  nsresult mResult;
  QuotaInfo mQuotaInfo;

public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE
};

NS_IMPL_ISUPPORTS_INHERITED(mozilla::dom::cache::Context::QuotaInitRunnable,
                            Action::Resolver, nsIRunnable);

NS_IMETHODIMP
Context::QuotaInitRunnable::Run()
{
  switch(mState) {
    case STATE_CALL_WAIT_FOR_OPEN_ALLOWED:
    {
      MOZ_ASSERT(NS_IsMainThread());
      QuotaManager* qm = QuotaManager::GetOrCreate();
      if (!qm) {
        Resolve(NS_ERROR_FAILURE);
        return NS_OK;
      }

      nsCOMPtr<nsIPrincipal> principal = mManagerId->Principal();
      nsresult rv = qm->GetInfoFromPrincipal(principal,
                                             PERSISTENCE_TYPE_PERSISTENT,
                                             &mQuotaInfo.mGroup,
                                             &mQuotaInfo.mOrigin,
                                             &mQuotaInfo.mIsApp,
                                             &mQuotaInfo.mHasUnlimStoragePerm);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        Resolve(rv);
        return NS_OK;
      }

      mState = STATE_WAIT_FOR_OPEN_ALLOWED;
      // TODO: use default storage instead of persistent
      rv = qm->WaitForOpenAllowed(OriginOrPatternString::FromOrigin(mQuotaInfo.mOrigin),
                                  Nullable<PersistenceType>(PERSISTENCE_TYPE_PERSISTENT),
                                  mQuotaId, this);
      if (NS_FAILED(rv)) {
        Resolve(rv);
        return NS_OK;
      }
      break;
    }
    case STATE_WAIT_FOR_OPEN_ALLOWED:
    {
      MOZ_ASSERT(NS_IsMainThread());
      QuotaManager* qm = QuotaManager::Get();
      MOZ_ASSERT(qm);
      mState = STATE_ENSURE_ORIGIN_INITIALIZED;
      nsresult rv = qm->IOThread()->Dispatch(this, nsIThread::DISPATCH_NORMAL);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        Resolve(rv);
        return NS_OK;
      }
      break;
    }
    case STATE_ENSURE_ORIGIN_INITIALIZED:
    {
      // Can't assert quota IO thread because its an idle thread that can get
      // recreated.
      QuotaManager* qm = QuotaManager::Get();
      MOZ_ASSERT(qm);
      nsresult rv = qm->EnsureOriginIsInitialized(PERSISTENCE_TYPE_PERSISTENT,
                                                  mQuotaInfo.mGroup,
                                                  mQuotaInfo.mOrigin,
                                                  mQuotaInfo.mIsApp,
                                                  mQuotaInfo.mHasUnlimStoragePerm,
                                                  getter_AddRefs(mQuotaInfo.mDir));
      if (NS_FAILED(rv)) {
        Resolve(rv);
        return NS_OK;
      }

      mState = STATE_RUNNING;

      if (!mQuotaIOThreadAction) {
        Resolve(NS_OK);
        return NS_OK;
      }

      mQuotaIOThreadAction->RunOnTarget(this, mQuotaInfo);

      break;
    }
    case STATE_COMPLETING:
    {
      NS_ASSERT_OWNINGTHREAD(Action::Resolver);
      if (mQuotaIOThreadAction) {
        mQuotaIOThreadAction->CompleteOnInitiatingThread(mResult);
      }
      mContext->OnQuotaInit(mResult, mQuotaInfo);
      mState = STATE_COMPLETE;
      // Explicitly cleanup here as the destructor could fire on any of
      // the threads we have bounced through.
      Clear();
      break;
    }
    default:
    {
      MOZ_CRASH("unexpected state in QuotaInitRunnable");
      break;
    }
  }

  return NS_OK;
}

class Context::ActionRunnable MOZ_FINAL : public nsIRunnable
                                        , public Action::Resolver
{
public:
  ActionRunnable(Context* aContext, nsIEventTarget* aTarget, Action* aAction,
                 const QuotaInfo& aQuotaInfo)
    : mContext(aContext)
    , mTarget(aTarget)
    , mAction(aAction)
    , mQuotaInfo(aQuotaInfo)
    , mInitiatingThread(NS_GetCurrentThread())
    , mState(STATE_INIT)
    , mCanceled(false)
    , mResult(NS_OK)
  {
    MOZ_ASSERT(mContext);
    MOZ_ASSERT(mTarget);
    MOZ_ASSERT(mAction);
    MOZ_ASSERT(mQuotaInfo.mDir);
    MOZ_ASSERT(mInitiatingThread);
  }

  nsresult Dispatch()
  {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    MOZ_ASSERT(mState == STATE_INIT);

    if (mCanceled) {
      mState = STATE_COMPLETE;
      Clear();
      return NS_OK;
    }

    mState = STATE_RUN_ON_TARGET;
    nsresult rv = mTarget->Dispatch(this, nsIEventTarget::DISPATCH_NORMAL);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mState = STATE_COMPLETE;
      Clear();
    }
    return rv;
  }

  bool MatchesCacheId(CacheId aCacheId) {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    return mAction->MatchesCacheId(aCacheId);
  }

  void Cancel()
  {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    mCanceled = true;
    mResult = NS_ERROR_FAILURE;
    nsresult rv;
    switch(mState) {
      case STATE_RUNNING:
        // Re-dispatch if we are currently running
        rv = mTarget->Dispatch(this, nsIEventTarget::DISPATCH_NORMAL);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          MOZ_CRASH("Failed to dispatch ActionRunnable to target thread.");
        }
        break;
      case STATE_INIT:
      case STATE_RUN_ON_TARGET:
      case STATE_COMPLETING:
      case STATE_COMPLETE:
        break;
      default:
        MOZ_CRASH("unexpected state");
        break;
    }
  }

  virtual void Resolve(nsresult aRv) MOZ_OVERRIDE
  {
    MOZ_ASSERT(mState == STATE_RUNNING);
    mResult = aRv;
    mState = STATE_COMPLETING;
    nsresult rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
    if (NS_FAILED(rv)) {
      MOZ_CRASH("Failed to dispatch ActionRunnable to initiating thread.");
    }
  }

private:
  virtual ~ActionRunnable()
  {
    MOZ_ASSERT(mState == STATE_COMPLETE);
    MOZ_ASSERT(!mContext);
    MOZ_ASSERT(!mAction);
  }

  void Clear()
  {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    MOZ_ASSERT(mContext);
    MOZ_ASSERT(mAction);
    mContext->OnActionRunnableComplete(this);
    mContext = nullptr;
    mAction = nullptr;
  }

  enum State
  {
    STATE_INIT,
    STATE_RUN_ON_TARGET,
    STATE_RUNNING,
    STATE_COMPLETING,
    STATE_COMPLETE
  };

  nsRefPtr<Context> mContext;
  nsCOMPtr<nsIEventTarget> mTarget;
  nsRefPtr<Action> mAction;
  const QuotaInfo mQuotaInfo;
  nsCOMPtr<nsIThread> mInitiatingThread;
  State mState;
  bool mCanceled;
  nsresult mResult;

public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE
};

NS_IMPL_ISUPPORTS_INHERITED(mozilla::dom::cache::Context::ActionRunnable,
                            Action::Resolver, nsIRunnable);

NS_IMETHODIMP
Context::ActionRunnable::Run()
{
  nsresult rv;
  switch(mState) {
    case STATE_RUN_ON_TARGET:
      MOZ_ASSERT(NS_GetCurrentThread() == mTarget);
      if (mCanceled) {
        mState = STATE_COMPLETING;
        rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
        if (NS_FAILED(rv)) {
          MOZ_CRASH("Failed to dispatch ActionRunnable to initiating thread.");
        }
        break;
      }
      mState = STATE_RUNNING;
      mAction->RunOnTarget(this, mQuotaInfo);
      break;
    case STATE_RUNNING:
      MOZ_ASSERT(NS_GetCurrentThread() == mTarget);
      // We only re-enter the RUNNING state if we are canceling.  Normally we
      // should transition out of RUNNING via Resolve() instead.
      MOZ_ASSERT(mCanceled);
      mState = STATE_COMPLETING;
      mAction->CancelOnTarget();
      mResult = NS_FAILED(mResult) ? mResult : NS_ERROR_FAILURE;
      rv = mInitiatingThread->Dispatch(this, nsIThread::DISPATCH_NORMAL);
      if (NS_FAILED(rv)) {
        MOZ_CRASH("Failed to dispatch ActionRunnable to initiating thread.");
      }
      break;
    case STATE_COMPLETING:
      NS_ASSERT_OWNINGTHREAD(Action::Resolver);
      mAction->CompleteOnInitiatingThread(mResult);
      mState = STATE_COMPLETE;
      // Explicitly cleanup here as the destructor could fire on any of
      // the threads we have bounced through.
      Clear();
      break;
    case STATE_COMPLETE:
      // We can end up running in the complete state if we cancel on the origin
      // thread while simultaneously starting to run the action on the target
      // thread.
      MOZ_ASSERT(mCanceled);
      break;
    default:
      MOZ_CRASH("unexpected state in ActionRunnable");
      break;
  }
  return NS_OK;
}

Context::Context(Listener* aListener, ManagerId* aManagerId,
                 Action* aQuotaIOThreadAction)
  : mListener(aListener)
  , mManagerId(aManagerId)
  , mState(STATE_CONTEXT_INIT)
{
  MOZ_ASSERT(mListener);
  MOZ_ASSERT(mManagerId);

  nsRefPtr<QuotaInitRunnable> runnable =
    new QuotaInitRunnable(this, mManagerId, NS_LITERAL_CSTRING("Cache"),
                          aQuotaIOThreadAction);
  nsresult rv = runnable->Dispatch();
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch QuotaInitRunnable.");
  }
}

void
Context::Dispatch(nsIEventTarget* aTarget, Action* aAction)
{
  NS_ASSERT_OWNINGTHREAD(Context);
  MOZ_ASSERT(aTarget);
  MOZ_ASSERT(aAction);

  if (mState == STATE_CONTEXT_CANCELED) {
    return;
  } else if (mState == STATE_CONTEXT_INIT) {
    PendingAction* pending = mPendingActions.AppendElement();
    pending->mTarget = aTarget;
    pending->mAction = aAction;
    return;
  }

  MOZ_ASSERT(STATE_CONTEXT_READY);
  DispatchAction(aTarget, aAction);
}

void
Context::CancelAll()
{
  NS_ASSERT_OWNINGTHREAD(Context);
  mState = STATE_CONTEXT_CANCELED;
  mPendingActions.Clear();
  for (uint32_t i = 0; i < mActionRunnables.Length(); ++i) {
    mActionRunnables[i]->Cancel();
  }
}

void
Context::CancelForCacheId(CacheId aCacheId)
{
  NS_ASSERT_OWNINGTHREAD(Context);
  for (uint32_t i = 0; i < mPendingActions.Length(); ++i) {
    if (mPendingActions[i].mAction->MatchesCacheId(aCacheId)) {
      mPendingActions.RemoveElementAt(i);
    }
  }
  for (uint32_t i = 0; i < mActionRunnables.Length(); ++i) {
    if (mActionRunnables[i]->MatchesCacheId(aCacheId)) {
      mActionRunnables[i]->Cancel();
    }
  }
}

Context::~Context()
{
  NS_ASSERT_OWNINGTHREAD(Context);
  MOZ_ASSERT(mListener);

  // Unlock the quota dir as we go out of scope.
  nsCOMPtr<nsIRunnable> runnable =
    new QuotaReleaseRunnable(mQuotaInfo, NS_LITERAL_CSTRING("Cache"));
  nsresult rv = NS_DispatchToMainThread(runnable, nsIThread::DISPATCH_NORMAL);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch QuotaReleaseRunnable to main thread.");
  }

  mListener->RemoveContext(this);
}

void
Context::DispatchAction(nsIEventTarget* aTarget, Action* aAction)
{
  NS_ASSERT_OWNINGTHREAD(Context);

  nsRefPtr<ActionRunnable> runnable =
    new ActionRunnable(this, aTarget, aAction, mQuotaInfo);
  mActionRunnables.AppendElement(runnable);
  nsresult rv = runnable->Dispatch();
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch ActionRunnable to target thread.");
  }
}

void
Context::OnQuotaInit(nsresult aRv, const QuotaInfo& aQuotaInfo)
{
  NS_ASSERT_OWNINGTHREAD(Context);
  MOZ_ASSERT(mState == STATE_CONTEXT_INIT);

  if (NS_FAILED(aRv)) {
    for (uint32_t i = 0; i < mPendingActions.Length(); ++i) {
      mPendingActions[i].mAction->CompleteOnInitiatingThread(aRv);
    }
    mPendingActions.Clear();
    // Context will destruct after return here and last ref is released.
    return;
  }

  mQuotaInfo = aQuotaInfo;
  mState = STATE_CONTEXT_READY;

  for (uint32_t i = 0; i < mPendingActions.Length(); ++i) {
    DispatchAction(mPendingActions[i].mTarget, mPendingActions[i].mAction);
  }
  mPendingActions.Clear();
}

void
Context::OnActionRunnableComplete(ActionRunnable* aActionRunnable)
{
  NS_ASSERT_OWNINGTHREAD(Context);
  MOZ_ASSERT(aActionRunnable);
  for (uint32_t i = 0; i < mActionRunnables.Length(); ++i) {
    if (aActionRunnable == mActionRunnables[i]) {
      mActionRunnables.RemoveElementAt(i);
      return;
    }
  }
  MOZ_ASSERT(false);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
