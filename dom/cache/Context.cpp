/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/Context.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/dom/cache/Action.h"
#include "mozilla/dom/quota/OriginOrPatternString.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "nsIFile.h"
#include "nsIRunnable.h"
#include "nsThreadUtils.h"

namespace {

using mozilla::dom::Nullable;
using mozilla::dom::quota::OriginOrPatternString;
using mozilla::dom::quota::QuotaManager;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;

class QuotaReleaseRunnable MOZ_FINAL : public nsIRunnable
{
public:
  QuotaReleaseRunnable(const nsACString& aOrigin, const nsACString& aQuotaId)
    : mOrigin(aOrigin)
    , mQuotaId(aQuotaId)
  {
  }

private:
  ~QuotaReleaseRunnable()
  {}

  const nsCString mOrigin;
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
  qm->AllowNextSynchronizedOp(OriginOrPatternString::FromOrigin(mOrigin),
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
                    const nsACString& aOrigin,
                    const nsACString& aBaseDomain,
                    const nsACString& aQuotaId,
                    Action* aQuotaIOThreadAction)
    : mContext(aContext)
    , mOrigin(aOrigin)
    , mBaseDomain(aBaseDomain)
    , mQuotaId(aQuotaId)
    , mQuotaIOThreadAction(aQuotaIOThreadAction)
    , mInitiatingThread(NS_GetCurrentThread())
    , mState(STATE_INIT)
    , mResult(NS_OK)
  {
    MOZ_ASSERT(mContext);
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
  const nsCString mOrigin;
  const nsCString mBaseDomain;
  const nsCString mQuotaId;
  nsRefPtr<Action> mQuotaIOThreadAction;
  nsCOMPtr<nsIThread> mInitiatingThread;
  State mState;
  nsresult mResult;
  nsCOMPtr<nsIFile> mQuotaDir;

public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE
};

NS_IMPL_ISUPPORTS_INHERITED(mozilla::dom::cache::Context::QuotaInitRunnable,
                            Action::Resolver, nsIRunnable);

NS_IMETHODIMP
Context::QuotaInitRunnable::Run()
{
  QuotaManager* qm;
  nsresult rv;

  switch(mState) {
    case STATE_CALL_WAIT_FOR_OPEN_ALLOWED:
      MOZ_ASSERT(NS_IsMainThread());
      qm = QuotaManager::GetOrCreate();
      if (!qm) {
        Resolve(NS_ERROR_FAILURE);
        return NS_OK;
      }
      mState = STATE_WAIT_FOR_OPEN_ALLOWED;
      rv = qm->WaitForOpenAllowed(OriginOrPatternString::FromOrigin(mOrigin),
                                  Nullable<PersistenceType>(PERSISTENCE_TYPE_PERSISTENT),
                                  mQuotaId, this);
      if (NS_FAILED(rv)) {
        Resolve(rv);
        return NS_OK;
      }
      break;
    case STATE_WAIT_FOR_OPEN_ALLOWED:
      MOZ_ASSERT(NS_IsMainThread());
      qm = QuotaManager::Get();
      MOZ_ASSERT(qm);
      mState = STATE_ENSURE_ORIGIN_INITIALIZED;
      rv = qm->IOThread()->Dispatch(this, nsIThread::DISPATCH_NORMAL);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        Resolve(rv);
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
                                         getter_AddRefs(mQuotaDir));
      if (NS_FAILED(rv)) {
        Resolve(rv);
        return NS_OK;
      }
      mState = STATE_RUNNING;
      if (mQuotaIOThreadAction) {
        nsCOMPtr<nsIFile> quotaDir;
        rv = mQuotaDir->Clone(getter_AddRefs(quotaDir));
        if (NS_FAILED(rv)) {
          Resolve(rv);
          return NS_OK;
        }
        mQuotaIOThreadAction->RunOnTarget(this, quotaDir);
      } else {
        Resolve(NS_OK);
      }
      break;
    case STATE_COMPLETING:
      NS_ASSERT_OWNINGTHREAD(Action::Resolver);
      if (mQuotaIOThreadAction) {
        mQuotaIOThreadAction->CompleteOnInitiatingThread(mResult);
      }
      mContext->OnQuotaInit(mResult, mQuotaDir);
      mState = STATE_COMPLETE;
      // Explicitly cleanup here as the destructor could fire on any of
      // the threads we have bounced through.
      Clear();
      break;
    default:
      MOZ_CRASH("unexpected state in QuotaInitRunnable");
      break;
  }

