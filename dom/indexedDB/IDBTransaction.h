/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_idbtransaction_h__
#define mozilla_dom_indexeddb_idbtransaction_h__

#include "mozilla/dom/IDBTransactionBinding.h"
#include "mozilla/dom/indexedDB/IDBWrapperCache.h"
#include "nsAutoPtr.h"
#include "nsClassHashtable.h"
#include "nsCOMPtr.h"
#include "nsCycleCollectionParticipant.h"
#include "nsHashKeys.h"
#include "nsInterfaceHashtable.h"
#include "nsIRunnable.h"
#include "nsRefPtrHashtable.h"
#include "nsString.h"
#include "nsTArray.h"

class mozIStorageConnection;
class mozIStorageStatement;
class nsPIDOMWindow;

namespace mozilla {

class ErrorResult;
class EventChainPreVisitor;

namespace dom {

class DOMError;
class DOMStringList;

namespace indexedDB {

class AsyncConnectionHelper;
class BackgroundTransactionChild;
class BackgroundVersionChangeTransactionChild;
class CommitHelper;
class DatabaseInfo;
class FileInfo;
class IDBDatabase;
class IDBObjectStore;
class IDBRequest;
class IDBTransaction;
class IndexedDBDatabaseChild;
class IndexedDBTransactionChild;
class IndexedDBTransactionParent;
class IndexMetadata;
struct ObjectStoreInfo;
class ObjectStoreSpec;
class UpdateRefcountFunction;

class IDBTransactionListener
{
public:
  NS_IMETHOD_(nsrefcnt) AddRef() = 0;
  NS_IMETHOD_(nsrefcnt) Release() = 0;

