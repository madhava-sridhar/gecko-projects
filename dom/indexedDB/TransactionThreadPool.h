/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_transactionthreadpool_h__
#define mozilla_dom_indexeddb_transactionthreadpool_h__

#include "mozilla/Attributes.h"
#include "nsClassHashtable.h"
#include "nsCOMPtr.h"
#include "nsHashKeys.h"
#include "nsTArray.h"

template <typename> class nsAutoPtr;
class nsIEventTarget;
class nsIRunnable;
class nsIThreadPool;
struct PRThread;

namespace mozilla {
namespace dom {
namespace indexedDB {

class TransactionThreadPool MOZ_FINAL
{
  friend class nsAutoPtr<TransactionThreadPool>;

  class FinishTransactionRunnable;
  friend class FinishTransactionRunnable;

  class TransactionQueue;
  friend class TransactionQueue;

  class CleanupRunnable;
  struct DatabaseTransactionInfo;
  struct DatabasesCompleteCallback;
  struct TransactionInfo;
  struct TransactionInfoPair;

  nsCOMPtr<nsIThreadPool> mThreadPool;

  nsClassHashtable<nsCStringHashKey, DatabaseTransactionInfo>
    mTransactionsInProgress;

  nsTArray<DatabasesCompleteCallback> mCompleteCallbacks;

#ifdef DEBUG
  PRThread* mDEBUGOwningPRThread;
#endif

public:
  class FinishCallback;

  // returns a non-owning ref!
  static TransactionThreadPool* GetOrCreate();

  // returns a non-owning ref!
  static TransactionThreadPool* Get();

  static void Shutdown();

  static uint64_t NextTransactionId();

  nsresult Dispatch(uint64_t aTransactionId,
                    const nsACString& aDatabaseId,
                    const nsTArray<nsString>& aObjectStoreNames,
                    uint16_t aMode,
                    nsIRunnable* aRunnable,
                    bool aFinish,
                    FinishCallback* aFinishCallback);

  void Dispatch(uint64_t aTransactionId,
                const nsACString& aDatabaseId,
                nsIRunnable* aRunnable,
                bool aFinish,
                FinishCallback* aFinishCallback);

  void WaitForDatabasesToComplete(nsTArray<nsCString>& aDatabaseIds,
                                  nsIRunnable* aCallback);

  // Returns true if there are running or pending transactions for aDatabase.
  bool HasTransactionsForDatabase(const nsACString& aDatabaseId);

  void AssertIsOnOwningThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

private:
  static PLDHashOperator
  CollectTransactions(const uint64_t& aTransactionId,
                      TransactionInfo* aValue,
                      void* aUserArg);

  static PLDHashOperator
  FindTransaction(const uint64_t& aTransactionId,
                  TransactionInfo* aValue,
                  void* aUserArg);

  static PLDHashOperator
  MaybeUnblockTransaction(nsPtrHashKey<TransactionInfo>* aKey,
                          void* aUserArg);

  TransactionThreadPool();
  ~TransactionThreadPool();

  nsresult Init();
  nsresult Cleanup();

  void FinishTransaction(uint64_t aTransactionId,
                         const nsACString& aDatabaseId,
                         const nsTArray<nsString>& aObjectStoreNames,
                         uint16_t aMode);

  TransactionQueue* GetQueueForTransaction(uint64_t aTransactionId,
                                           const nsACString& aDatabaseId);

  TransactionQueue& GetQueueForTransaction(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode);

  bool MaybeFireCallback(DatabasesCompleteCallback aCallback);
};

class NS_NO_VTABLE TransactionThreadPool::FinishCallback
{
public:
  NS_IMETHOD_(MozExternalRefCountType)
  AddRef() = 0;

  NS_IMETHOD_(MozExternalRefCountType)
  Release() = 0;

  // Called on the owning thread before any additional transactions are
  // unblocked.
  virtual void
  TransactionFinishedBeforeUnblock() = 0;

  // Called on the owning thread after additional transactions may have been
  // unblocked.
  virtual void
  TransactionFinishedAfterUnblock() = 0;

protected:
  FinishCallback()
  { }

  virtual
  ~FinishCallback()
  { }
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_transactionthreadpool_h__
