/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_actorschild_h__
#define mozilla_dom_indexeddb_actorschild_h__

#include "mozilla/dom/indexedDB/IndexedDatabase.h"

#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryRequestChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBDatabaseChild.h"

class nsIEventTarget;
template <class> class nsCOMPtr;

namespace mozilla {
namespace ipc {

class BackgroundChildImpl;

} // namespace ipc
} // namespace mozilla

BEGIN_INDEXEDDB_NAMESPACE

class IDBFactory;
class IDBOpenDBRequest;
class IDBRequest;

class BackgroundRequestChildBase
{
protected:
  nsRefPtr<IDBRequest> mRequest;

protected:
  BackgroundRequestChildBase(IDBRequest* aRequest);

  virtual
  ~BackgroundRequestChildBase();
};

class BackgroundFactoryChild MOZ_FINAL : public PBackgroundIDBFactoryChild
{
  friend class mozilla::ipc::BackgroundChildImpl;
  friend class IDBFactory;

  IDBFactory* mFactory;

#ifdef DEBUG
  nsCOMPtr<nsIEventTarget> mOwningThread;
#endif

private:
  // Only created by IDBFactory.
  BackgroundFactoryChild(IDBFactory* aFactory);

  // Only destroyed by mozilla::ipc::BackgroundChildImpl.
  ~BackgroundFactoryChild();

public:
  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

private:
  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual PBackgroundIDBFactoryRequestChild*
  AllocPBackgroundIDBFactoryRequestChild(const FactoryRequestParams& aParams)
                                         MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBFactoryRequestChild(
                                      PBackgroundIDBFactoryRequestChild* aActor)
                                      MOZ_OVERRIDE;

  virtual PBackgroundIDBDatabaseChild*
  AllocPBackgroundIDBDatabaseChild(const DatabaseMetadata& aMetadata)
                                   MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBDatabaseChild(PBackgroundIDBDatabaseChild* aActor)
                                     MOZ_OVERRIDE;
};

class BackgroundFactoryRequestChild MOZ_FINAL :
                                        public BackgroundRequestChildBase,
                                        public PBackgroundIDBFactoryRequestChild
{
  friend class IDBFactory;
  friend class BackgroundFactoryChild;

  nsRefPtr<IDBFactory> mFactory;
  nsCString mDatabaseId;

private:
  // Only created by IDBFactory.
  BackgroundFactoryRequestChild(IDBFactory* aFactory,
                                IDBOpenDBRequest* aOpenRequest,
                                const nsACString& aDatabaseId);

  // Only destroyed by BackgroundFactoryChild.
  ~BackgroundFactoryRequestChild();

public:
  void
  AssertIsOnOwningThread() const
  {
    static_cast<BackgroundFactoryChild*>(Manager())->AssertIsOnOwningThread();
  }

private:
  // IPDL methods are only called by IPDL.
  virtual bool
  Recv__delete__(const FactoryRequestResponse& aResponse) MOZ_OVERRIDE;

  virtual bool
  RecvBlocked(const uint64_t& aCurrentVersion) MOZ_OVERRIDE;
};

class BackgroundDatabaseChild MOZ_FINAL : public PBackgroundIDBDatabaseChild
{
  friend class BackgroundFactoryChild;

  DatabaseMetadata mMetadata;

private:
  // Only constructed by BackgroundFactoryChild.
  BackgroundDatabaseChild(const DatabaseMetadata& aMetadata);

  // Only destroyed by BackgroundFactoryChild.
  ~BackgroundDatabaseChild();

public:
  void
  AssertIsOnOwningThread() const
  {
    static_cast<BackgroundFactoryChild*>(Manager())->AssertIsOnOwningThread();
  }

private:
  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;
};

END_INDEXEDDB_NAMESPACE

#endif // mozilla_dom_indexeddb_actorschild_h__
