/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_actorschild_h__
#define mozilla_dom_indexeddb_actorschild_h__

#include "js/RootingAPI.h"
#include "mozilla/Attributes.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBCursorChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBDatabaseChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryRequestChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBRequestChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBTransactionChild.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBVersionChangeTransactionChild.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsTArrayForwardDeclare.h"

class nsIEventTarget;

namespace mozilla {
namespace ipc {

class BackgroundChildImpl;

} // namespace ipc

namespace dom {
namespace indexedDB {

class IDBCursor;
class IDBDatabase;
class IDBFactory;
class IDBOpenDBRequest;
class IDBRequest;
class IDBTransaction;

class Key;
class SerializedStructuredCloneReadInfo;

class BackgroundFactoryChild MOZ_FINAL
  : public PBackgroundIDBFactoryChild
{
  friend class mozilla::ipc::BackgroundChildImpl;
  friend class IDBFactory;

  IDBFactory* mFactory;

#ifdef DEBUG
  nsCOMPtr<nsIEventTarget> mOwningThread;
#endif

public:
  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  IDBFactory*
  GetDOMObject() const
  {
    AssertIsOnOwningThread();
    return mFactory;
  }

private:
  // Only created by IDBFactory.
  BackgroundFactoryChild(IDBFactory* aFactory);

  // Only destroyed by mozilla::ipc::BackgroundChildImpl.
  ~BackgroundFactoryChild();

  void
  SendDeleteMeInternal();

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
  AllocPBackgroundIDBDatabaseChild(const DatabaseSpec& aSpec,
                                   PBackgroundIDBFactoryRequestChild* aRequest)
                                   MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBDatabaseChild(PBackgroundIDBDatabaseChild* aActor)
                                     MOZ_OVERRIDE;

  bool
  SendDeleteMe() MOZ_DELETE;
};

class BackgroundDatabaseChild;

class BackgroundRequestChildBase
{
protected:
  nsRefPtr<IDBRequest> mRequest;

public:
  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  IDBRequest*
  GetDOMObject() const
  {
    AssertIsOnOwningThread();
    return mRequest;
  }

protected:
  BackgroundRequestChildBase(IDBRequest* aRequest);

  virtual
  ~BackgroundRequestChildBase();
};

class BackgroundFactoryRequestChild MOZ_FINAL
  : public BackgroundRequestChildBase
  , public PBackgroundIDBFactoryRequestChild
{
  typedef mozilla::dom::quota::PersistenceType PersistenceType;

  friend class IDBFactory;
  friend class BackgroundFactoryChild;
  friend class BackgroundDatabaseChild;

  nsRefPtr<IDBFactory> mFactory;
  const uint64_t mRequestedVersion;
  const PersistenceType mPersistenceType;

public:
  IDBOpenDBRequest*
  GetOpenDBRequest() const;

private:
  // Only created by IDBFactory.
  BackgroundFactoryRequestChild(IDBFactory* aFactory,
                                IDBOpenDBRequest* aOpenRequest,
                                uint64_t aRequestedVersion,
                                PersistenceType aPersistenceType);

  // Only destroyed by BackgroundFactoryChild.
  ~BackgroundFactoryRequestChild();

  bool
  HandleResponse(nsresult aResponse);

  bool
  HandleResponse(const OpenDatabaseRequestResponse& aResponse);

  bool
  HandleResponse(const DeleteDatabaseRequestResponse& aResponse);

  // IPDL methods are only called by IPDL.
  virtual bool
  Recv__delete__(const FactoryRequestResponse& aResponse) MOZ_OVERRIDE;

  virtual bool
  RecvBlocked(const uint64_t& aCurrentVersion) MOZ_OVERRIDE;
};

class BackgroundDatabaseChild MOZ_FINAL
  : public PBackgroundIDBDatabaseChild
{
  typedef mozilla::dom::quota::PersistenceType PersistenceType;

  friend class BackgroundFactoryChild;
  friend class BackgroundFactoryRequestChild;
  friend class IDBDatabase;

  nsAutoPtr<DatabaseSpec> mSpec;
  nsRefPtr<IDBDatabase> mTemporaryStrongDatabase;
  BackgroundFactoryRequestChild* mOpenRequestActor;
  IDBDatabase* mDatabase;

  PersistenceType mPersistenceType;

public:
  void
  AssertIsOnOwningThread() const
  {
    static_cast<BackgroundFactoryChild*>(Manager())->AssertIsOnOwningThread();
  }

  const DatabaseSpec*
  Spec() const
  {
    AssertIsOnOwningThread();
    return mSpec;
  }

  IDBDatabase*
  GetDOMObject() const
  {
    AssertIsOnOwningThread();
    return mDatabase;
  }

private:
  // Only constructed by BackgroundFactoryChild.
  BackgroundDatabaseChild(const DatabaseSpec& aSpec,
                          BackgroundFactoryRequestChild* aOpenRequest);

  // Only destroyed by BackgroundFactoryChild.
  ~BackgroundDatabaseChild();

  void
  SendDeleteMeInternal();

  bool
  EnsureDOMObject();

  void
  ReleaseDOMObject();

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual PBackgroundIDBTransactionChild*
  AllocPBackgroundIDBTransactionChild(
                                     const nsTArray<nsString>& aObjectStoreNames,
                                     const Mode& aMode)
                                     MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBTransactionChild(PBackgroundIDBTransactionChild* aActor)
                                        MOZ_OVERRIDE;

  virtual PBackgroundIDBVersionChangeTransactionChild*
  AllocPBackgroundIDBVersionChangeTransactionChild(
                                              const uint64_t& aCurrentVersion,
                                              const uint64_t& aRequestedVersion,
                                              const int64_t& aNextObjectStoreId,
                                              const int64_t& aNextIndexId)
                                              MOZ_OVERRIDE;

  virtual bool
  RecvPBackgroundIDBVersionChangeTransactionConstructor(
                            PBackgroundIDBVersionChangeTransactionChild* aActor,
                            const uint64_t& aCurrentVersion,
                            const uint64_t& aRequestedVersion,
                            const int64_t& aNextObjectStoreId,
                            const int64_t& aNextIndexId)
                            MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBVersionChangeTransactionChild(
                            PBackgroundIDBVersionChangeTransactionChild* aActor)
                            MOZ_OVERRIDE;

  virtual bool
  RecvVersionChange(const uint64_t& aOldVersion, const uint64_t& aNewVersion)
                    MOZ_OVERRIDE;

  virtual bool
  RecvInvalidate() MOZ_OVERRIDE;

  bool
  SendDeleteMe() MOZ_DELETE;
};

class BackgroundVersionChangeTransactionChild;

class BackgroundTransactionBase
{
  friend class BackgroundVersionChangeTransactionChild;