  // Called just before dispatching the final events on the transaction.
  virtual nsresult
  NotifyTransactionPreComplete(IDBTransaction* aTransaction) = 0;
  // Called just after dispatching the final events on the transaction.
  virtual nsresult
  NotifyTransactionPostComplete(IDBTransaction* aTransaction) = 0;
};

class IDBTransaction MOZ_FINAL
  : public IDBWrapperCache
  , public nsIRunnable
{
  friend class AsyncConnectionHelper;
  friend class CommitHelper;
  friend class IndexedDBDatabaseChild;
  friend class ThreadObserver;

public:
  enum Mode
  {
    READ_ONLY = 0,
    READ_WRITE,
    VERSION_CHANGE,

    // Only needed for IPC serialization helper, should never be used in code.
    MODE_INVALID
  };

  enum ReadyState
  {
    INITIAL = 0,
    LOADING,
    COMMITTING,
    DONE
  };

private:
  nsRefPtr<IDBDatabase> mDatabase;
  nsRefPtr<DatabaseInfo> mDatabaseInfo;
  nsRefPtr<DOMError> mError;
  nsTArray<nsString> mObjectStoreNames;
  ReadyState mReadyState;
  Mode mMode;
  uint32_t mPendingRequests;

  nsInterfaceHashtable<nsCStringHashKey, mozIStorageStatement>
    mCachedStatements;

  nsRefPtr<IDBTransactionListener> mListener;

  // Only touched on the database thread.
  nsCOMPtr<mozIStorageConnection> mConnection;

  // Only touched on the database thread.
  uint32_t mSavepointCount;

  nsTArray<nsRefPtr<IDBObjectStore>> mObjectStores;

  nsRefPtr<UpdateRefcountFunction> mUpdateFileRefcountFunction;
  nsRefPtrHashtable<nsISupportsHashKey, FileInfo> mCreatedFileInfos;

  IndexedDBTransactionChild* mActorChild;
  IndexedDBTransactionParent* mActorParent;

  // Tagged with mMode. If mMode is VERSION_CHANGE then mBackgroundActor will be
  // a BackgroundVersionChangeTransactionChild. Otherwise it will be a
  // BackgroundTransactionChild.
  union {
    BackgroundTransactionChild* mNormalBackgroundActor;
    BackgroundVersionChangeTransactionChild* mVersionChangeBackgroundActor;
  } mBackgroundActor;

  nsresult mAbortCode;

  // Only used for VERSION_CHANGE transactions.
  int64_t mNextObjectStoreId;
  int64_t mNextIndexId;

#ifdef MOZ_ENABLE_PROFILER_SPS
  uint64_t mSerialNumber;
#endif
  bool mCreating;

#ifdef DEBUG
  bool mFiredCompleteOrAbort;
#endif

public:
  NS_DECL_ISUPPORTS_INHERITED
  NS_DECL_NSIRUNNABLE

  NS_DECL_CYCLE_COLLECTION_CLASS_INHERITED(IDBTransaction, IDBWrapperCache)

  static already_AddRefed<IDBTransaction>
  CreateVersionChange(IDBDatabase* aDatabase,
                      BackgroundVersionChangeTransactionChild* aActor,
                      int64_t aNextObjectStoreId,
                      int64_t aNextIndexId);

  static already_AddRefed<IDBTransaction>
  Create(IDBDatabase* aDatabase,
         const nsTArray<nsString>& aObjectStoreNames,
         Mode aMode);

  static IDBTransaction*
  GetCurrent();

  void
  AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  void
  SetBackgroundActor(BackgroundTransactionChild* aBackgroundActor);

  void
  ClearBackgroundActor()
  {
    AssertIsOnOwningThread();

    if (mMode == VERSION_CHANGE) {
      mBackgroundActor.mVersionChangeBackgroundActor = nullptr;
    } else {
      mBackgroundActor.mNormalBackgroundActor = nullptr;
    }
  }

  void
  RefreshSpec();

  // nsIDOMEventTarget
  virtual nsresult PreHandleEvent(EventChainPreVisitor& aVisitor) MOZ_OVERRIDE;

  void OnNewRequest();
  void OnRequestFinished();
  void OnRequestDisconnected();

  void SetTransactionListener(IDBTransactionListener* aListener);

  bool StartSavepoint();
  nsresult ReleaseSavepoint();
  void RollbackSavepoint();

  // Only meant to be called on mStorageThread!
  nsresult GetOrCreateConnection(mozIStorageConnection** aConnection);

  already_AddRefed<mozIStorageStatement>
  GetCachedStatement(const nsACString& aQuery);

  template<int N>
  already_AddRefed<mozIStorageStatement>
  GetCachedStatement(const char (&aQuery)[N])
  {
    return GetCachedStatement(NS_LITERAL_CSTRING(aQuery));
  }

  bool IsOpen() const;

  bool IsFinished() const
  {
    return mReadyState > LOADING;
  }

  bool IsWriteAllowed() const
  {
    return mMode == READ_WRITE || mMode == VERSION_CHANGE;
  }

  bool IsAborted() const
  {
    return NS_FAILED(mAbortCode);
  }

  // 'Get' prefix is to avoid name collisions with the enum
  Mode GetMode()
  {
    return mMode;
  }

  uint64_t Id() const
  {
    // XXX Remove me!
    return 0;
  }

  IDBDatabase* Database()
  {
    NS_ASSERTION(mDatabase, "This should never be null!");
    return mDatabase;
  }

  DatabaseInfo* DBInfo() const
  {
    return mDatabaseInfo;
  }

  const nsTArray<nsString>&
  ObjectStoreNamesInternal() const
  {
    return mObjectStoreNames;
  }

  already_AddRefed<IDBObjectStore>
  GetOrCreateObjectStore(const nsAString& aName,
                         ObjectStoreInfo* aObjectStoreInfo,
                         bool aCreating);

  already_AddRefed<IDBObjectStore>
  CreateObjectStore(const ObjectStoreSpec& aSpec);

  void
  DeleteObjectStore(int64_t aObjectStoreId);

  void
  CreateIndex(IDBObjectStore* aObjectStore, const IndexMetadata& aMetadata);

  void
  DeleteIndex(IDBObjectStore* aObjectStore, int64_t aIndexId);

  already_AddRefed<FileInfo> GetFileInfo(nsIDOMBlob* aBlob);
  void AddFileInfo(nsIDOMBlob* aBlob, FileInfo* aFileInfo);

  void ClearCreatedFileInfos();

  void
  SetActor(IndexedDBTransactionChild* aActorChild)
  {
    NS_ASSERTION(!aActorChild || !mActorChild, "Shouldn't have more than one!");
    mActorChild = aActorChild;
  }

  void
  SetActor(IndexedDBTransactionParent* aActorParent)
  {
    NS_ASSERTION(!aActorParent || !mActorParent,
                 "Shouldn't have more than one!");
    mActorParent = aActorParent;
  }

  IndexedDBTransactionChild*
  GetActorChild() const
  {
    return mActorChild;
  }

  IndexedDBTransactionParent*
  GetActorParent() const
  {
    return mActorParent;
  }

  nsresult
  Abort(IDBRequest* aRequest);

  nsresult
  Abort(nsresult aAbortCode);

  nsresult
  GetAbortCode() const
  {
    return mAbortCode;
  }

#ifdef MOZ_ENABLE_PROFILER_SPS
  uint32_t
  GetSerialNumber() const
  {
    return mSerialNumber;
  }
#endif

  // nsWrapperCache
  virtual JSObject*
  WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope) MOZ_OVERRIDE;

