/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbdatabase_h__
#define mozilla_dom_indexeddb_idbdatabase_h__

#include "mozilla/Attributes.h"
#include "mozilla/dom/IDBTransactionBinding.h"
#include "mozilla/dom/indexedDB/IDBWrapperCache.h"
#include "mozilla/dom/quota/PersistenceType.h"
#include "nsAutoPtr.h"
#include "nsHashKeys.h"
#include "nsIFileStorage.h"
#include "nsIOfflineStorage.h"
#include "nsString.h"
#include "nsTHashtable.h"

class nsIDocument;
class nsIScriptContext;
class nsPIDOMWindow;

namespace mozilla {

class ErrorResult;
class EventChainPostVisitor;

namespace dom {

class ContentParent;
class DOMStringList;
class IDBObjectStoreParameters;
template <typename> class Sequence;

namespace quota {

class Client;

} // namespace quota

namespace indexedDB {

class AsyncConnectionHelper;
class BackgroundDatabaseChild;
class DatabaseSpec;
class FileManager;
class IDBFactory;
class IDBIndex;
class IDBObjectStore;
class IDBRequest;
class IDBTransaction;
class IndexedDatabaseManager;
struct ObjectStoreInfoGuts;

class IDBDatabase MOZ_FINAL
  : public IDBWrapperCache
  , public nsIOfflineStorage
{
  typedef mozilla::dom::quota::PersistenceType PersistenceType;
  typedef mozilla::dom::quota::Client QuotaClient;

  friend class AsyncConnectionHelper;
  friend class IndexedDatabaseManager;
  friend class IndexedDBDatabaseParent;
  friend class IndexedDBDatabaseChild;

  // The factory must be kept alive when IndexedDB is used in multiple
  // processes. If it dies then the entire actor tree will be destroyed with it
  // and the world will explode.
  nsRefPtr<IDBFactory> mFactory;

  nsAutoPtr<DatabaseSpec> mSpec;

  // Normally null except during a versionchange transaction.
  nsAutoPtr<DatabaseSpec> mPreviousSpec;

  nsRefPtr<FileManager> mFileManager;

  BackgroundDatabaseChild* mBackgroundActor;

  nsRefPtr<QuotaClient> mQuotaClient;

  nsTHashtable<nsPtrHashKey<IDBTransaction>> mTransactions;

  bool mClosed;
  bool mInvalidated;

public:
  static already_AddRefed<IDBDatabase>
  Create(IDBWrapperCache* aOwnerCache,
         IDBFactory* aFactory,
         BackgroundDatabaseChild* aActor,
         DatabaseSpec* aSpec,
         PersistenceType aPersistenceType);

  static IDBDatabase*
  FromStorage(nsIOfflineStorage* aStorage);

  static IDBDatabase*
  FromStorage(nsIFileStorage* aStorage);

  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  const nsString&
  Name() const;

  void
  GetName(nsAString& aName) const
  {
    AssertIsOnOwningThread();

    aName = Name();
  }

  uint64_t
  Version() const;

  already_AddRefed<nsIDocument>
  GetOwnerDocument() const;

  void
  CloseInternal();

  void
  EnterSetVersionTransaction(uint64_t aNewVersion);

  void
  ExitSetVersionTransaction();

  // Called when a versionchange transaction is aborted to reset the
  // DatabaseInfo.
  void
  RevertToPreviousState();

  IDBFactory*
  Factory() const
  {
    AssertIsOnOwningThread();

    return mFactory;
  }

  void
  RegisterTransaction(IDBTransaction* aTransaction);

  void
  UnregisterTransaction(IDBTransaction* aTransaction);

  void
  AbortTransactions();

  nsPIDOMWindow*
  GetParentObject() const;

  already_AddRefed<DOMStringList>
  ObjectStoreNames() const;

  already_AddRefed<IDBObjectStore>
  CreateObjectStore(JSContext* aCx,
                    const nsAString& aName,
                    const IDBObjectStoreParameters& aOptionalParameters,
                    ErrorResult& aRv);

  void
  DeleteObjectStore(const nsAString& name, ErrorResult& aRv);

  already_AddRefed<IDBTransaction>
  Transaction(const nsAString& aStoreName,
              IDBTransactionMode aMode,
              ErrorResult& aRv);

  already_AddRefed<IDBTransaction>
  Transaction(const Sequence<nsString>& aStoreNames,
              IDBTransactionMode aMode,
              ErrorResult& aRv);

  IMPL_EVENT_HANDLER(abort)
  IMPL_EVENT_HANDLER(error)
  IMPL_EVENT_HANDLER(versionchange)

  StorageType
  Storage() const
  {
    return PersistenceTypeToStorage(mPersistenceType);
  }

  already_AddRefed<IDBRequest>
  MozCreateFileHandle(const nsAString& aName,
                      const Optional<nsAString>& aType,
                      ErrorResult& aRv);

  void
  ClearBackgroundActor()
  {
    AssertIsOnOwningThread();

    mBackgroundActor = nullptr;
  }

  const DatabaseSpec*
  Spec() const
  {
    return mSpec;
  }

  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIFILESTORAGE
  NS_DECL_NSIOFFLINESTORAGE
  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(IDBDatabase, IDBWrapperCache)

  // nsIDOMEventTarget
  virtual void
  LastRelease() MOZ_OVERRIDE;

  virtual nsresult
  PostHandleEvent(EventChainPostVisitor& aVisitor) MOZ_OVERRIDE;

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx) MOZ_OVERRIDE;

private:
  IDBDatabase(IDBWrapperCache* aOwnerCache,
              IDBFactory* aFactory,
              BackgroundDatabaseChild* aActor,
              DatabaseSpec* aSpec,
              PersistenceType aPersistenceType);

  ~IDBDatabase();

  bool
  RunningVersionChangeTransaction() const
  {
    AssertIsOnOwningThread();

    return !!mPreviousSpec;
  }

  void
  RefreshSpec(bool aMayDelete);
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbdatabase_h__