  // mTemporaryStrongTransaction is strong and is only valid until the end of
  // NoteComplete() member function or until the NoteActorDestroyed() member
  // function is called.
  nsRefPtr<IDBTransaction> mTemporaryStrongTransaction;

protected:
  // mTransaction is weak and is valid until the NoteActorDestroyed() member
  // function is called.
  IDBTransaction* mTransaction;

public:
#ifdef DEBUG
  virtual void
  AssertIsOnOwningThread() const = 0;
#else
  void
  AssertIsOnOwningThread() const
  { }
#endif

  IDBTransaction*
  GetDOMObject() const
  {
    AssertIsOnOwningThread();
    return mTransaction;
  }

protected:
  BackgroundTransactionBase();
  BackgroundTransactionBase(IDBTransaction* aTransaction);

  virtual
  ~BackgroundTransactionBase();

  void
  NoteActorDestroyed();

  void
  NoteComplete();

private:
  // Only called by BackgroundVersionChangeTransactionChild.
  void
  SetDOMTransaction(IDBTransaction* aDOMObject);
};

class BackgroundTransactionChild MOZ_FINAL
  : public BackgroundTransactionBase
  , public PBackgroundIDBTransactionChild
{
  friend class BackgroundDatabaseChild;
  friend class IDBDatabase;

public:
#ifdef DEBUG
  virtual void
  AssertIsOnOwningThread() const;
#endif

  void
  SendDeleteMeInternal();

private:
  // Only created by IDBDatabase.
  BackgroundTransactionChild(IDBTransaction* aTransaction);

  // Only destroyed by BackgroundDatabaseChild.
  ~BackgroundTransactionChild();

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  bool
  RecvComplete(const nsresult& aResult) MOZ_OVERRIDE;

  virtual PBackgroundIDBRequestChild*
  AllocPBackgroundIDBRequestChild(const RequestParams& aParams) MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBRequestChild(PBackgroundIDBRequestChild* aActor)
                                    MOZ_OVERRIDE;

  virtual PBackgroundIDBCursorChild*
  AllocPBackgroundIDBCursorChild(const OpenCursorParams& aParams) MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBCursorChild(PBackgroundIDBCursorChild* aActor)
                                   MOZ_OVERRIDE;

  bool
  SendDeleteMe() MOZ_DELETE;
};

class BackgroundVersionChangeTransactionChild MOZ_FINAL
  : public BackgroundTransactionBase
  , public PBackgroundIDBVersionChangeTransactionChild
{
  friend class BackgroundDatabaseChild;

  IDBOpenDBRequest* mOpenDBRequest;

public:
#ifdef DEBUG
  virtual void
  AssertIsOnOwningThread() const;
#endif

  void
  SendDeleteMeInternal();

private:
  // Only created by BackgroundDatabaseChild.
  BackgroundVersionChangeTransactionChild(IDBOpenDBRequest* aOpenDBRequest);

  // Only destroyed by BackgroundDatabaseChild.
  ~BackgroundVersionChangeTransactionChild();

  // Only called by BackgroundDatabaseChild.
  void
  SetDOMTransaction(IDBTransaction* aDOMObject)
  {
    BackgroundTransactionBase::SetDOMTransaction(aDOMObject);
  }

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  bool
  RecvComplete(const nsresult& aResult) MOZ_OVERRIDE;

  virtual PBackgroundIDBRequestChild*
  AllocPBackgroundIDBRequestChild(const RequestParams& aParams) MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBRequestChild(PBackgroundIDBRequestChild* aActor)
                                    MOZ_OVERRIDE;

  virtual PBackgroundIDBCursorChild*
  AllocPBackgroundIDBCursorChild(const OpenCursorParams& aParams) MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBCursorChild(PBackgroundIDBCursorChild* aActor)
                                   MOZ_OVERRIDE;

  bool
  SendDeleteMe() MOZ_DELETE;
};

class BackgroundTransactionRequestChildBase
  : public BackgroundRequestChildBase
{
protected:
  nsRefPtr<IDBTransaction> mTransaction;

protected:
  BackgroundTransactionRequestChildBase(IDBRequest* aRequest);

  virtual
  ~BackgroundTransactionRequestChildBase();
};

class BackgroundRequestChild MOZ_FINAL
  : public BackgroundTransactionRequestChildBase
  , public PBackgroundIDBRequestChild
{
  friend class BackgroundTransactionChild;
  friend class BackgroundVersionChangeTransactionChild;

public:
  BackgroundRequestChild(IDBRequest* aRequest);

private:
  // Only destroyed by BackgroundTransactionChild or
  // BackgroundVersionChangeTransactionChild.
  ~BackgroundRequestChild();

  bool
  HandleResponse(nsresult aResponse);

  bool
  HandleResponse(const Key& aResponse);

  bool
  HandleResponse(const nsTArray<Key>& aResponse);

  bool
  HandleResponse(const SerializedStructuredCloneReadInfo& aResponse);

  bool
  HandleResponse(const nsTArray<SerializedStructuredCloneReadInfo>& aResponse);

  bool
  HandleResponse(JS::Handle<JS::Value> aResponse);

  bool
  HandleResponse(uint64_t aResponse);

  // IPDL methods are only called by IPDL.
  virtual bool
  Recv__delete__(const RequestResponse& aResponse) MOZ_OVERRIDE;
};

class BackgroundCursorChild MOZ_FINAL
  : public BackgroundTransactionRequestChildBase
  , public PBackgroundIDBCursorChild
{
  friend class BackgroundTransactionChild;
  friend class BackgroundVersionChangeTransactionChild;

  IDBObjectStore* mObjectStore;
  IDBIndex* mIndex;
  IDBCursor* mCursor;

  // Only set while a request is in progress.
  nsRefPtr<IDBCursor> mStrongCursor;

  Direction mDirection;

public:
  BackgroundCursorChild(IDBRequest* aRequest,
                        IDBObjectStore* aObjectStore,
                        Direction aDirection);

  BackgroundCursorChild(IDBRequest* aRequest,
                        IDBIndex* aIndex,
                        Direction aDirection);

  void
  SendContinueInternal(const CursorRequestParams& aParams);

  void
  SendDeleteMeInternal();

private:
  // Only destroyed by BackgroundTransactionChild or
  // BackgroundVersionChangeTransactionChild.
  ~BackgroundCursorChild();

  void
  HandleResponse(const ObjectStoreCursorResponse& aResponse);

  void
  HandleResponse(const ObjectStoreKeyCursorResponse& aResponse);

  void
  HandleResponse(const IndexCursorResponse& aResponse);

  void
  HandleResponse(const IndexKeyCursorResponse& aResponse);

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual bool
  RecvResponse(const CursorResponse& aResponse) MOZ_OVERRIDE;

  // Force callers to use SendContinueInternal.
  bool
  SendContinue(const CursorRequestParams& aParams) MOZ_DELETE;

  bool
  SendDeleteMe() MOZ_DELETE;
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_actorschild_h__
