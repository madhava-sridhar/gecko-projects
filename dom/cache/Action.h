/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_Action_h
#define mozilla_dom_cache_Action_h

#include "mozilla/dom/cache/Types.h"
#include "nsISupportsImpl.h"

class nsIFile;

namespace mozilla {
namespace dom {
namespace cache {

class Action : public nsISupports
{
protected:
  virtual ~Action() { }

public:
  class Resolver : public nsISupports
  {
  protected:
    virtual ~Resolver() { }

  public:

    // Note: Action must drop Resolver ref after calling Resolve()!
    // Note: Must be called on Action's target thread.
    virtual void Resolve(nsresult aRv)=0;

    NS_DECL_THREADSAFE_ISUPPORTS
  };

  // Execute operations on target thread. Once complete call
  // Resolver::Resolve().  This can be done sync or async.
  // Note: Action should hold Resolver ref until its ready to call Resolve().
  virtual void RunOnTarget(Resolver* aResolver, const QuotaInfo& aQuotaInfo)=0;

  // Called on target thread if the Action is being canceled.  Simply
  // clean up and do not call Resolver::Resolve() in this case.
  // Note: Action must drop Resolver ref if CancelOnTarget() is called!
  virtual void CancelOnTarget() { }

  // Executed on the initiating thread and is passed the nsresult given to
  // Resolver::Resolve().
  virtual void CompleteOnInitiatingThread(nsresult aRv) { }

  // Executed on the initiating thread.  If this Action will operate on the
  // given cache ID then override this to return true.
  virtual bool MatchesCacheId(CacheId aCacheId) { return false; }

  NS_DECL_THREADSAFE_ISUPPORTS
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_Action_h
