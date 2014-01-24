/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "TransactionThreadPool.h"

#include "IDBTransaction.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "nsComponentManagerUtils.h"
#include "nsIThreadPool.h"
#include "nsThreadUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsXPCOMCIDInternal.h"
#include "ProfilerHelpers.h"

using mozilla::MonitorAutoLock;
using mozilla::ipc::AssertIsOnBackgroundThread;

USING_INDEXEDDB_NAMESPACE

namespace {

const uint32_t kThreadLimit = 20;
const uint32_t kIdleThreadLimit = 5;
const uint32_t kIdleThreadTimeoutMs = 30000;

// Only touched on the PBackground thread.
TransactionThreadPool* gThreadPool = nullptr;

// Only touched on the PBackground thread.
uint64_t gUniqueTransactionId = 0;

#ifdef MOZ_ENABLE_PROFILER_SPS

class TransactionThreadPoolListener : public nsIThreadPoolListener
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSITHREADPOOLLISTENER

private:
  virtual ~TransactionThreadPoolListener()
  { }
};

#endif // MOZ_ENABLE_PROFILER_SPS

} // anonymous namespace

BEGIN_INDEXEDDB_NAMESPACE

class FinishTransactionRunnable MOZ_FINAL : public nsIRunnable
{
public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIRUNNABLE

  inline FinishTransactionRunnable(uint64_t aTransactionId,
                                   const nsACString& aDatabaseId,
                                   const nsTArray<nsString>& aObjectStoreNames,
                                   uint16_t aMode,
                                   nsCOMPtr<nsIRunnable>& aFinishRunnable);

private:
  uint64_t mTransactionId;
  const nsCString mDatabaseId;
  const nsTArray<nsString> mObjectStoreNames;
  uint16_t mMode;
  nsCOMPtr<nsIRunnable> mFinishRunnable;
};

END_INDEXEDDB_NAMESPACE

class TransactionThreadPool::CleanupRunnable
  : public nsRunnable
{
  nsAutoPtr<TransactionThreadPool> mThreadPool;

public:
  CleanupRunnable(TransactionThreadPool* aThreadPool)
    : mThreadPool(aThreadPool)
  {
    AssertIsOnBackgroundThread();
    MOZ_ASSERT(aThreadPool);
  }

  NS_IMETHOD
  Run()
  {
    AssertIsOnBackgroundThread();
    if (NS_FAILED(mThreadPool->Cleanup())) {
      NS_WARNING("Failed to clean up thread pool!");
    }

    return NS_OK;
  }
};

TransactionThreadPool::TransactionThreadPool()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!gThreadPool);
}

TransactionThreadPool::~TransactionThreadPool()
{
  AssertIsOnBackgroundThread();
}

// static
TransactionThreadPool*
TransactionThreadPool::GetOrCreate()
{
  AssertIsOnBackgroundThread();

  if (!gThreadPool) {
    nsAutoPtr<TransactionThreadPool> pool(new TransactionThreadPool());

    if (NS_WARN_IF(NS_FAILED(pool->Init()))) {
      return nullptr;
    }

    gThreadPool = pool.forget();
  }

  return gThreadPool;
}

// static
TransactionThreadPool*
TransactionThreadPool::Get()
{
  AssertIsOnBackgroundThread();

  return gThreadPool;
}

// static
void
TransactionThreadPool::Shutdown()
{
  AssertIsOnBackgroundThread();

  if (gThreadPool) {
    nsRefPtr<CleanupRunnable> runnable = new CleanupRunnable(gThreadPool);
    gThreadPool = nullptr;

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(runnable)));
  }

  gUniqueTransactionId = 0;
}

uint64_t
TransactionThreadPool::NextTransactionId()
{
  AssertIsOnBackgroundThread();

  return ++gUniqueTransactionId;
}

