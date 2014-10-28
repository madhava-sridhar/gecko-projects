/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_Context_h
#define mozilla_dom_cache_Context_h

#include "mozilla/dom/cache/Types.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsISupportsImpl.h"
#include "nsString.h"
#include "nsTArray.h"

class nsIEventTarget;
class nsIFile;

namespace mozilla {
namespace dom {
namespace cache {

class Action;

class Context MOZ_FINAL
{
public:
  class Listener
  {
  protected:
    virtual ~Listener() { }
  public:
    // Called from the Context destructor on the thread that originally
    // created the Context.
    virtual void RemoveContext(Context* aContext)=0;

    NS_INLINE_DECL_REFCOUNTING(mozilla::dom::cache::Context::Listener)
  };

  Context(Listener* aListener, const nsACString& aOrigin,
          const nsACString& aBaseDomain, Action* aQuotaIOThreadAction);

  // Execute given action on the target once the quota manager has been
  // initialized.
  //
  // Only callable from the thread that created the Context.
  void Dispatch(nsIEventTarget* aTarget, Action* aAction);

  // Cancel any Actions running or waiting to run.  This should allow the
  // Context to be released and Listener::RemoveContext() will be called
  // when complete.
  //
  // Only callable from the thread that created the Context.
  void CancelAll();

  // Cancel any Actions running or waiting to run that operate on the given
  // cache ID.
  //
  // Only callable from the thread that created the Context.
  void CancelForCacheId(CacheId aCacheId);

private:
  class QuotaInitRunnable;
  class ActionRunnable;

  enum State
  {
    STATE_CONTEXT_INIT,
    STATE_CONTEXT_READY,
    STATE_CONTEXT_CANCELED
  };

  struct PendingAction
  {
    nsCOMPtr<nsIEventTarget> mTarget;
    nsRefPtr<Action> mAction;
  };

  ~Context();
  void DispatchAction(nsIEventTarget* aTarget, Action* aAction);
  void OnQuotaInit(nsresult aRv, nsIFile* aQuotaDir);
  void OnActionRunnableComplete(ActionRunnable* const aAction);

  nsRefPtr<Listener> mListener;
  const nsCString mOrigin;
  State mState;
  nsCOMPtr<nsIFile> mQuotaDir;
  nsTArray<PendingAction> mPendingActions;

  // weak refs since ~ActionRunnable() removes itself from this list
  nsTArray<ActionRunnable*> mActionRunnables;

public:
  NS_INLINE_DECL_REFCOUNTING(mozilla::dom::cache::Context)
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_Context_h