  return NS_OK;
}

class Context::ActionRunnable MOZ_FINAL : public nsIRunnable
                                        , public Action::Resolver
{
public:
  ActionRunnable(Context* aContext, nsIEventTarget* aTarget, Action* aAction,
                 nsIFile* aQuotaDir)
    : mContext(aContext)
    , mTarget(aTarget)
    , mAction(aAction)
    , mQuotaDir(aQuotaDir)
    , mInitiatingThread(NS_GetCurrentThread())
    , mState(STATE_INIT)
    , mResult(NS_OK)
  {
    MOZ_ASSERT(mContext);
    MOZ_ASSERT(mTarget);
    MOZ_ASSERT(mAction);
    MOZ_ASSERT(mQuotaDir);
    MOZ_ASSERT(mInitiatingThread);
  }

  nsresult Dispatch()
  {
    NS_ASSERT_OWNINGTHREAD(Action::Resolver);
    MOZ_ASSERT(mState == STATE_INIT);

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
    nsresult rv;
    switch(mState) {
      case STATE_INIT:
        mState = STATE_COMPLETE;
        break;
      case STATE_RUN_ON_TARGET:
        mState = STATE_CANCELING;
        break;
      case STATE_RUNNING:
        mState = STATE_CANCELING;
        rv = mTarget->Dispatch(this, nsIEventTarget::DISPATCH_NORMAL);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          MOZ_CRASH("Failed to dispatch ActionRunnable to target thread.");
        }
      case STATE_CANCELING:
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
    STATE_CANCELING,
    STATE_RUNNING,
    STATE_COMPLETING,
    STATE_COMPLETE
  };

  nsRefPtr<Context> mContext;
  nsCOMPtr<nsIEventTarget> mTarget;
  nsRefPtr<Action> mAction;
  nsCOMPtr<nsIFile> mQuotaDir;
  nsCOMPtr<nsIThread> mInitiatingThread;
  State mState;
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
      mState = STATE_RUNNING;
      mAction->RunOnTarget(this, mQuotaDir);
      break;
    case STATE_CANCELING:
      MOZ_ASSERT(NS_GetCurrentThread() == mTarget);
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
    default:
      MOZ_CRASH("unexpected state in ActionRunnable");
      break;
  }
  return NS_OK;
}

Context::Context(Listener* aListener, const nsACString& aOrigin,
                 const nsACString& aBaseDomain, Action* aQuotaIOThreadAction)
  : mListener(aListener)
  , mOrigin(aOrigin)
  , mState(STATE_CONTEXT_INIT)
{
  MOZ_ASSERT(mListener);

  nsRefPtr<QuotaInitRunnable> runnable =
    new QuotaInitRunnable(this, aOrigin, aBaseDomain,
                          NS_LITERAL_CSTRING("Cache"), aQuotaIOThreadAction);
  nsresult rv = runnable->Dispatch();
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch QuotaInitRunnable.");
  }
}

void
Context::ClearListener()
{
  NS_ASSERT_OWNINGTHREAD(Context);
  MOZ_ASSERT(mListener);
  mListener = nullptr;
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

  // Unlock the quota dir as we go out of scope.
  nsCOMPtr<nsIRunnable> runnable =
    new QuotaReleaseRunnable(mOrigin, NS_LITERAL_CSTRING("Cache"));
  nsresult rv = NS_DispatchToMainThread(runnable, nsIThread::DISPATCH_NORMAL);
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch QuotaReleaseRunnable to main thread.");
  }

  if (mListener) {
    mListener->RemoveContext(this);
  }
}

void
Context::DispatchAction(nsIEventTarget* aTarget, Action* aAction)
{
  NS_ASSERT_OWNINGTHREAD(Context);

  nsCOMPtr<nsIFile> quotaDir;
  nsresult rv = mQuotaDir->Clone(getter_AddRefs(quotaDir));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    aAction->CompleteOnInitiatingThread(rv);
    return;
  }

  nsRefPtr<ActionRunnable> runnable =
    new ActionRunnable(this, aTarget, aAction, quotaDir);
  mActionRunnables.AppendElement(runnable);
  rv = runnable->Dispatch();
  if (NS_FAILED(rv)) {
    MOZ_CRASH("Failed to dispatch ActionRunnable to target thread.");
  }
}

void
Context::OnQuotaInit(nsresult aRv, nsIFile* aQuotaDir)
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

  mQuotaDir = aQuotaDir;
  MOZ_ASSERT(mQuotaDir);
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