nsresult
TransactionThreadPool::Init()
{
  AssertIsOnBackgroundThread();

  nsresult rv;
  mThreadPool = do_CreateInstance(NS_THREADPOOL_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetName(NS_LITERAL_CSTRING("IndexedDB Trans"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetThreadLimit(kThreadLimit);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetIdleThreadLimit(kIdleThreadLimit);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mThreadPool->SetIdleThreadTimeout(kIdleThreadTimeoutMs);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

#ifdef MOZ_ENABLE_PROFILER_SPS
  nsCOMPtr<nsIThreadPoolListener> listener =
    new TransactionThreadPoolListener();

  rv = mThreadPool->SetListener(listener);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }
#endif

  return NS_OK;
}

nsresult
TransactionThreadPool::Cleanup()
{
  AssertIsOnBackgroundThread();

  PROFILER_LABEL("IndexedDB", "TransactionThreadPool::Cleanup");

  nsresult rv = mThreadPool->Shutdown();
  NS_ENSURE_SUCCESS(rv, rv);

  nsIThread* currentThread = NS_GetCurrentThread();
  MOZ_ASSERT(currentThread);

  // Make sure the pool is still accessible while any callbacks generated from
  // the other threads are processed.
  rv = NS_ProcessPendingEvents(currentThread);
  NS_ENSURE_SUCCESS(rv, rv);

  if (!mCompleteCallbacks.IsEmpty()) {
    // Run all callbacks manually now.
    for (uint32_t index = 0; index < mCompleteCallbacks.Length(); index++) {
      mCompleteCallbacks[index].mCallback->Run();
    }
    mCompleteCallbacks.Clear();

    // And make sure they get processed.
    rv = NS_ProcessPendingEvents(currentThread);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

// static
PLDHashOperator
TransactionThreadPool::MaybeUnblockTransaction(nsPtrHashKey<TransactionInfo>* aKey,
                                               void* aUserArg)
{
  AssertIsOnBackgroundThread();

  TransactionInfo* maybeUnblockedInfo = aKey->GetKey();
  TransactionInfo* finishedInfo = static_cast<TransactionInfo*>(aUserArg);

  NS_ASSERTION(maybeUnblockedInfo->blockedOn.Contains(finishedInfo),
               "Huh?");
  maybeUnblockedInfo->blockedOn.RemoveEntry(finishedInfo);
  if (!maybeUnblockedInfo->blockedOn.Count()) {
    // Let this transaction run.
    maybeUnblockedInfo->queue->Unblock();
  }

  return PL_DHASH_NEXT;
}

void
TransactionThreadPool::FinishTransaction(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode)
{
  AssertIsOnBackgroundThread();

  PROFILER_LABEL("IndexedDB", "TransactionThreadPool::FinishTransaction");

  DatabaseTransactionInfo* dbTransactionInfo;
  if (!mTransactionsInProgress.Get(aDatabaseId, &dbTransactionInfo)) {
    NS_ERROR("We don't know anyting about this database?!");
    return;
  }

  DatabaseTransactionInfo::TransactionHashtable& transactionsInProgress =
    dbTransactionInfo->transactions;

  uint32_t transactionCount = transactionsInProgress.Count();

#ifdef DEBUG
  if (aMode == IDBTransaction::VERSION_CHANGE) {
    NS_ASSERTION(transactionCount == 1,
                 "More transactions running than should be!");
  }
#endif

  if (transactionCount == 1) {
#ifdef DEBUG
    {
      const TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
      NS_ASSERTION(info->transactionId == aTransactionId, "Transaction mismatch!");
    }
#endif
    mTransactionsInProgress.Remove(aDatabaseId);

    // See if we need to fire any complete callbacks.
    uint32_t index = 0;
    while (index < mCompleteCallbacks.Length()) {
      if (MaybeFireCallback(mCompleteCallbacks[index])) {
        mCompleteCallbacks.RemoveElementAt(index);
      }
      else {
        index++;
      }
    }

    return;
  }
  TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
  NS_ASSERTION(info, "We've never heard of this transaction?!?");

  const nsTArray<nsString>& objectStoreNames = aObjectStoreNames;
  for (uint32_t index = 0, count = objectStoreNames.Length(); index < count;
       index++) {
    TransactionInfoPair* blockInfo =
      dbTransactionInfo->blockingTransactions.Get(objectStoreNames[index]);
    NS_ASSERTION(blockInfo, "Huh?");

    if (aMode == IDBTransaction::READ_WRITE &&
        blockInfo->lastBlockingReads == info) {
      blockInfo->lastBlockingReads = nullptr;
    }

    uint32_t i = blockInfo->lastBlockingWrites.IndexOf(info);
    if (i != blockInfo->lastBlockingWrites.NoIndex) {
      blockInfo->lastBlockingWrites.RemoveElementAt(i);
    }
  }

  info->blocking.EnumerateEntries(MaybeUnblockTransaction, info);

  transactionsInProgress.Remove(aTransactionId);
}

TransactionThreadPool::TransactionQueue*
TransactionThreadPool::GetQueueForTransaction(uint64_t aTransactionId,
                                              const nsACString& aDatabaseId)
{
  AssertIsOnBackgroundThread();

  DatabaseTransactionInfo* dbTransactionInfo;
  if (mTransactionsInProgress.Get(aDatabaseId, &dbTransactionInfo)) {
    DatabaseTransactionInfo::TransactionHashtable& transactionsInProgress =
      dbTransactionInfo->transactions;
    TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
    if (info) {
      // We recognize this one.
      return info->queue;
    }
  }

  return nullptr;
}

TransactionThreadPool::TransactionQueue&
TransactionThreadPool::GetQueueForTransaction(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode)
{
  AssertIsOnBackgroundThread();

  TransactionQueue* existingQueue =
    GetQueueForTransaction(aTransactionId, aDatabaseId);
  if (existingQueue) {
    return *existingQueue;
  }

  // See if we can run this transaction now.
  DatabaseTransactionInfo* dbTransactionInfo;
  if (!mTransactionsInProgress.Get(aDatabaseId, &dbTransactionInfo)) {
    // First transaction for this database.
    dbTransactionInfo = new DatabaseTransactionInfo();
    mTransactionsInProgress.Put(aDatabaseId, dbTransactionInfo);
  }

  DatabaseTransactionInfo::TransactionHashtable& transactionsInProgress =
    dbTransactionInfo->transactions;
  TransactionInfo* info = transactionsInProgress.Get(aTransactionId);
  if (info) {
    // We recognize this one.
    return *info->queue;
  }

  TransactionInfo* transactionInfo = new TransactionInfo(aTransactionId,
                                                         aDatabaseId,
                                                         aObjectStoreNames,
                                                         aMode);

  dbTransactionInfo->transactions.Put(aTransactionId, transactionInfo);;

  for (uint32_t index = 0, count = aObjectStoreNames.Length(); index < count;
       index++) {
    TransactionInfoPair* blockInfo =
      dbTransactionInfo->blockingTransactions.Get(aObjectStoreNames[index]);
    if (!blockInfo) {
      blockInfo = new TransactionInfoPair();
      blockInfo->lastBlockingReads = nullptr;
      dbTransactionInfo->blockingTransactions.Put(aObjectStoreNames[index],
                                                  blockInfo);
    }

    // Mark what we are blocking on.
    if (blockInfo->lastBlockingReads) {
      TransactionInfo* blockingInfo = blockInfo->lastBlockingReads;
      transactionInfo->blockedOn.PutEntry(blockingInfo);
      blockingInfo->blocking.PutEntry(transactionInfo);
    }

    if (aMode == IDBTransaction::READ_WRITE &&
        blockInfo->lastBlockingWrites.Length()) {
      for (uint32_t index = 0,
           count = blockInfo->lastBlockingWrites.Length(); index < count;
           index++) {
        TransactionInfo* blockingInfo = blockInfo->lastBlockingWrites[index];
        transactionInfo->blockedOn.PutEntry(blockingInfo);
        blockingInfo->blocking.PutEntry(transactionInfo);
      }
    }

    if (aMode == IDBTransaction::READ_WRITE) {
      blockInfo->lastBlockingReads = transactionInfo;
      blockInfo->lastBlockingWrites.Clear();
    }
    else {
      blockInfo->lastBlockingWrites.AppendElement(transactionInfo);
    }
  }

  if (!transactionInfo->blockedOn.Count()) {
    transactionInfo->queue->Unblock();
  }

  return *transactionInfo->queue;
}

nsresult
TransactionThreadPool::Dispatch(uint64_t aTransactionId,
                                const nsACString& aDatabaseId,
                                const nsTArray<nsString>& aObjectStoreNames,
                                uint16_t aMode,
                                nsIRunnable* aRunnable,
                                bool aFinish,
                                nsIRunnable* aFinishRunnable)
{
  TransactionQueue& queue = GetQueueForTransaction(aTransactionId,
                                                   aDatabaseId,
                                                   aObjectStoreNames,
                                                   aMode);

  queue.Dispatch(aRunnable);
  if (aFinish) {
    queue.Finish(aFinishRunnable);
  }
  return NS_OK;
}

void
TransactionThreadPool::Dispatch(uint64_t aTransactionId,
                                const nsACString& aDatabaseId,
                                nsIRunnable* aRunnable,
                                bool aFinish,
                                nsIRunnable* aFinishRunnable)
{
  TransactionQueue* queue = GetQueueForTransaction(aTransactionId, aDatabaseId);
  if (!queue) {
    MOZ_CRASH("This transaction id was useless!");
  }

  queue->Dispatch(aRunnable);
  if (aFinish) {
    queue->Finish(aFinishRunnable);
  }
}

void
TransactionThreadPool::WaitForDatabasesToComplete(
                                             nsTArray<nsCString>& aDatabaseIds,
                                             nsIRunnable* aCallback)
{
  AssertIsOnBackgroundThread();
  NS_ASSERTION(!aDatabaseIds.IsEmpty(), "No databases to wait on!");
  NS_ASSERTION(aCallback, "Null pointer!");

  DatabasesCompleteCallback* callback = mCompleteCallbacks.AppendElement();

  callback->mCallback = aCallback;
  callback->mDatabaseIds.SwapElements(aDatabaseIds);

  if (MaybeFireCallback(*callback)) {
    mCompleteCallbacks.RemoveElementAt(mCompleteCallbacks.Length() - 1);
  }
}

// static
PLDHashOperator
TransactionThreadPool::CollectTransactions(const uint64_t& aTransactionId,
                                           TransactionInfo* aValue,
                                           void* aUserArg)
{
  nsAutoTArray<TransactionInfo*, 50>* transactionArray =
    static_cast<nsAutoTArray<TransactionInfo*, 50>*>(aUserArg);
  transactionArray->AppendElement(aValue);

  return PL_DHASH_NEXT;
}

struct MOZ_STACK_CLASS TransactionSearchInfo
{
  TransactionSearchInfo(const nsACString& aDatabaseId)
    : databaseId(aDatabaseId), found(false)
  {
  }

  nsCString databaseId;
  bool found;
};

// static
PLDHashOperator
TransactionThreadPool::FindTransaction(const uint64_t& aTransactionId,
                                       TransactionInfo* aValue,
                                       void* aUserArg)
{
  TransactionSearchInfo* info = static_cast<TransactionSearchInfo*>(aUserArg);

  if (aValue->databaseId == info->databaseId) {
    info->found = true;
    return PL_DHASH_STOP;
  }

  return PL_DHASH_NEXT;
}
bool
TransactionThreadPool::HasTransactionsForDatabase(const nsACString& aDatabaseId)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!aDatabaseId.IsEmpty(), "An empty DatabaseId!");

  DatabaseTransactionInfo* dbTransactionInfo = nullptr;
  dbTransactionInfo = mTransactionsInProgress.Get(aDatabaseId);
  if (!dbTransactionInfo) {
    return false;
  }

  TransactionSearchInfo info(aDatabaseId);
  dbTransactionInfo->transactions.EnumerateRead(FindTransaction, &info);

  return info.found;
}

bool
TransactionThreadPool::MaybeFireCallback(DatabasesCompleteCallback aCallback)
{
  AssertIsOnBackgroundThread();

  PROFILER_LABEL("IndexedDB", "TransactionThreadPool::MaybeFireCallback");

  for (uint32_t index = 0; index < aCallback.mDatabaseIds.Length(); index++) {
    nsCString databaseId = aCallback.mDatabaseIds[index];
    if (databaseId.IsEmpty()) {
      MOZ_CRASH();
    }

    if (mTransactionsInProgress.Get(databaseId, nullptr)) {
      return false;
    }
  }

  aCallback.mCallback->Run();
  return true;
}

TransactionThreadPool::
TransactionQueue::TransactionQueue(uint64_t aTransactionId,
                                   const nsACString& aDatabaseId,
                                   const nsTArray<nsString>& aObjectStoreNames,
                                   uint16_t aMode)
: mMonitor("TransactionQueue::mMonitor"),
  mOwningThread(NS_GetCurrentThread()),
  mTransactionId(aTransactionId),
  mDatabaseId(aDatabaseId),
  mObjectStoreNames(aObjectStoreNames),
  mMode(aMode),
  mShouldFinish(false)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(mOwningThread);
}

void
TransactionThreadPool::TransactionQueue::Unblock()
{
  MonitorAutoLock lock(mMonitor);

  // NB: Finish may be called before Unblock.

  TransactionThreadPool::Get()->mThreadPool->
    Dispatch(this, NS_DISPATCH_NORMAL);
}

void
TransactionThreadPool::TransactionQueue::Dispatch(nsIRunnable* aRunnable)
{
  MonitorAutoLock lock(mMonitor);

  NS_ASSERTION(!mShouldFinish, "Dispatch called after Finish!");

  mQueue.AppendElement(aRunnable);

  mMonitor.Notify();
}

void
TransactionThreadPool::TransactionQueue::Finish(nsIRunnable* aFinishRunnable)
{
  MonitorAutoLock lock(mMonitor);

  NS_ASSERTION(!mShouldFinish, "Finish called more than once!");

  mShouldFinish = true;
  mFinishRunnable = aFinishRunnable;

  mMonitor.Notify();
}

NS_IMPL_ISUPPORTS1(TransactionThreadPool::TransactionQueue, nsIRunnable)

NS_IMETHODIMP
TransactionThreadPool::TransactionQueue::Run()
{
  PROFILER_LABEL("IndexedDB", "TransactionQueue::Run");

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: Beginning database work",
                    "IDBTransaction[%llu] DT Start",
                    mTransaction->GetSerialNumber());

  nsAutoTArray<nsCOMPtr<nsIRunnable>, 10> queue;
  nsCOMPtr<nsIRunnable> finishRunnable;
  bool shouldFinish = false;

  do {
    NS_ASSERTION(queue.IsEmpty(), "Should have cleared this!");

    {
      MonitorAutoLock lock(mMonitor);
      while (!mShouldFinish && mQueue.IsEmpty()) {
        if (NS_FAILED(mMonitor.Wait())) {
          NS_ERROR("Failed to wait!");
        }
      }

      mQueue.SwapElements(queue);
      if (mShouldFinish) {
        mFinishRunnable.swap(finishRunnable);
        shouldFinish = true;
      }
    }

    uint32_t count = queue.Length();
    for (uint32_t index = 0; index < count; index++) {
      nsCOMPtr<nsIRunnable>& runnable = queue[index];
      runnable->Run();
      runnable = nullptr;
    }

    if (count) {
      queue.Clear();
    }
  } while (!shouldFinish);

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: Finished database work",
                    "IDBTransaction[%llu] DT Done",
                    mTransaction->GetSerialNumber());

  nsCOMPtr<nsIRunnable> finishTransactionRunnable =
    new FinishTransactionRunnable(mTransactionId, mDatabaseId,
                                  mObjectStoreNames, mMode,
                                  finishRunnable);
  if (NS_FAILED(mOwningThread->Dispatch(finishTransactionRunnable,
                                        NS_DISPATCH_NORMAL))) {
    NS_WARNING("Failed to dispatch finishTransactionRunnable!");
  }

  return NS_OK;
}

