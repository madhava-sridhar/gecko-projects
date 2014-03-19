/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_actorsparent_h__
#define mozilla_dom_indexeddb_actorsparent_h__

#include "mozilla/Attributes.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryParent.h"
#include "mozilla/dom/quota/StoragePrivilege.h"
#include "nsISupportsImpl.h"
#include "nsString.h"

namespace mozilla {
namespace ipc {

class BackgroundParentImpl;

} // namespace ipc

namespace dom {
namespace indexedDB {

class BackgroundFactoryParent MOZ_FINAL
  : public PBackgroundIDBFactoryParent
{
  friend class mozilla::ipc::BackgroundParentImpl;

  typedef mozilla::dom::quota::StoragePrivilege StoragePrivilege;

  // Counts the number of "live" BackgroundFactoryParent instances that have not
  // yet had ActorDestroy called.
  static uint64_t sFactoryInstanceCount;

  nsCString mGroup;
  nsCString mOrigin;
  StoragePrivilege mPrivilege;

#ifdef DEBUG
  bool mActorDestroyed;
#endif

public:
  const nsCString&
  Group() const
  {
    return mGroup;
  }

  const nsCString&
  Origin() const
  {
    return mOrigin;
  }

  StoragePrivilege
  Privilege() const
  {
    return mPrivilege;
  }

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(BackgroundFactoryParent)

private:
  // Only constructed in Create().
  BackgroundFactoryParent(const nsCString& aGroup,
                          const nsCString& aOrigin,
                          const StoragePrivilege& aPrivilege);

  // Reference counted.
  ~BackgroundFactoryParent();

  // Only created by mozilla::ipc::BackgroundParentImpl.
  static already_AddRefed<BackgroundFactoryParent>
  Create(const nsCString& aGroup,
         const nsCString& aOrigin,
         const StoragePrivilege& aPrivilege);

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual bool
  RecvDeleteMe() MOZ_OVERRIDE;

  virtual PBackgroundIDBFactoryRequestParent*
  AllocPBackgroundIDBFactoryRequestParent(const FactoryRequestParams& aParams)
                                          MOZ_OVERRIDE;

  virtual bool
  RecvPBackgroundIDBFactoryRequestConstructor(
                                     PBackgroundIDBFactoryRequestParent* aActor,
                                     const FactoryRequestParams& aParams)
                                     MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBFactoryRequestParent(
                                     PBackgroundIDBFactoryRequestParent* aActor)
                                     MOZ_OVERRIDE;

  virtual PBackgroundIDBDatabaseParent*
  AllocPBackgroundIDBDatabaseParent(
                                   const DatabaseSpec& aSpec,
                                   PBackgroundIDBFactoryRequestParent* aRequest)
                                   MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBDatabaseParent(PBackgroundIDBDatabaseParent* aActor)
                                      MOZ_OVERRIDE;
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_actorsparent_h__