  // WebIDL
  nsPIDOMWindow*
  GetParentObject() const;

  IDBTransactionMode
  GetMode(ErrorResult& aRv) const;

  IDBDatabase*
  Db() const
  {
    NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
    return mDatabase;
  }

  DOMError*
  GetError(ErrorResult& aRv);

  already_AddRefed<IDBObjectStore>
  ObjectStore(const nsAString& aName, ErrorResult& aRv);

  void
  Abort(ErrorResult& aRv)
  {
    NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
    aRv = AbortInternal(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR, nullptr);
  }

  IMPL_EVENT_HANDLER(abort)
  IMPL_EVENT_HANDLER(complete)
  IMPL_EVENT_HANDLER(error)

  already_AddRefed<DOMStringList>
  ObjectStoreNames();

  void
  FireCompleteOrAbortEvents(nsresult aResult);

  // Only for VERSION_CHANGE transactions.
  int64_t
  NextObjectStoreId();

  // Only for VERSION_CHANGE transactions.
  int64_t
  NextIndexId();

private:
  nsresult
  AbortInternal(nsresult aAbortCode,
                already_AddRefed<mozilla::dom::DOMError> aError);

  IDBTransaction(IDBDatabase* aDatabase);
  ~IDBTransaction();

  nsresult CommitOrRollback();

  void SendCommit();
  void SendAbort(nsresult aResultCode);
};

class CommitHelper MOZ_FINAL : public nsIRunnable
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIRUNNABLE

  CommitHelper(IDBTransaction* aTransaction,
               IDBTransactionListener* aListener,
               const nsTArray<nsRefPtr<IDBObjectStore> >& mUpdatedObjectStores);
  CommitHelper(IDBTransaction* aTransaction,
               nsresult aAbortCode);
  ~CommitHelper();

  template<class T>
  bool AddDoomedObject(nsCOMPtr<T>& aCOMPtr)
  {
    if (aCOMPtr) {
      if (!mDoomedObjects.AppendElement(do_QueryInterface(aCOMPtr))) {
        NS_ERROR("Out of memory!");
        return false;
      }
      aCOMPtr = nullptr;
    }
    return true;
  }

private:
  // Writes new autoincrement counts to database
  nsresult WriteAutoIncrementCounts();

  // Updates counts after a successful commit
  void CommitAutoIncrementCounts();

  // Reverts counts when a transaction is aborted
  void RevertAutoIncrementCounts();

  nsRefPtr<IDBTransaction> mTransaction;
  nsRefPtr<IDBTransactionListener> mListener;
  nsCOMPtr<mozIStorageConnection> mConnection;
  nsRefPtr<UpdateRefcountFunction> mUpdateFileRefcountFunction;
  nsAutoTArray<nsCOMPtr<nsISupports>, 10> mDoomedObjects;
  nsAutoTArray<nsRefPtr<IDBObjectStore>, 10> mAutoIncrementObjectStores;

  nsresult mAbortCode;
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_idbtransaction_h__