FinishTransactionRunnable::FinishTransactionRunnable(
                                    uint64_t aTransactionId,
                                    const nsACString& aDatabaseId,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    uint16_t aMode,
                                    nsCOMPtr<nsIRunnable>& aFinishRunnable)
: mTransactionId(aTransactionId),
  mDatabaseId(aDatabaseId),
  mObjectStoreNames(aObjectStoreNames),
  mMode(aMode)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  mFinishRunnable.swap(aFinishRunnable);
}

NS_IMPL_ISUPPORTS1(FinishTransactionRunnable, nsIRunnable)

NS_IMETHODIMP
FinishTransactionRunnable::Run()
{
  AssertIsOnBackgroundThread();

  PROFILER_LABEL("IndexedDB", "FinishTransactionRunnable::Run");

  if (!gThreadPool) {
    NS_ERROR("Running after shutdown!");
    return NS_ERROR_FAILURE;
  }

  gThreadPool->FinishTransaction(mTransactionId, mDatabaseId, mObjectStoreNames,
                                 mMode);

  if (mFinishRunnable) {
    mFinishRunnable->Run();
    mFinishRunnable = nullptr;
  }

  return NS_OK;
}

#ifdef MOZ_ENABLE_PROFILER_SPS

NS_IMPL_ISUPPORTS1(TransactionThreadPoolListener, nsIThreadPoolListener)

NS_IMETHODIMP
TransactionThreadPoolListener::OnThreadCreated()
{
  MOZ_ASSERT(!NS_IsMainThread());
  char aLocal;
  profiler_register_thread("IndexedDB Transaction", &aLocal);
  return NS_OK;
}

NS_IMETHODIMP
TransactionThreadPoolListener::OnThreadShuttingDown()
{
  MOZ_ASSERT(!NS_IsMainThread());
  profiler_unregister_thread();
  return NS_OK;
}

#endif // MOZ_ENABLE_PROFILER_SPS
