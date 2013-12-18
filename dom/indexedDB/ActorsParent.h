/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_actorsparent_h__
#define mozilla_dom_indexeddb_actorsparent_h__

#include "mozilla/dom/indexedDB/IndexedDatabase.h"

#include "mozilla/ipc/PBackground.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryParent.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryRequestParent.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBDatabaseParent.h"
#include "mozilla/dom/quota/StoragePrivilege.h"

namespace mozilla {
namespace ipc {

class BackgroundParentImpl;

} // namespace ipc
} // namespace mozilla

BEGIN_INDEXEDDB_NAMESPACE

class BackgroundFactoryParent MOZ_FINAL : public PBackgroundIDBFactoryParent
{
  friend class mozilla::ipc::BackgroundParentImpl;

  typedef mozilla::dom::quota::StoragePrivilege StoragePrivilege;

  nsCString mGroup;
  nsCString mASCIIOrigin;
  StoragePrivilege mPrivilege;

  // Counts the number of "live" BackgroundFactoryParent instances that have not
  // yet had ActorDestroy called.
  static uint64_t sFactoryInstanceCount;

private:
  // Only created by mozilla::ipc::BackgroundParentImpl.
  static BackgroundFactoryParent*
  Create(const nsCString& aGroup,
         const nsCString& aASCIIOrigin,
         const StoragePrivilege& aPrivilege);

  // Only constructed in Create().
  BackgroundFactoryParent(const nsCString& aGroup,
                          const nsCString& aASCIIOrigin,
                          const StoragePrivilege& aPrivilege);

  // Only destroyed by mozilla::ipc::BackgroundChildImpl.
  ~BackgroundFactoryParent();

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

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
  AllocPBackgroundIDBDatabaseParent(const DatabaseMetadata& aMetadata)
                                    MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBDatabaseParent(PBackgroundIDBDatabaseParent* aActor)
                                      MOZ_OVERRIDE;
};

class BackgroundDatabaseParent MOZ_FINAL : public PBackgroundIDBDatabaseParent
{
  friend class BackgroundFactoryParent;

  DatabaseMetadata mMetadata;

private:
  // Only constructed by BackgroundFactoryParent.
  BackgroundDatabaseParent(const DatabaseMetadata& aMetadata);

  // Only destroyed by BackgroundFactoryParent.
  ~BackgroundDatabaseParent();

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;
};

END_INDEXEDDB_NAMESPACE

#endif // mozilla_dom_indexeddb_actorsparent_h__
