/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "IDBObjectStore.h"

#include <algorithm>
#include "ActorsChild.h"
#include "AsyncConnectionHelper.h"
#include "DatabaseInfo.h"
#include "FileManager.h"
#include "IDBCursor.h"
#include "IDBEvents.h"
#include "IDBFileHandle.h"
#include "IDBIndex.h"
#include "IDBKeyRange.h"
#include "IDBTransaction.h"
#include "IndexedDatabase.h"
#include "IndexedDatabaseInlines.h"
#include "jsfriendapi.h"
#include "KeyPath.h"
#include "MainThreadUtils.h"
#include "mozilla/Endian.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/Move.h"
#include "mozilla/storage.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/ContentParent.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/dom/FileHandleBinding.h"
#include "mozilla/dom/IDBObjectStoreBinding.h"
#include "mozilla/dom/PBlobParent.h"
#include "mozilla/dom/StructuredCloneTags.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "mozilla/dom/ipc/Blob.h"
#include "mozilla/dom/ipc/nsIRemoteBlob.h"
#include "mozilla/dom/quota/FileStreams.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsDOMClassInfo.h"
#include "nsDOMFile.h"
#include "nsIOutputStream.h"
#include "nsJSUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsThreadUtils.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"
#include "snappy/snappy.h"

#define FILE_COPY_BUFFER_SIZE 32768

using namespace mozilla;
using namespace mozilla::dom;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;

namespace mozilla {
namespace dom {
namespace indexedDB {

struct FileHandleData
{
  nsString type;
  nsString name;
};

struct BlobOrFileData
{
  BlobOrFileData()
  : tag(0), size(0), lastModifiedDate(UINT64_MAX)
  { }

  uint32_t tag;
  uint64_t size;
  nsString type;
  nsString name;
  uint64_t lastModifiedDate;
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

namespace {

inline
bool
IgnoreNothing(char16_t c)
{
  return false;
}

class ObjectStoreHelper : public AsyncConnectionHelper
{
protected:
  class ObjectStoreRequestParams;
  class IndexedDBObjectStoreRequestChild;

public:
  ObjectStoreHelper(IDBTransaction* aTransaction,
                    IDBRequest* aRequest,
                    IDBObjectStore* aObjectStore)
  : AsyncConnectionHelper(aTransaction, aRequest), mObjectStore(aObjectStore),
    mActor(nullptr)
  {
    NS_ASSERTION(aTransaction, "Null transaction!");
    NS_ASSERTION(aRequest, "Null request!");
    NS_ASSERTION(aObjectStore, "Null object store!");
  }

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult Dispatch(nsIEventTarget* aDatabaseThread) MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) = 0;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue) = 0;

protected:
  nsRefPtr<IDBObjectStore> mObjectStore;

private:
  IndexedDBObjectStoreRequestChild* mActor;
};

class NoRequestObjectStoreHelper : public AsyncConnectionHelper
{
public:
  NoRequestObjectStoreHelper(IDBTransaction* aTransaction,
                             IDBObjectStore* aObjectStore)
  : AsyncConnectionHelper(aTransaction, nullptr), mObjectStore(aObjectStore)
  {
    NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
    NS_ASSERTION(aTransaction, "Null transaction!");
    NS_ASSERTION(aObjectStore, "Null object store!");
  }

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
                                            MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult OnSuccess() MOZ_OVERRIDE;

  virtual void OnError() MOZ_OVERRIDE;

protected:
  nsRefPtr<IDBObjectStore> mObjectStore;
};

class AddHelper : public ObjectStoreHelper
{
public:
  AddHelper(IDBTransaction* aTransaction,
            IDBRequest* aRequest,
            IDBObjectStore* aObjectStore,
            StructuredCloneWriteInfo&& aCloneWriteInfo,
            const Key& aKey,
            bool aOverwrite,
            nsTArray<IndexUpdateInfo>& aIndexUpdateInfo)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mCloneWriteInfo(Move(aCloneWriteInfo)),
    mKey(aKey),
    mOverwrite(aOverwrite)
  {
    mIndexUpdateInfo.SwapElements(aIndexUpdateInfo);
  }

  ~AddHelper()
  {
    IDBObjectStore::ClearCloneWriteInfo(mCloneWriteInfo);
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

private:
  // These may change in the autoincrement case.
  StructuredCloneWriteInfo mCloneWriteInfo;
  Key mKey;
  nsTArray<IndexUpdateInfo> mIndexUpdateInfo;
  const bool mOverwrite;
};

class GetHelper : public ObjectStoreHelper
{
public:
  GetHelper(IDBTransaction* aTransaction,
            IDBRequest* aRequest,
            IDBObjectStore* aObjectStore,
            IDBKeyRange* aKeyRange)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mKeyRange(aKeyRange)
  {
    NS_ASSERTION(aKeyRange, "Null key range!");
  }

  ~GetHelper()
  {
    IDBObjectStore::ClearCloneReadInfo(mCloneReadInfo);
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

protected:
  // In-params.
  nsRefPtr<IDBKeyRange> mKeyRange;

private:
  // Out-params.
  StructuredCloneReadInfo mCloneReadInfo;
};

class DeleteHelper : public GetHelper
{
public:
  DeleteHelper(IDBTransaction* aTransaction,
               IDBRequest* aRequest,
               IDBObjectStore* aObjectStore,
               IDBKeyRange* aKeyRange)
  : GetHelper(aTransaction, aRequest, aObjectStore, aKeyRange)
  { }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;
};

class ClearHelper : public ObjectStoreHelper
{
public:
  ClearHelper(IDBTransaction* aTransaction,
              IDBRequest* aRequest,
              IDBObjectStore* aObjectStore)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore)
  { }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;
};

class OpenCursorHelper : public ObjectStoreHelper
{
public:
  OpenCursorHelper(IDBTransaction* aTransaction,
                   IDBRequest* aRequest,
                   IDBObjectStore* aObjectStore,
                   IDBKeyRange* aKeyRange,
                   IDBCursor::Direction aDirection)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mKeyRange(aKeyRange), mDirection(aDirection)
  { }

  ~OpenCursorHelper()
  {
    IDBObjectStore::ClearCloneReadInfo(mCloneReadInfo);
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

private:
  nsresult EnsureCursor();

  // In-params.
  nsRefPtr<IDBKeyRange> mKeyRange;
  const IDBCursor::Direction mDirection;

  // Out-params.
  Key mKey;
  StructuredCloneReadInfo mCloneReadInfo;
  nsCString mContinueQuery;
  nsCString mContinueToQuery;
  Key mRangeKey;

  // Only used in the parent process.
  nsRefPtr<IDBCursor> mCursor;
  SerializedStructuredCloneReadInfo mSerializedCloneReadInfo;
};

class OpenKeyCursorHelper MOZ_FINAL : public ObjectStoreHelper
{
public:
  OpenKeyCursorHelper(IDBTransaction* aTransaction,
                      IDBRequest* aRequest,
                      IDBObjectStore* aObjectStore,
                      IDBKeyRange* aKeyRange,
                      IDBCursor::Direction aDirection)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mKeyRange(aKeyRange), mDirection(aDirection)
  { }

  virtual nsresult
  DoDatabaseWork(mozIStorageConnection* aConnection) MOZ_OVERRIDE;

  virtual nsresult
  GetSuccessResult(JSContext* aCx, JS::MutableHandle<JS::Value> aVal)
                   MOZ_OVERRIDE;

  virtual void
  ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

private:
  ~OpenKeyCursorHelper()
  { }

  nsresult EnsureCursor();

  // In-params.
  nsRefPtr<IDBKeyRange> mKeyRange;
  const IDBCursor::Direction mDirection;

  // Out-params.
  Key mKey;
  nsCString mContinueQuery;
  nsCString mContinueToQuery;
  Key mRangeKey;

  // Only used in the parent process.
  nsRefPtr<IDBCursor> mCursor;
};

class GetAllHelper : public ObjectStoreHelper
{
public:
  GetAllHelper(IDBTransaction* aTransaction,
               IDBRequest* aRequest,
               IDBObjectStore* aObjectStore,
               IDBKeyRange* aKeyRange,
               const uint32_t aLimit)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mKeyRange(aKeyRange), mLimit(aLimit)
  { }

  ~GetAllHelper()
  {
    for (uint32_t index = 0; index < mCloneReadInfos.Length(); index++) {
      IDBObjectStore::ClearCloneReadInfo(mCloneReadInfos[index]);
    }
  }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

protected:
  // In-params.
  nsRefPtr<IDBKeyRange> mKeyRange;
  const uint32_t mLimit;

private:
  // Out-params.
  nsTArray<StructuredCloneReadInfo> mCloneReadInfos;
};

class GetAllKeysHelper MOZ_FINAL : public ObjectStoreHelper
{
public:
  GetAllKeysHelper(IDBTransaction* aTransaction,
                   IDBRequest* aRequest,
                   IDBObjectStore* aObjectStore,
                   IDBKeyRange* aKeyRange,
                   const uint32_t aLimit)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mKeyRange(aKeyRange), mLimit(aLimit)
  { }

  virtual nsresult
  DoDatabaseWork(mozIStorageConnection* aConnection) MOZ_OVERRIDE;

  virtual nsresult
  GetSuccessResult(JSContext* aCx, JS::MutableHandle<JS::Value> aVal)
                   MOZ_OVERRIDE;

  virtual void
  ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

private:
  ~GetAllKeysHelper()
  { }

  nsRefPtr<IDBKeyRange> mKeyRange;
  const uint32_t mLimit;
  nsTArray<Key> mKeys;
};

class CountHelper : public ObjectStoreHelper
{
public:
  CountHelper(IDBTransaction* aTransaction,
              IDBRequest* aRequest,
              IDBObjectStore* aObjectStore,
              IDBKeyRange* aKeyRange)
  : ObjectStoreHelper(aTransaction, aRequest, aObjectStore),
    mKeyRange(aKeyRange), mCount(0)
  { }

  virtual nsresult DoDatabaseWork(mozIStorageConnection* aConnection)
                                  MOZ_OVERRIDE;

  virtual nsresult GetSuccessResult(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aVal) MOZ_OVERRIDE;

  virtual void ReleaseMainThreadObjects() MOZ_OVERRIDE;

  virtual nsresult
  PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams) MOZ_OVERRIDE;

  virtual ChildProcessSendResult
  SendResponseToChildProcess(nsresult aResultCode) MOZ_OVERRIDE;

  virtual nsresult
  UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
                                  MOZ_OVERRIDE;

private:
  nsRefPtr<IDBKeyRange> mKeyRange;
  uint64_t mCount;
};

class MOZ_STACK_CLASS AutoRemoveIndex
{
public:
  AutoRemoveIndex(ObjectStoreInfo* aObjectStoreInfo,
                  const nsAString& aIndexName)
  : mObjectStoreInfo(aObjectStoreInfo), mIndexName(aIndexName)
  { }

  ~AutoRemoveIndex()
  {
    if (mObjectStoreInfo) {
      for (uint32_t i = 0; i < mObjectStoreInfo->indexes.Length(); i++) {
        if (mObjectStoreInfo->indexes[i].name == mIndexName) {
          mObjectStoreInfo->indexes.RemoveElementAt(i);
          break;
        }
      }
    }
  }

  void forget()
  {
    mObjectStoreInfo = nullptr;
  }

private:
  ObjectStoreInfo* mObjectStoreInfo;
  nsString mIndexName;
};

class ThreadLocalJSRuntime
{
  JSRuntime* mRuntime;
  JSContext* mContext;
  JSObject* mGlobal;

  static const JSClass sGlobalClass;
  static const unsigned sRuntimeHeapSize = 768 * 1024;

  ThreadLocalJSRuntime()
  : mRuntime(nullptr), mContext(nullptr), mGlobal(nullptr)
  {
      MOZ_COUNT_CTOR(ThreadLocalJSRuntime);
  }

  nsresult Init()
  {
    mRuntime = JS_NewRuntime(sRuntimeHeapSize, JS_NO_HELPER_THREADS);
    NS_ENSURE_TRUE(mRuntime, NS_ERROR_OUT_OF_MEMORY);

    /*
     * Not setting this will cause JS_CHECK_RECURSION to report false
     * positives
     */
    JS_SetNativeStackQuota(mRuntime, 128 * sizeof(size_t) * 1024); 

    mContext = JS_NewContext(mRuntime, 0);
    NS_ENSURE_TRUE(mContext, NS_ERROR_OUT_OF_MEMORY);

    JSAutoRequest ar(mContext);

    mGlobal = JS_NewGlobalObject(mContext, &sGlobalClass, nullptr,
                                 JS::FireOnNewGlobalHook);
    NS_ENSURE_TRUE(mGlobal, NS_ERROR_OUT_OF_MEMORY);

    js::SetDefaultObjectForContext(mContext, mGlobal);
    return NS_OK;
  }

 public:
  static ThreadLocalJSRuntime *Create()
  {
    ThreadLocalJSRuntime *entry = new ThreadLocalJSRuntime();
    NS_ENSURE_TRUE(entry, nullptr);

    if (NS_FAILED(entry->Init())) {
      delete entry;
      return nullptr;
    }

    return entry;
  }

  JSContext *Context() const
  {
    return mContext;
  }

  JSObject *Global() const
  {
    return mGlobal;
  }

  ~ThreadLocalJSRuntime()
  {
    MOZ_COUNT_DTOR(ThreadLocalJSRuntime);

    if (mContext) {
      JS_DestroyContext(mContext);
    }

    if (mRuntime) {
      JS_DestroyRuntime(mRuntime);
    }
  }
};

const JSClass ThreadLocalJSRuntime::sGlobalClass = {
  "IndexedDBTransactionThreadGlobal",
  JSCLASS_GLOBAL_FLAGS,
  JS_PropertyStub, JS_DeletePropertyStub, JS_PropertyStub, JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub, JS_ConvertStub
};

already_AddRefed<IDBRequest>
GenerateRequest(IDBObjectStore* aObjectStore)
{
  MOZ_ASSERT(aObjectStore);
  aObjectStore->AssertIsOnOwningThread();

  IDBTransaction* transaction = aObjectStore->Transaction();

  nsRefPtr<IDBRequest> request =
    IDBRequest::Create(aObjectStore, transaction->Database(), transaction);
  MOZ_ASSERT(request);

  return request.forget();
}

struct MOZ_STACK_CLASS GetAddInfoClosure
{
  IDBObjectStore* mThis;
  StructuredCloneWriteInfo& mCloneWriteInfo;
  JS::Handle<JS::Value> mValue;
};

nsresult
GetAddInfoCallback(JSContext* aCx, void* aClosure)
{
  GetAddInfoClosure* data = static_cast<GetAddInfoClosure*>(aClosure);
  MOZ_ASSERT(data);
  data->mThis->AssertIsOnOwningThread();

  data->mCloneWriteInfo.mOffsetToKeyProp = 0;
  data->mCloneWriteInfo.mTransaction = data->mThis->Transaction();

  if (!IDBObjectStore::SerializeValue(aCx, data->mCloneWriteInfo,
                                      data->mValue)) {
    return NS_ERROR_DOM_DATA_CLONE_ERR;
  }

  return NS_OK;
}

inline
BlobChild*
ActorFromRemoteBlob(nsIDOMBlob* aBlob)
{
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  nsCOMPtr<nsIRemoteBlob> remoteBlob = do_QueryInterface(aBlob);
  if (remoteBlob) {
    BlobChild* actor =
      static_cast<BlobChild*>(static_cast<PBlobChild*>(remoteBlob->GetPBlob()));
    NS_ASSERTION(actor, "Null actor?!");
    return actor;
  }
  return nullptr;
}

inline
bool
ResolveMysteryFile(nsIDOMBlob* aBlob, const nsString& aName,
                   const nsString& aContentType, uint64_t aSize,
                   uint64_t aLastModifiedDate)
{
  BlobChild* actor = ActorFromRemoteBlob(aBlob);
  if (actor) {
    return actor->SetMysteryBlobInfo(aName, aContentType,
                                     aSize, aLastModifiedDate);
  }
  return true;
}

inline
bool
ResolveMysteryBlob(nsIDOMBlob* aBlob, const nsString& aContentType,
                   uint64_t aSize)
{
  BlobChild* actor = ActorFromRemoteBlob(aBlob);
  if (actor) {
    return actor->SetMysteryBlobInfo(aContentType, aSize);
  }
  return true;
}

bool
StructuredCloneReadString(JSStructuredCloneReader* aReader,
                          nsCString& aString)
{
  uint32_t length;
  if (!JS_ReadBytes(aReader, &length, sizeof(uint32_t))) {
    NS_WARNING("Failed to read length!");
    return false;
  }
  length = NativeEndian::swapFromLittleEndian(length);

  if (!aString.SetLength(length, fallible_t())) {
    NS_WARNING("Out of memory?");
    return false;
  }
  char* buffer = aString.BeginWriting();

  if (!JS_ReadBytes(aReader, buffer, length)) {
    NS_WARNING("Failed to read type!");
    return false;
  }

  return true;
}

// static
bool
ReadFileHandle(JSStructuredCloneReader* aReader,
                FileHandleData* aRetval)
{
  static_assert(SCTAG_DOM_FILEHANDLE == 0xFFFF8004,
                "Update me!");
  MOZ_ASSERT(aReader && aRetval);

  nsCString type;
  if (!StructuredCloneReadString(aReader, type)) {
    return false;
  }
  CopyUTF8toUTF16(type, aRetval->type);

  nsCString name;
  if (!StructuredCloneReadString(aReader, name)) {
    return false;
  }
  CopyUTF8toUTF16(name, aRetval->name);

  return true;
}

// static
bool
ReadBlobOrFile(JSStructuredCloneReader* aReader,
               uint32_t aTag,
               BlobOrFileData* aRetval)
{
  static_assert(SCTAG_DOM_BLOB == 0xFFFF8001 &&
                SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE == 0xFFFF8002 &&
                SCTAG_DOM_FILE == 0xFFFF8005,
                "Update me!");
  MOZ_ASSERT(aReader && aRetval);
  MOZ_ASSERT(aTag == SCTAG_DOM_FILE ||
             aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
             aTag == SCTAG_DOM_BLOB);

  aRetval->tag = aTag;

  // If it's not a FileHandle, it's a Blob or a File.
  uint64_t size;
  if (!JS_ReadBytes(aReader, &size, sizeof(uint64_t))) {
    NS_WARNING("Failed to read size!");
    return false;
  }
  aRetval->size = NativeEndian::swapFromLittleEndian(size);

  nsCString type;
  if (!StructuredCloneReadString(aReader, type)) {
    return false;
  }
  CopyUTF8toUTF16(type, aRetval->type);

  // Blobs are done.
  if (aTag == SCTAG_DOM_BLOB) {
    return true;
  }

  NS_ASSERTION(aTag == SCTAG_DOM_FILE ||
               aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE, "Huh?!");

  uint64_t lastModifiedDate;
  if (aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE) {
    lastModifiedDate = UINT64_MAX;
  }
  else {
    if(!JS_ReadBytes(aReader, &lastModifiedDate, sizeof(lastModifiedDate))) {
      NS_WARNING("Failed to read lastModifiedDate");
      return false;
    }
    lastModifiedDate = NativeEndian::swapFromLittleEndian(lastModifiedDate);
  }
  aRetval->lastModifiedDate = lastModifiedDate;

  nsCString name;
  if (!StructuredCloneReadString(aReader, name)) {
    return false;
  }
  CopyUTF8toUTF16(name, aRetval->name);

  return true;
}

class ValueDeserializationHelper
{
public:
  static JSObject* CreateAndWrapFileHandle(JSContext* aCx,
                                           IDBDatabase* aDatabase,
                                           StructuredCloneFile& aFile,
                                           const FileHandleData& aData)
  {
    MOZ_ASSERT(NS_IsMainThread());

    nsRefPtr<FileInfo>& fileInfo = aFile.mFileInfo;

    nsRefPtr<IDBFileHandle> fileHandle = IDBFileHandle::Create(aDatabase,
      aData.name, aData.type, fileInfo.forget());

    JS::Rooted<JSObject*> global(aCx, JS::CurrentGlobalOrNull(aCx));
    if (!global) {
      return nullptr;
    }
    return fileHandle->WrapObject(aCx, global);
  }

  static JSObject* CreateAndWrapBlobOrFile(JSContext* aCx,
                                           IDBDatabase* aDatabase,
                                           StructuredCloneFile& aFile,
                                           const BlobOrFileData& aData)
  {
    MOZ_ASSERT(NS_IsMainThread());

    MOZ_ASSERT(aData.tag == SCTAG_DOM_FILE ||
               aData.tag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
               aData.tag == SCTAG_DOM_BLOB);

    nsresult rv = NS_OK;

    nsRefPtr<FileInfo>& fileInfo = aFile.mFileInfo;

    nsCOMPtr<nsIFile> nativeFile;
    if (!aFile.mFile) {
      FileManager* fileManager = aDatabase->Manager();
        NS_ASSERTION(fileManager, "This should never be null!");

      nsCOMPtr<nsIFile> directory = fileManager->GetDirectory();
      if (!directory) {
        NS_WARNING("Failed to get directory!");
        return nullptr;
      }

      nativeFile = fileManager->GetFileForId(directory, fileInfo->Id());
      if (!nativeFile) {
        NS_WARNING("Failed to get file!");
        return nullptr;
      }
    }

    if (aData.tag == SCTAG_DOM_BLOB) {
      nsCOMPtr<nsIDOMBlob> domBlob;
      if (aFile.mFile) {
        if (!ResolveMysteryBlob(aFile.mFile, aData.type, aData.size)) {
          return nullptr;
        }
        domBlob = aFile.mFile;
      }
      else {
        domBlob = new nsDOMFileFile(aData.type, aData.size, nativeFile,
                                    fileInfo);
      }

      JS::Rooted<JS::Value> wrappedBlob(aCx);
      JS::Rooted<JSObject*> global(aCx, JS::CurrentGlobalOrNull(aCx));
      rv = nsContentUtils::WrapNative(aCx, global, domBlob,
                                      &NS_GET_IID(nsIDOMBlob),
                                      &wrappedBlob);
      if (NS_FAILED(rv)) {
        NS_WARNING("Failed to wrap native!");
        return nullptr;
      }

      return JSVAL_TO_OBJECT(wrappedBlob);
    }

    nsCOMPtr<nsIDOMFile> domFile;
    if (aFile.mFile) {
      if (!ResolveMysteryFile(aFile.mFile, aData.name, aData.type, aData.size,
                              aData.lastModifiedDate)) {
        return nullptr;
      }
      domFile = do_QueryInterface(aFile.mFile);
      NS_ASSERTION(domFile, "This should never fail!");
    }
    else {
      domFile = new nsDOMFileFile(aData.name, aData.type, aData.size,
                                  nativeFile, fileInfo);
    }

    JS::Rooted<JS::Value> wrappedFile(aCx);
    JS::Rooted<JSObject*> global(aCx, JS::CurrentGlobalOrNull(aCx));
    rv = nsContentUtils::WrapNative(aCx, global, domFile,
                                    &NS_GET_IID(nsIDOMFile),
                                    &wrappedFile);
    if (NS_FAILED(rv)) {
      NS_WARNING("Failed to wrap native!");
      return nullptr;
    }

    return JSVAL_TO_OBJECT(wrappedFile);
  }
};

class IndexDeserializationHelper
{
public:
  static JSObject* CreateAndWrapFileHandle(JSContext* aCx,
                                           IDBDatabase* aDatabase,
                                           StructuredCloneFile& aFile,
                                           const FileHandleData& aData)
  {
    // FileHandle can't be used in index creation, so just make a dummy object.
    return JS_NewObject(aCx, nullptr, JS::NullPtr(), JS::NullPtr());
  }

  static JSObject* CreateAndWrapBlobOrFile(JSContext* aCx,
                                           IDBDatabase* aDatabase,
                                           StructuredCloneFile& aFile,
                                           const BlobOrFileData& aData)
  {
    MOZ_ASSERT(aData.tag == SCTAG_DOM_FILE ||
               aData.tag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
               aData.tag == SCTAG_DOM_BLOB);

    // The following properties are available for use in index creation
    //   Blob.size
    //   Blob.type
    //   File.name
    //   File.lastModifiedDate

    JS::Rooted<JSObject*> obj(aCx,
      JS_NewObject(aCx, nullptr, JS::NullPtr(), JS::NullPtr()));
    if (!obj) {
      NS_WARNING("Failed to create object!");
      return nullptr;
    }

    // Technically these props go on the proto, but this detail won't change
    // the results of index creation.

    JS::Rooted<JSString*> type(aCx,
      JS_NewUCStringCopyN(aCx, aData.type.get(), aData.type.Length()));
    if (!type ||
        !JS_DefineProperty(aCx, obj, "size",
                           JS_NumberValue((double)aData.size),
                           nullptr, nullptr, 0) ||
        !JS_DefineProperty(aCx, obj, "type", STRING_TO_JSVAL(type),
                           nullptr, nullptr, 0)) {
      return nullptr;
    }

    if (aData.tag == SCTAG_DOM_BLOB) {
      return obj;
    }

    JS::Rooted<JSString*> name(aCx,
      JS_NewUCStringCopyN(aCx, aData.name.get(), aData.name.Length()));
    JS::Rooted<JSObject*> date(aCx,
      JS_NewDateObjectMsec(aCx, aData.lastModifiedDate));
    if (!name || !date ||
        !JS_DefineProperty(aCx, obj, "name", STRING_TO_JSVAL(name),
                           nullptr, nullptr, 0) ||
        !JS_DefineProperty(aCx, obj, "lastModifiedDate", OBJECT_TO_JSVAL(date),
                           nullptr, nullptr, 0)) {
      return nullptr;
    }

    return obj;
  }
};

template <class DeserializationTraits>
JSObject*
CommonStructuredCloneReadCallback(JSContext* aCx,
                                  JSStructuredCloneReader* aReader,
                                  uint32_t aTag,
                                  uint32_t aData,
                                  void* aClosure)
{
  // We need to statically assert that our tag values are what we expect
  // so that if people accidentally change them they notice.
  static_assert(SCTAG_DOM_BLOB == 0xFFFF8001 &&
                SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE == 0xFFFF8002 &&
                SCTAG_DOM_FILEHANDLE == 0xFFFF8004 &&
                SCTAG_DOM_FILE == 0xFFFF8005,
                "You changed our structured clone tag values and just ate "
                "everyone's IndexedDB data.  I hope you are happy.");

  if (aTag == SCTAG_DOM_FILE_WITHOUT_LASTMODIFIEDDATE ||
      aTag == SCTAG_DOM_FILEHANDLE ||
      aTag == SCTAG_DOM_BLOB ||
      aTag == SCTAG_DOM_FILE) {
    StructuredCloneReadInfo* cloneReadInfo =
      reinterpret_cast<StructuredCloneReadInfo*>(aClosure);

    if (aData >= cloneReadInfo->mFiles.Length()) {
      NS_ERROR("Bad blob index!");
      return nullptr;
    }

    StructuredCloneFile& file = cloneReadInfo->mFiles[aData];
    IDBDatabase* database = cloneReadInfo->mDatabase;

    if (aTag == SCTAG_DOM_FILEHANDLE) {
      FileHandleData data;
      if (!ReadFileHandle(aReader, &data)) {
        return nullptr;
      }

      return DeserializationTraits::CreateAndWrapFileHandle(aCx, database,
                                                            file, data);
    }

    BlobOrFileData data;
    if (!ReadBlobOrFile(aReader, aTag, &data)) {
      return nullptr;
    }

    return DeserializationTraits::CreateAndWrapBlobOrFile(aCx, database,
                                                          file, data);
  }

  const JSStructuredCloneCallbacks* runtimeCallbacks =
    js::GetContextStructuredCloneCallbacks(aCx);

  if (runtimeCallbacks) {
    return runtimeCallbacks->read(aCx, aReader, aTag, aData, nullptr);
  }

  return nullptr;
}

} // anonymous namespace

const JSClass IDBObjectStore::sDummyPropJSClass = {
  "dummy", 0,
  JS_PropertyStub,  JS_DeletePropertyStub,
  JS_PropertyStub,  JS_StrictPropertyStub,
  JS_EnumerateStub, JS_ResolveStub,
  JS_ConvertStub
};

// static
already_AddRefed<IDBObjectStore>
IDBObjectStore::Create(IDBTransaction* aTransaction,
                       const ObjectStoreSpec& aSpec)
{
  MOZ_ASSERT(aTransaction);
  aTransaction->AssertIsOnOwningThread();

  nsRefPtr<IDBObjectStore> objectStore = new IDBObjectStore();

  objectStore->mTransaction = aTransaction;
  objectStore->mSpec = &aSpec;

  return objectStore.forget();
}

// static
nsresult
IDBObjectStore::AppendIndexUpdateInfo(
                                    int64_t aIndexID,
                                    const KeyPath& aKeyPath,
                                    bool aUnique,
                                    bool aMultiEntry,
                                    JSContext* aCx,
                                    JS::Handle<JS::Value> aVal,
                                    nsTArray<IndexUpdateInfo>& aUpdateInfoArray)
{
  nsresult rv;

  if (!aMultiEntry) {
    Key key;
    rv = aKeyPath.ExtractKey(aCx, aVal, key);

    // If an index's keyPath doesn't match an object, we ignore that object.
    if (rv == NS_ERROR_DOM_INDEXEDDB_DATA_ERR || key.IsUnset()) {
      return NS_OK;
    }

    if (NS_FAILED(rv)) {
      return rv;
    }

    IndexUpdateInfo* updateInfo = aUpdateInfoArray.AppendElement();
    updateInfo->indexId() = aIndexID;
    updateInfo->value() = key;

    return NS_OK;
  }

  JS::Rooted<JS::Value> val(aCx);
  if (NS_FAILED(aKeyPath.ExtractKeyAsJSVal(aCx, aVal, val.address()))) {
    return NS_OK;
  }

  if (JS_IsArrayObject(aCx, val)) {
    JS::Rooted<JSObject*> array(aCx, &val.toObject());
    uint32_t arrayLength;
    if (NS_WARN_IF(!JS_GetArrayLength(aCx, array, &arrayLength))) {
      IDB_REPORT_INTERNAL_ERR();
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    for (uint32_t arrayIndex = 0; arrayIndex < arrayLength; arrayIndex++) {
      JS::Rooted<JS::Value> arrayItem(aCx);
      if (NS_WARN_IF(!JS_GetElement(aCx, array, arrayIndex, &arrayItem))) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      Key value;
      if (NS_FAILED(value.SetFromJSVal(aCx, arrayItem)) ||
          value.IsUnset()) {
        // Not a value we can do anything with, ignore it.
        continue;
      }

      IndexUpdateInfo* updateInfo = aUpdateInfoArray.AppendElement();
      updateInfo->indexId() = aIndexID;
      updateInfo->value() = value;
    }
  }
  else {
    Key value;
    if (NS_FAILED(value.SetFromJSVal(aCx, val)) ||
        value.IsUnset()) {
      // Not a value we can do anything with, ignore it.
      return NS_OK;
    }

    IndexUpdateInfo* updateInfo = aUpdateInfoArray.AppendElement();
    updateInfo->indexId() = aIndexID;
    updateInfo->value() = value;
  }

  return NS_OK;
}

// static
nsresult
IDBObjectStore::UpdateIndexes(IDBTransaction* aTransaction,
                              int64_t aObjectStoreId,
                              const Key& aObjectStoreKey,
                              bool aOverwrite,
                              int64_t aObjectDataId,
                              const nsTArray<IndexUpdateInfo>& aUpdateInfoArray)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_LABEL("IndexedDB", "IDBObjectStore::UpdateIndexes");

  nsresult rv;

  NS_ASSERTION(aObjectDataId != INT64_MIN, "Bad objectData id!");

  NS_NAMED_LITERAL_CSTRING(objectDataId, "object_data_id");

  if (aOverwrite) {
    nsCOMPtr<mozIStorageStatement> deleteStmt =
      aTransaction->GetCachedStatement(
        "DELETE FROM unique_index_data "
        "WHERE object_data_id = :object_data_id; "
        "DELETE FROM index_data "
        "WHERE object_data_id = :object_data_id");
    NS_ENSURE_TRUE(deleteStmt, NS_ERROR_FAILURE);

    mozStorageStatementScoper scoper(deleteStmt);

    rv = deleteStmt->BindInt64ByName(objectDataId, aObjectDataId);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = deleteStmt->Execute();
    NS_ENSURE_SUCCESS(rv, rv);
  }

  // Avoid lots of hash lookups for objectStores with lots of indexes by lazily
  // holding the necessary statements on the stack outside the loop.
  nsCOMPtr<mozIStorageStatement> insertUniqueStmt;
  nsCOMPtr<mozIStorageStatement> insertStmt;

  uint32_t infoCount = aUpdateInfoArray.Length();
  for (uint32_t i = 0; i < infoCount; i++) {
    const IndexUpdateInfo& updateInfo = aUpdateInfoArray[i];

    MOZ_CRASH("Fix unique thing.");
    //nsCOMPtr<mozIStorageStatement>& stmt =
      //updateInfo.indexUnique ? insertUniqueStmt : insertStmt;
    nsCOMPtr<mozIStorageStatement>& stmt = insertStmt;

    if (!stmt) {
      MOZ_CRASH("Fix unique thing.");/*
      stmt = updateInfo.indexUnique ?
        aTransaction->GetCachedStatement(
          "INSERT INTO unique_index_data "
            "(index_id, object_data_id, object_data_key, value) "
          "VALUES (:index_id, :object_data_id, :object_data_key, :value)") :
        aTransaction->GetCachedStatement(
          "INSERT OR IGNORE INTO index_data ("
            "index_id, object_data_id, object_data_key, value) "
          "VALUES (:index_id, :object_data_id, :object_data_key, :value)");*/
    }
    NS_ENSURE_TRUE(stmt, NS_ERROR_FAILURE);

    mozStorageStatementScoper scoper(stmt);

    rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("index_id"),
                               updateInfo.indexId());
    NS_ENSURE_SUCCESS(rv, rv);

    rv = stmt->BindInt64ByName(objectDataId, aObjectDataId);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = aObjectStoreKey.BindToStatement(stmt,
                                         NS_LITERAL_CSTRING("object_data_key"));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = updateInfo.value().BindToStatement(stmt, NS_LITERAL_CSTRING("value"));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = stmt->Execute();

    MOZ_CRASH("Fix unique thing");/*
    if (rv == NS_ERROR_STORAGE_CONSTRAINT && updateInfo.indexUnique) {
      // If we're inserting multiple entries for the same unique index, then
      // we might have failed to insert due to colliding with another entry for
      // the same index in which case we should ignore it.
      
      for (int32_t j = (int32_t)i - 1;
           j >= 0 && aUpdateInfoArray[j].indexId == updateInfo.indexId;
           --j) {
        if (updateInfo.value == aUpdateInfoArray[j].value) {
          // We found a key with the same value for the same index. So we
          // must have had a collision with a value we just inserted.
          rv = NS_OK;
          break;
        }
      }
    }*/

    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  return NS_OK;
}

// static
nsresult
IDBObjectStore::GetStructuredCloneReadInfoFromStatement(
                                           mozIStorageStatement* aStatement,
                                           uint32_t aDataIndex,
                                           uint32_t aFileIdsIndex,
                                           IDBDatabase* aDatabase,
                                           StructuredCloneReadInfo& aInfo)
{
  MOZ_CRASH("Remove me!");
}

// static
void
IDBObjectStore::ClearCloneWriteInfo(StructuredCloneWriteInfo& aWriteInfo)
{
  // This is kind of tricky, we only want to release stuff on the main thread,
  // but we can end up being called on other threads if we have already been
  // cleared on the main thread.
  if (!aWriteInfo.mCloneBuffer.data() && !aWriteInfo.mFiles.Length()) {
    return;
  }

  // If there's something to clear, we should be on the main thread.
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  ClearStructuredCloneBuffer(aWriteInfo.mCloneBuffer);
  aWriteInfo.mFiles.Clear();
}

// static
void
IDBObjectStore::ClearCloneReadInfo(StructuredCloneReadInfo& aReadInfo)
{
  // This is kind of tricky, we only want to release stuff on the main thread,
  // but we can end up being called on other threads if we have already been
  // cleared on the main thread.
  if (!aReadInfo.mCloneBuffer.data() && !aReadInfo.mFiles.Length()) {
    return;
  }

  // If there's something to clear, we should be on the main thread.
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  ClearStructuredCloneBuffer(aReadInfo.mCloneBuffer);
  aReadInfo.mFiles.Clear();
}

// static
void
IDBObjectStore::ClearStructuredCloneBuffer(JSAutoStructuredCloneBuffer& aBuffer)
{
  if (aBuffer.data()) {
    aBuffer.clear();
  }
}

// static
bool
IDBObjectStore::DeserializeValue(JSContext* aCx,
                                 StructuredCloneReadInfo& aCloneReadInfo,
                                 JS::MutableHandle<JS::Value> aValue)
{
  MOZ_ASSERT(aCx);

  if (aCloneReadInfo.mData.IsEmpty()) {
    aValue.setUndefined();
    return true;
  }

  size_t dataLen = aCloneReadInfo.mData.Length();

  uint64_t* data =
    const_cast<uint64_t*>(reinterpret_cast<uint64_t*>(
      aCloneReadInfo.mData.Elements()));

  MOZ_ASSERT(!(dataLen % sizeof(*data)));

  JSAutoRequest ar(aCx);

  static JSStructuredCloneCallbacks callbacks = {
    CommonStructuredCloneReadCallback<ValueDeserializationHelper>,
    nullptr,
    nullptr
  };

  if (!JS_ReadStructuredClone(aCx, data, dataLen, JS_STRUCTURED_CLONE_VERSION,
                              aValue, &callbacks, &aCloneReadInfo)) {
    return false;
  }

  return true;
}

// static
bool
IDBObjectStore::SerializeValue(JSContext* aCx,
                               StructuredCloneWriteInfo& aCloneWriteInfo,
                               JS::Handle<JS::Value> aValue)
{
  MOZ_ASSERT(aCx);

  static JSStructuredCloneCallbacks callbacks = {
    nullptr,
    StructuredCloneWriteCallback,
    nullptr
  };

  JSAutoStructuredCloneBuffer& buffer = aCloneWriteInfo.mCloneBuffer;

  return buffer.write(aCx, aValue, &callbacks, &aCloneWriteInfo);
}

// static
bool
IDBObjectStore::DeserializeIndexValue(JSContext* aCx,
                                      StructuredCloneReadInfo& aCloneReadInfo,
                                      JS::MutableHandle<JS::Value> aValue)
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(aCx);

  if (aCloneReadInfo.mData.IsEmpty()) {
    aValue.setUndefined();
    return true;
  }

  size_t dataLen = aCloneReadInfo.mData.Length();

  uint64_t* data =
    const_cast<uint64_t*>(reinterpret_cast<uint64_t*>(
      aCloneReadInfo.mData.Elements()));

  MOZ_ASSERT(!(dataLen % sizeof(*data)));

  JSAutoRequest ar(aCx);

  static JSStructuredCloneCallbacks callbacks = {
    CommonStructuredCloneReadCallback<IndexDeserializationHelper>,
    nullptr,
    nullptr
  };

  if (!JS_ReadStructuredClone(aCx, data, dataLen, JS_STRUCTURED_CLONE_VERSION,
                              aValue, &callbacks, &aCloneReadInfo)) {
    return false;
  }

  return true;
}

// static
bool
IDBObjectStore::StructuredCloneWriteCallback(JSContext* aCx,
                                             JSStructuredCloneWriter* aWriter,
                                             JS::Handle<JSObject*> aObj,
                                             void* aClosure)
{
  MOZ_ASSERT(aCx);
  MOZ_ASSERT(aWriter);
  MOZ_ASSERT(aClosure);

  auto cloneWriteInfo = static_cast<StructuredCloneWriteInfo*>(aClosure);

  if (JS_GetClass(aObj) == &sDummyPropJSClass) {
    MOZ_ASSERT(!cloneWriteInfo->mOffsetToKeyProp);
    cloneWriteInfo->mOffsetToKeyProp = js_GetSCOffset(aWriter);

    uint64_t value = 0;
    // Omit endian swap
    return JS_WriteBytes(aWriter, &value, sizeof(value));
  }

  IDBTransaction* transaction = cloneWriteInfo->mTransaction;
  FileManager* fileManager = transaction->Database()->Manager();

  file::FileHandle* fileHandle = nullptr;
  if (NS_SUCCEEDED(UNWRAP_OBJECT(FileHandle, aObj, fileHandle))) {
    nsRefPtr<FileInfo> fileInfo = fileHandle->GetFileInfo();

    // Throw when trying to store non IDB file handles or IDB file handles
    // across databases.
    if (!fileInfo || fileInfo->Manager() != fileManager) {
      return false;
    }

    NS_ConvertUTF16toUTF8 convType(fileHandle->Type());
    uint32_t convTypeLength =
      NativeEndian::swapToLittleEndian(convType.Length());

    NS_ConvertUTF16toUTF8 convName(fileHandle->Name());
    uint32_t convNameLength =
      NativeEndian::swapToLittleEndian(convName.Length());

    if (!JS_WriteUint32Pair(aWriter, SCTAG_DOM_FILEHANDLE,
                            cloneWriteInfo->mFiles.Length()) ||
        !JS_WriteBytes(aWriter, &convTypeLength, sizeof(uint32_t)) ||
        !JS_WriteBytes(aWriter, convType.get(), convType.Length()) ||
        !JS_WriteBytes(aWriter, &convNameLength, sizeof(uint32_t)) ||
        !JS_WriteBytes(aWriter, convName.get(), convName.Length())) {
      return false;
    }

    StructuredCloneFile* file = cloneWriteInfo->mFiles.AppendElement();
    file->mFileInfo = fileInfo.forget();

    return true;
  }

  MOZ_ASSERT(NS_IsMainThread(), "This can't work off the main thread!");

  nsCOMPtr<nsIXPConnectWrappedNative> wrappedNative;
  nsContentUtils::XPConnect()->
    GetWrappedNativeOfJSObject(aCx, aObj, getter_AddRefs(wrappedNative));

  if (wrappedNative) {
    nsISupports* supports = wrappedNative->Native();

    nsCOMPtr<nsIDOMBlob> blob = do_QueryInterface(supports);
    if (blob) {
      nsCOMPtr<nsIInputStream> inputStream;

      // Check if it is a blob created from this db or the blob was already
      // stored in this db
      nsRefPtr<FileInfo> fileInfo = transaction->GetFileInfo(blob);
      if (!fileInfo && fileManager) {
        fileInfo = blob->GetFileInfo(fileManager);

        if (!fileInfo) {
          fileInfo = fileManager->GetNewFileInfo();
          if (!fileInfo) {
            NS_WARNING("Failed to get new file info!");
            return false;
          }

          if (NS_FAILED(blob->GetInternalStream(getter_AddRefs(inputStream)))) {
            NS_WARNING("Failed to get internal steam!");
            return false;
          }

          transaction->AddFileInfo(blob, fileInfo);
        }
      }

      uint64_t size;
      if (NS_FAILED(blob->GetSize(&size))) {
        NS_WARNING("Failed to get size!");
        return false;
      }
      size = NativeEndian::swapToLittleEndian(size);

      nsString type;
      if (NS_FAILED(blob->GetType(type))) {
        NS_WARNING("Failed to get type!");
        return false;
      }
      NS_ConvertUTF16toUTF8 convType(type);
      uint32_t convTypeLength =
        NativeEndian::swapToLittleEndian(convType.Length());

      nsCOMPtr<nsIDOMFile> file = do_QueryInterface(blob);

      if (!JS_WriteUint32Pair(aWriter, file ? SCTAG_DOM_FILE : SCTAG_DOM_BLOB,
                              cloneWriteInfo->mFiles.Length()) ||
          !JS_WriteBytes(aWriter, &size, sizeof(size)) ||
          !JS_WriteBytes(aWriter, &convTypeLength, sizeof(convTypeLength)) ||
          !JS_WriteBytes(aWriter, convType.get(), convType.Length())) {
        return false;
      }

      if (file) {
        uint64_t lastModifiedDate = 0;
        if (NS_FAILED(file->GetMozLastModifiedDate(&lastModifiedDate))) {
          NS_WARNING("Failed to get last modified date!");
          return false;
        }

        lastModifiedDate = NativeEndian::swapToLittleEndian(lastModifiedDate);

        nsString name;
        if (NS_FAILED(file->GetName(name))) {
          NS_WARNING("Failed to get name!");
          return false;
        }
        NS_ConvertUTF16toUTF8 convName(name);
        uint32_t convNameLength =
          NativeEndian::swapToLittleEndian(convName.Length());

        if (!JS_WriteBytes(aWriter, &lastModifiedDate, sizeof(lastModifiedDate)) || 
            !JS_WriteBytes(aWriter, &convNameLength, sizeof(convNameLength)) ||
            !JS_WriteBytes(aWriter, convName.get(), convName.Length())) {
          return false;
        }
      }

      StructuredCloneFile* cloneFile = cloneWriteInfo->mFiles.AppendElement();
      cloneFile->mFile = blob.forget();
      cloneFile->mFileInfo = fileInfo.forget();
      cloneFile->mInputStream = inputStream.forget();

      return true;
    }
  }

  // try using the runtime callbacks
  const JSStructuredCloneCallbacks* runtimeCallbacks =
    js::GetContextStructuredCloneCallbacks(aCx);
  if (runtimeCallbacks) {
    return runtimeCallbacks->write(aCx, aWriter, aObj, nullptr);
  }

  return false;
}

// static
nsresult
IDBObjectStore::ConvertFileIdsToArray(const nsAString& aFileIds,
                                      nsTArray<int64_t>& aResult)
{
  MOZ_CRASH("Remove me!");
}

// static
void
IDBObjectStore::ConvertActorsToBlobs(
                                   const nsTArray<PBlobChild*>& aActors,
                                   nsTArray<StructuredCloneFile>& aFiles)
{
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aFiles.IsEmpty(), "Should be empty!");

  if (!aActors.IsEmpty()) {
    NS_ASSERTION(ContentChild::GetSingleton(), "This should never be null!");

    uint32_t length = aActors.Length();
    aFiles.SetCapacity(length);

    for (uint32_t index = 0; index < length; index++) {
      BlobChild* actor = static_cast<BlobChild*>(aActors[index]);

      StructuredCloneFile* file = aFiles.AppendElement();
      file->mFile = actor->GetBlob();
    }
  }
}

// static
nsresult
IDBObjectStore::ConvertBlobsToActors(
                                    ContentParent* aContentParent,
                                    FileManager* aFileManager,
                                    const nsTArray<StructuredCloneFile>& aFiles,
                                    nsTArray<PBlobParent*>& aActors)
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aContentParent, "Null contentParent!");
  NS_ASSERTION(aFileManager, "Null file manager!");

  if (!aFiles.IsEmpty()) {
    nsCOMPtr<nsIFile> directory = aFileManager->GetDirectory();
    if (!directory) {
      IDB_WARNING("Failed to get directory!");
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    uint32_t fileCount = aFiles.Length();
    aActors.SetCapacity(fileCount);

    for (uint32_t index = 0; index < fileCount; index++) {
      const StructuredCloneFile& file = aFiles[index];
      NS_ASSERTION(file.mFileInfo, "This should never be null!");

      nsCOMPtr<nsIFile> nativeFile =
        aFileManager->GetFileForId(directory, file.mFileInfo->Id());
      if (!nativeFile) {
        IDB_WARNING("Failed to get file!");
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      nsCOMPtr<nsIDOMBlob> blob = new nsDOMFileFile(nativeFile, file.mFileInfo);

      BlobParent* actor =
        aContentParent->GetOrCreateActorForBlob(blob);
      if (!actor) {
        // This can only fail if the child has crashed.
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      aActors.AppendElement(actor);
    }
  }

  return NS_OK;
}

IDBObjectStore::IDBObjectStore()
  : mCachedKeyPath(JSVAL_VOID)
  , mSpec(nullptr)
  , mRooted(false)
{
  SetIsDOMBinding();
}

IDBObjectStore::~IDBObjectStore()
{
  AssertIsOnOwningThread();

  if (mRooted) {
    mCachedKeyPath = JSVAL_VOID;
    mozilla::DropJSObjects(this);
  }
}

#ifdef DEBUG

void
IDBObjectStore::AssertIsOnOwningThread() const
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnOwningThread();
}

#endif // DEBUG

nsresult
IDBObjectStore::GetAddInfo(JSContext* aCx,
                           JS::Handle<JS::Value> aValue,
                           JS::Handle<JS::Value> aKeyVal,
                           StructuredCloneWriteInfo& aCloneWriteInfo,
                           Key& aKey,
                           nsTArray<IndexUpdateInfo>& aUpdateInfoArray)
{
  // Return DATA_ERR if a key was passed in and this objectStore uses inline
  // keys.
  if (!aKeyVal.isUndefined() && HasValidKeyPath()) {
    return NS_ERROR_DOM_INDEXEDDB_DATA_ERR;
  }

  bool isAutoIncrement = AutoIncrement();

  nsresult rv;

  if (!HasValidKeyPath()) {
    // Out-of-line keys must be passed in.
    rv = aKey.SetFromJSVal(aCx, aKeyVal);
    if (NS_FAILED(rv)) {
      return rv;
    }
  } else if (!isAutoIncrement) {
    rv = GetKeyPath().ExtractKey(aCx, aValue, aKey);
    if (NS_FAILED(rv)) {
      return rv;
    }
  }

  // Return DATA_ERR if no key was specified this isn't an autoIncrement
  // objectStore.
  if (aKey.IsUnset() && !isAutoIncrement) {
    return NS_ERROR_DOM_INDEXEDDB_DATA_ERR;
  }

  // Figure out indexes and the index values to update here.
  const nsTArray<IndexMetadata>& indexes = mSpec->indexes();

  const uint32_t idxCount = indexes.Length();
  aUpdateInfoArray.SetCapacity(idxCount); // Pretty good estimate

  for (uint32_t idxIndex = 0; idxIndex < idxCount; idxIndex++) {
    const IndexMetadata& metadata = indexes[idxIndex];

    rv = AppendIndexUpdateInfo(metadata.id(), metadata.keyPath(),
                               metadata.unique(), metadata.multiEntry(), aCx,
                               aValue, aUpdateInfoArray);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  GetAddInfoClosure data = { this, aCloneWriteInfo, aValue };

  if (isAutoIncrement && HasValidKeyPath()) {
    MOZ_ASSERT(aKey.IsUnset());

    rv = GetKeyPath().ExtractOrCreateKey(aCx, aValue, aKey, &GetAddInfoCallback,
                                         &data);
  } else {
    rv = GetAddInfoCallback(aCx, &data);
  }

  return rv;
}

already_AddRefed<IDBRequest>
IDBObjectStore::AddOrPut(JSContext* aCx,
                         JS::Handle<JS::Value> aValue,
                         JS::Handle<JS::Value> aKey,
                         bool aOverwrite,
                         ErrorResult& aRv)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(aCx);

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  JS::Rooted<JS::Value> value(aCx, aValue);
  Key key;
  StructuredCloneWriteInfo cloneWriteInfo;
  nsTArray<IndexUpdateInfo> updateInfo;

  aRv = GetAddInfo(aCx, value, aKey, cloneWriteInfo, key, updateInfo);
  if (aRv.Failed()) {
    return nullptr;
  }

  FallibleTArray<uint8_t> cloneData;
  if (NS_WARN_IF(!cloneData.SetLength(cloneWriteInfo.mCloneBuffer.nbytes()))) {
    aRv = NS_ERROR_OUT_OF_MEMORY;
    return nullptr;
  }

  memcpy(cloneData.Elements(), cloneWriteInfo.mCloneBuffer.data(),
         cloneWriteInfo.mCloneBuffer.nbytes());

  cloneWriteInfo.mCloneBuffer.clear();

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  ObjectStoreAddPutParams commonParams;
  commonParams.objectStoreId() = Id();
  commonParams.cloneInfo().data().SwapElements(cloneData);
  commonParams.cloneInfo().offsetToKeyProp() = cloneWriteInfo.mOffsetToKeyProp;
  commonParams.key() = key;
  commonParams.indexUpdateInfos().SwapElements(updateInfo);

  RequestParams params;
  if (aOverwrite) {
    params = ObjectStorePutParams(commonParams);
  } else {
    params = ObjectStoreAddParams(commonParams);
  }

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

#ifdef IDB_PROFILER_USE_MARKS
  if (aOverwrite) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).%s(%s)",
                      "IDBRequest[%llu] MT IDBObjectStore.put()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
  else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).add(%s)",
                      "IDBRequest[%llu] MT IDBObjectStore.add()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
#endif

  return request.forget();
}

nsresult
IDBObjectStore::AddOrPutInternal(
                      const SerializedStructuredCloneWriteInfo& aCloneWriteInfo,
                      const Key& aKey,
                      const nsTArray<IndexUpdateInfo>& aUpdateInfoArray,
                      const nsTArray<nsCOMPtr<nsIDOMBlob> >& aBlobs,
                      bool aOverwrite,
                      IDBRequest** _retval)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  if (!mTransaction->IsOpen()) {
    return NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR;
  }

  if (!IsWriteAllowed()) {
    return NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  StructuredCloneWriteInfo cloneWriteInfo;
  if (!cloneWriteInfo.SetFromSerialized(aCloneWriteInfo)) {
    IDB_WARNING("Failed to copy structured clone buffer!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  if (!aBlobs.IsEmpty()) {
    FileManager* fileManager = Transaction()->Database()->Manager();
    NS_ASSERTION(fileManager, "Null file manager?!");

    uint32_t length = aBlobs.Length();
    cloneWriteInfo.mFiles.SetCapacity(length);

    for (uint32_t index = 0; index < length; index++) {
      const nsCOMPtr<nsIDOMBlob>& blob = aBlobs[index];

      nsCOMPtr<nsIInputStream> inputStream;

      nsRefPtr<FileInfo> fileInfo = Transaction()->GetFileInfo(blob);
      if (!fileInfo) {
        fileInfo = blob->GetFileInfo(fileManager);

        if (!fileInfo) {
          fileInfo = fileManager->GetNewFileInfo();
          if (!fileInfo) {
            IDB_WARNING("Failed to get new file info!");
            return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
          }

          if (NS_FAILED(blob->GetInternalStream(getter_AddRefs(inputStream)))) {
            IDB_WARNING("Failed to get internal steam!");
            return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
          }

          // XXXbent This is where we should send a message back to the child to
          //         update the file id.

          Transaction()->AddFileInfo(blob, fileInfo);
        }
      }

      StructuredCloneFile* file = cloneWriteInfo.mFiles.AppendElement();
      file->mFile = blob;
      file->mFileInfo.swap(fileInfo);
      file->mInputStream.swap(inputStream);
    }
  }

  Key key(aKey);

  nsTArray<IndexUpdateInfo> updateInfo(aUpdateInfoArray);

  nsRefPtr<AddHelper> helper =
    new AddHelper(mTransaction, request, this, Move(cloneWriteInfo), key,
                  aOverwrite, updateInfo);

  nsresult rv = helper->DispatchToTransactionPool();
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

#ifdef IDB_PROFILER_USE_MARKS
  if (aOverwrite) {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).%s(%s)",
                      "IDBRequest[%llu] MT IDBObjectStore.put()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
  else {
    IDB_PROFILER_MARK("IndexedDB Request %llu: "
                      "database(%s).transaction(%s).objectStore(%s).add(%s)",
                      "IDBRequest[%llu] MT IDBObjectStore.add()",
                      request->GetSerialNumber(),
                      IDB_PROFILER_STRING(Transaction()->Database()),
                      IDB_PROFILER_STRING(Transaction()),
                      IDB_PROFILER_STRING(this),
                      key.IsUnset() ? "" : IDB_PROFILER_STRING(key));
  }
#endif

  request.forget(_retval);
  return NS_OK;
  */
}

already_AddRefed<IDBRequest>
IDBObjectStore::GetAllInternal(IDBKeyRange* aKeyRange,
                               uint32_t aLimit, ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  nsRefPtr<GetAllHelper> helper =
    new GetAllHelper(mTransaction, request, this, aKeyRange, aLimit);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "getAll(%s, %lu)",
                    "IDBRequest[%llu] MT IDBObjectStore.getAll()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                    aLimit);

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::GetAllKeysInternal(IDBKeyRange* aKeyRange, uint32_t aLimit,
                                   ErrorResult& aRv)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  nsRefPtr<GetAllKeysHelper> helper =
    new GetAllKeysHelper(mTransaction, request, this, aKeyRange, aLimit);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "getAllKeys(%s, %lu)",
                    "IDBRequest[%llu] MT IDBObjectStore.getAllKeys()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                    aLimit);

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::DeleteInternal(IDBKeyRange* aKeyRange,
                               ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(aKeyRange, "Null key range!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  nsRefPtr<DeleteHelper> helper =
    new DeleteHelper(mTransaction, request, this, aKeyRange);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).delete(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.delete()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::Clear(ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  nsRefPtr<ClearHelper> helper(new ClearHelper(mTransaction, request, this));

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).clear()",
                    "IDBRequest[%llu] MT IDBObjectStore.clear()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::CountInternal(IDBKeyRange* aKeyRange, ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  nsRefPtr<CountHelper> helper =
    new CountHelper(mTransaction, request, this, aKeyRange);
  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).count(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.count()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::OpenCursorInternal(IDBKeyRange* aKeyRange,
                                   size_t aDirection, ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  IDBCursor::Direction direction =
    static_cast<IDBCursor::Direction>(aDirection);

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  nsRefPtr<OpenCursorHelper> helper =
    new OpenCursorHelper(mTransaction, request, this, aKeyRange, direction);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "openCursor(%s, %s)",
                    "IDBRequest[%llu] MT IDBObjectStore.openCursor()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                    IDB_PROFILER_STRING(direction));

  return request.forget();
}

nsresult
IDBObjectStore::OpenCursorFromChildProcess(
                            IDBRequest* aRequest,
                            size_t aDirection,
                            const Key& aKey,
                            const SerializedStructuredCloneReadInfo& aCloneInfo,
                            nsTArray<StructuredCloneFile>& aBlobs,
                            IDBCursor** _retval)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  IDBCursor::Direction direction =
    static_cast<IDBCursor::Direction>(aDirection);

  StructuredCloneReadInfo cloneInfo;

  if (!cloneInfo.SetFromSerialized(aCloneInfo)) {
    IDB_WARNING("Failed to copy clone buffer!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  cloneInfo.mFiles.SwapElements(aBlobs);

  nsRefPtr<IDBCursor> cursor =
    IDBCursor::Create(aRequest, mTransaction, this, direction, Key(),
                      EmptyCString(), EmptyCString(), aKey, Move(cloneInfo));
  IDB_ENSURE_TRUE(cursor, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  NS_ASSERTION(!cloneInfo.mCloneBuffer.data(), "Should have swapped!");

  cursor.forget(_retval);
  return NS_OK;
}

nsresult
IDBObjectStore::OpenCursorFromChildProcess(IDBRequest* aRequest,
                                           size_t aDirection,
                                           const Key& aKey,
                                           IDBCursor** _retval)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(aRequest);

  auto direction = static_cast<IDBCursor::Direction>(aDirection);

  nsRefPtr<IDBCursor> cursor =
    IDBCursor::Create(aRequest, mTransaction, this, direction, Key(),
                      EmptyCString(), EmptyCString(), aKey);
  IDB_ENSURE_TRUE(cursor, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  cursor.forget(_retval);
  return NS_OK;
}

already_AddRefed<IDBRequest>
IDBObjectStore::OpenKeyCursorInternal(IDBKeyRange* aKeyRange, size_t aDirection,
                                      ErrorResult& aRv)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  auto direction = static_cast<IDBCursor::Direction>(aDirection);

  nsRefPtr<OpenKeyCursorHelper> helper =
    new OpenKeyCursorHelper(mTransaction, request, this, aKeyRange, direction);

  nsresult rv = helper->DispatchToTransactionPool();
  if (NS_FAILED(rv)) {
    IDB_WARNING("Failed to dispatch!");
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return nullptr;
  }

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "openKeyCursor(%s, %s)",
                    "IDBRequest[%llu] MT IDBObjectStore.openKeyCursor()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange),
                    IDB_PROFILER_STRING(direction));

  return request.forget();
}

already_AddRefed<IDBIndex>
IDBObjectStore::Index(const nsAString& aName, ErrorResult &aRv)
{
  AssertIsOnOwningThread();

  if (mTransaction->IsFinished()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  const nsTArray<IndexMetadata>& indexes = mSpec->indexes();

  const IndexMetadata* metadata = nullptr;

  for (uint32_t idxCount = indexes.Length(), idxIndex = 0;
       idxIndex < idxCount;
       idxIndex++) {
    const IndexMetadata& index = indexes[idxIndex];
    if (index.name() == aName) {
      metadata = &index;
      break;
    }
  }

  if (!metadata) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return nullptr;
  }

  const int64_t desiredId = metadata->id();

  nsRefPtr<IDBIndex> index;

  for (uint32_t idxCount = mIndexes.Length(), idxIndex = 0;
       idxIndex < idxCount;
       idxIndex++) {
    nsRefPtr<IDBIndex>& existingIndex = mIndexes[idxIndex];

    if (existingIndex->Id() == desiredId) {
      index = existingIndex;
      break;
    }
  }

  if (!index) {
    index = IDBIndex::Create(this, *metadata);
    MOZ_ASSERT(index);

    mIndexes.AppendElement(index);
  }

  return index.forget();
}

NS_IMPL_CYCLE_COLLECTION_CLASS(IDBObjectStore)

NS_IMPL_CYCLE_COLLECTION_TRACE_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRACE_PRESERVED_WRAPPER
  NS_IMPL_CYCLE_COLLECTION_TRACE_JSVAL_MEMBER_CALLBACK(mCachedKeyPath)
NS_IMPL_CYCLE_COLLECTION_TRACE_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE_SCRIPT_OBJECTS
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mTransaction)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mIndexes);
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(IDBObjectStore)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER

  // Don't unlink mTransaction!

  NS_IMPL_CYCLE_COLLECTION_UNLINK(mIndexes);

  tmp->mCachedKeyPath = JSVAL_VOID;

  if (tmp->mRooted) {
    mozilla::DropJSObjects(tmp);
    tmp->mRooted = false;
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(IDBObjectStore)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(IDBObjectStore)
NS_IMPL_CYCLE_COLLECTING_RELEASE(IDBObjectStore)

JSObject*
IDBObjectStore::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return IDBObjectStoreBinding::Wrap(aCx, aScope, this);
}

nsPIDOMWindow*
IDBObjectStore::GetParentObject() const
{
  return mTransaction->GetParentObject();
}

JS::Value
IDBObjectStore::GetKeyPath(JSContext* aCx, ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!JSVAL_IS_VOID(mCachedKeyPath)) {
    return mCachedKeyPath;
  }

  aRv = GetKeyPath().ToJSVal(aCx, mCachedKeyPath);
  ENSURE_SUCCESS(aRv, JSVAL_VOID);

  if (JSVAL_IS_GCTHING(mCachedKeyPath)) {
    mozilla::HoldJSObjects(this);
    mRooted = true;
  }

  return mCachedKeyPath;
}

already_AddRefed<DOMStringList>
IDBObjectStore::IndexNames()
{
  AssertIsOnOwningThread();

  const nsTArray<IndexMetadata>& indexes = mSpec->indexes();

  nsRefPtr<DOMStringList> list = new DOMStringList();

  if (!indexes.IsEmpty()) {
    nsTArray<nsString>& listNames = list->StringArray();
    listNames.SetCapacity(indexes.Length());

    for (uint32_t index = 0; index < indexes.Length(); index++) {
      listNames.InsertElementSorted(indexes[index].name());
    }
  }

  return list.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::Get(JSContext* aCx,
                    JS::Handle<JS::Value> aKey,
                    ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (aRv.Failed()) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for get().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  ObjectStoreGetParams params;
  params.objectStoreId() = Id();
  keyRange->ToSerialized(params.keyRange());

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).get(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.get()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::GetAll(JSContext* aCx,
                       JS::Handle<JS::Value> aKey,
                       const Optional<uint32_t>& aLimit, ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  uint32_t limit = UINT32_MAX;
  if (aLimit.WasPassed() && aLimit.Value() != 0) {
    limit = aLimit.Value();
  }

  return GetAllInternal(keyRange, limit, aRv);
}

already_AddRefed<IDBRequest>
IDBObjectStore::Delete(JSContext* aCx,
                       JS::Handle<JS::Value> aKey,
                       ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  if (!IsWriteAllowed()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_READ_ONLY_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  if (NS_WARN_IF((aRv.Failed()))) {
    return nullptr;
  }

  if (!keyRange) {
    // Must specify a key or keyRange for delete().
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_DATA_ERR);
    return nullptr;
  }

  nsRefPtr<IDBRequest> request = GenerateRequest(this);
  MOZ_ASSERT(request);

  ObjectStoreDeleteParams params;
  params.objectStoreId() = Id();
  keyRange->ToSerialized(params.keyRange());

  BackgroundRequestChild* actor = new BackgroundRequestChild(request);

  mTransaction->StartRequest(actor, params);

  IDB_PROFILER_MARK("IndexedDB Request %llu: "
                    "database(%s).transaction(%s).objectStore(%s).delete(%s)",
                    "IDBRequest[%llu] MT IDBObjectStore.delete()",
                    request->GetSerialNumber(),
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(aKeyRange));

  return request.forget();
}

already_AddRefed<IDBRequest>
IDBObjectStore::OpenCursor(JSContext* aCx,
                           JS::Handle<JS::Value> aRange,
                           IDBCursorDirection aDirection, ErrorResult& aRv)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aRange, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  IDBCursor::Direction direction = IDBCursor::ConvertDirection(aDirection);
  size_t argDirection = static_cast<size_t>(direction);

  return OpenCursorInternal(keyRange, argDirection, aRv);
}

already_AddRefed<IDBIndex>
IDBObjectStore::CreateIndex(JSContext* aCx,
                            const nsAString& aName,
                            const nsAString& aKeyPath,
                            const IDBIndexParameters& aOptionalParameters,
                            ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  KeyPath keyPath(0);
  if (NS_FAILED(KeyPath::Parse(aCx, aKeyPath, &keyPath)) ||
      !keyPath.IsValid()) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return nullptr;
  }

  return CreateIndexInternal(aCx, aName, keyPath, aOptionalParameters, aRv);
}

already_AddRefed<IDBIndex>
IDBObjectStore::CreateIndex(JSContext* aCx,
                            const nsAString& aName,
                            const Sequence<nsString >& aKeyPath,
                            const IDBIndexParameters& aOptionalParameters,
                            ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  KeyPath keyPath(0);
  if (aKeyPath.IsEmpty() ||
      NS_FAILED(KeyPath::Parse(aCx, aKeyPath, &keyPath)) ||
      !keyPath.IsValid()) {
    aRv.Throw(NS_ERROR_DOM_SYNTAX_ERR);
    return nullptr;
  }

  return CreateIndexInternal(aCx, aName, keyPath, aOptionalParameters, aRv);
}

already_AddRefed<IDBIndex>
IDBObjectStore::CreateIndexInternal(
                                  JSContext* aCx,
                                  const nsAString& aName,
                                  const KeyPath& aKeyPath,
                                  const IDBIndexParameters& aOptionalParameters,
                                  ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = IDBTransaction::GetCurrent();

  if (!transaction ||
      transaction != mTransaction ||
      mTransaction->GetMode() != IDBTransaction::VERSION_CHANGE) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return nullptr;
  }

  MOZ_ASSERT(transaction->IsOpen());

  auto& indexes = const_cast<nsTArray<IndexMetadata>&>(mSpec->indexes());
  for (uint32_t count = indexes.Length(), index = 0;
       index < count;
       index++) {
    if (aName == indexes[index].name()) {
      aRv.Throw(NS_ERROR_DOM_INDEXEDDB_CONSTRAINT_ERR);
      return nullptr;
    }
  }

  if (aOptionalParameters.mMultiEntry && aKeyPath.IsArray()) {
    aRv.Throw(NS_ERROR_DOM_INVALID_ACCESS_ERR);
    return nullptr;
  }

#ifdef DEBUG
  for (uint32_t count = mIndexes.Length(), index = 0;
       index < count;
       index++) {
    MOZ_ASSERT(mIndexes[index]->Name() != aName);
  }
#endif

  IndexMetadata* metadata = indexes.AppendElement(
    IndexMetadata(transaction->NextIndexId(), nsString(aName), aKeyPath,
                  aOptionalParameters.mUnique,
                  aOptionalParameters.mMultiEntry));

  transaction->CreateIndex(this, *metadata);

  nsRefPtr<IDBIndex> index = IDBIndex::Create(this, *metadata);
  MOZ_ASSERT(index);

  mIndexes.AppendElement(index);

  IDB_PROFILER_MARK("IndexedDB Pseudo-request: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "createIndex(%s)",
                    "MT IDBObjectStore.createIndex()",
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this), IDB_PROFILER_STRING(index));

  return index.forget();
}

void
IDBObjectStore::DeleteIndex(const nsAString& aName, ErrorResult& aRv)
{
  AssertIsOnOwningThread();

  IDBTransaction* transaction = IDBTransaction::GetCurrent();

  if (!transaction ||
      transaction != mTransaction ||
      mTransaction->GetMode() != IDBTransaction::VERSION_CHANGE) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_ALLOWED_ERR);
    return;
  }

  MOZ_ASSERT(transaction->IsOpen());

  auto& indexes = const_cast<nsTArray<IndexMetadata>&>(mSpec->indexes());

  int64_t foundId = 0;

  for (uint32_t count = indexes.Length(), index = 0;
       index < count;
       index++) {
    const IndexMetadata& metadata = indexes[index];

    if (aName == metadata.name()) {
      foundId = metadata.id();
      indexes.RemoveElementAt(index);
      break;
    }
  }

  if (!foundId) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_NOT_FOUND_ERR);
    return;
  }

  transaction->DeleteIndex(this, foundId);

  IDB_PROFILER_MARK("IndexedDB Pseudo-request: "
                    "database(%s).transaction(%s).objectStore(%s)."
                    "deleteIndex(\"%s\")",
                    "MT IDBObjectStore.deleteIndex()",
                    IDB_PROFILER_STRING(Transaction()->Database()),
                    IDB_PROFILER_STRING(Transaction()),
                    IDB_PROFILER_STRING(this),
                    NS_ConvertUTF16toUTF8(aName).get());
}

already_AddRefed<IDBRequest>
IDBObjectStore::Count(JSContext* aCx,
                      JS::Handle<JS::Value> aKey,
                      ErrorResult& aRv)
{
  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  return CountInternal(keyRange, aRv);
}

already_AddRefed<IDBRequest>
IDBObjectStore::GetAllKeys(JSContext* aCx,
                           JS::Handle<JS::Value> aKey,
                           const Optional<uint32_t>& aLimit, ErrorResult& aRv)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aKey, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  uint32_t limit = UINT32_MAX;
  if (aLimit.WasPassed() && aLimit.Value() != 0) {
    limit = aLimit.Value();
  }

  return GetAllKeysInternal(keyRange, limit, aRv);
}

already_AddRefed<IDBRequest>
IDBObjectStore::OpenKeyCursor(JSContext* aCx,
                              JS::Handle<JS::Value> aRange,
                              IDBCursorDirection aDirection, ErrorResult& aRv)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (!mTransaction->IsOpen()) {
    aRv.Throw(NS_ERROR_DOM_INDEXEDDB_TRANSACTION_INACTIVE_ERR);
    return nullptr;
  }

  nsRefPtr<IDBKeyRange> keyRange;
  aRv = IDBKeyRange::FromJSVal(aCx, aRange, getter_AddRefs(keyRange));
  ENSURE_SUCCESS(aRv, nullptr);

  IDBCursor::Direction direction = IDBCursor::ConvertDirection(aDirection);

  return OpenKeyCursorInternal(keyRange, static_cast<size_t>(direction), aRv);
}

void
IDBObjectStore::RefreshSpec()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);
  MOZ_ASSERT(mSpec != mDeletedSpec);

  const DatabaseSpec* dbSpec = mTransaction->Database()->Spec();
  MOZ_ASSERT(dbSpec);

  const nsTArray<ObjectStoreSpec>& objectStores = dbSpec->objectStores();

  DebugOnly<bool> found = false;

  for (uint32_t objCount = objectStores.Length(), objIndex = 0;
       objIndex < objCount;
       objIndex++) {
    const ObjectStoreSpec& objSpec = objectStores[objIndex];

    if (objSpec.metadata().id() == Id()) {
      MOZ_ASSERT(objSpec.metadata().name() == Name());

      mSpec = &objSpec;

      for (uint32_t idxCount = mIndexes.Length(), idxIndex = 0;
           idxIndex < idxCount;
           idxIndex++) {
        mIndexes[idxIndex]->RefreshMetadata();
      }

      found = true;
      break;
    }
  }

  MOZ_ASSERT(found);
}

const ObjectStoreSpec&
IDBObjectStore::Spec() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return *mSpec;
}

void
IDBObjectStore::NoteDeletion()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);
  MOZ_ASSERT(!mDeletedSpec);

  // Copy the spec here.
  mDeletedSpec = new ObjectStoreSpec(*mSpec);
  mDeletedSpec->indexes().Clear();

  mSpec = mDeletedSpec;

  if (!mIndexes.IsEmpty()) {
    for (uint32_t count = mIndexes.Length(), index = 0;
         index < count;
         index++) {
      mIndexes[index]->NoteDeletion();
    }

    mIndexes.Clear();
  }
}

const nsString&
IDBObjectStore::Name() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().name();
}

bool
IDBObjectStore::AutoIncrement() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().autoIncrement();
}

bool
IDBObjectStore::IsWriteAllowed() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mTransaction);

  return mTransaction->IsWriteAllowed();
}

int64_t
IDBObjectStore::Id() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().id();
}

const KeyPath&
IDBObjectStore::GetKeyPath() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().keyPath();
}

bool
IDBObjectStore::HasValidKeyPath() const
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mSpec);

  return mSpec->metadata().keyPath().IsValid();
}

inline nsresult
CopyData(nsIInputStream* aInputStream, nsIOutputStream* aOutputStream)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_LABEL("IndexedDB", "CopyData");

  nsresult rv;

  do {
    char copyBuffer[FILE_COPY_BUFFER_SIZE];

    uint32_t numRead;
    rv = aInputStream->Read(copyBuffer, sizeof(copyBuffer), &numRead);
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

    if (!numRead) {
      break;
    }

    uint32_t numWrite;
    rv = aOutputStream->Write(copyBuffer, numRead, &numWrite);
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

    if (numWrite < numRead) {
      // Must have hit the quota limit.
      return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
    }
  } while (true);

  rv = aOutputStream->Flush();
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

void
ObjectStoreHelper::ReleaseMainThreadObjects()
{
  mObjectStore = nullptr;
  AsyncConnectionHelper::ReleaseMainThreadObjects();
}

nsresult
ObjectStoreHelper::Dispatch(nsIEventTarget* aDatabaseThread)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB", "ObjectStoreHelper::Dispatch");

  if (IndexedDatabaseManager::IsMainProcess()) {
    return AsyncConnectionHelper::Dispatch(aDatabaseThread);
  }

  // If we've been invalidated then there's no point sending anything to the
  // parent process.
  if (mObjectStore->Transaction()->Database()->IsInvalidated()) {
    IDB_REPORT_INTERNAL_ERR();
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBObjectStoreChild* objectStoreActor = mObjectStore->GetActorChild();
  NS_ASSERTION(objectStoreActor, "Must have an actor here!");

  ObjectStoreRequestParams params;
  nsresult rv = PackArgumentsForParentProcess(params);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  NoDispatchEventTarget target;
  rv = AsyncConnectionHelper::Dispatch(&target);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mActor =
    new IndexedDBObjectStoreRequestChild(this, mObjectStore, params.type());
  objectStoreActor->SendPIndexedDBRequestConstructor(mActor, params);
  */
  return NS_OK;
}

void
NoRequestObjectStoreHelper::ReleaseMainThreadObjects()
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  mObjectStore = nullptr;
  AsyncConnectionHelper::ReleaseMainThreadObjects();
}

nsresult
NoRequestObjectStoreHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH();
}

AsyncConnectionHelper::ChildProcessSendResult
NoRequestObjectStoreHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  return Success_NotSent;
}

nsresult
NoRequestObjectStoreHelper::OnSuccess()
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  return NS_OK;
}

void
NoRequestObjectStoreHelper::OnError()
{
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  mTransaction->Abort(GetResultCode());
}

// This is a duplicate of the js engine's byte munging in StructuredClone.cpp
uint64_t
ReinterpretDoubleAsUInt64(double d)
{
  union {
    double d;
    uint64_t u;
  } pun;
  pun.d = d;
  return pun.u;
}

nsresult
AddHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(aConnection, "Passed a null connection!");

  PROFILER_LABEL("IndexedDB", "AddHelper::DoDatabaseWork");

  if (IndexedDatabaseManager::InLowDiskSpaceMode()) {
    NS_WARNING("Refusing to add more data because disk space is low!");
    return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
  }

  nsresult rv;
  bool keyUnset = mKey.IsUnset();
  int64_t osid = mObjectStore->Id();
  const KeyPath& keyPath = mObjectStore->GetKeyPath();

  // The "|| keyUnset" here is mostly a debugging tool. If a key isn't
  // specified we should never have a collision and so it shouldn't matter
  // if we allow overwrite or not. By not allowing overwrite we raise
  // detectable errors rather than corrupting data
  nsCOMPtr<mozIStorageStatement> stmt = !mOverwrite || keyUnset ?
    mTransaction->GetCachedStatement(
      "INSERT INTO object_data (object_store_id, key_value, data, file_ids) "
      "VALUES (:osid, :key_value, :data, :file_ids)") :
    mTransaction->GetCachedStatement(
      "INSERT OR REPLACE INTO object_data (object_store_id, key_value, data, "
                                          "file_ids) "
      "VALUES (:osid, :key_value, :data, :file_ids)");
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"), osid);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  NS_ASSERTION(!keyUnset || mObjectStore->AutoIncrement(),
               "Should have key unless autoincrement");

  int64_t autoIncrementNum = 0;

  if (mObjectStore->AutoIncrement()) {
    if (keyUnset) {
      autoIncrementNum = mObjectStore->Info()->nextAutoIncrementId;

      MOZ_ASSERT(autoIncrementNum > 0,
                 "Generated key must always be a positive integer");

      if (autoIncrementNum > (1LL << 53)) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      mKey.SetFromInteger(autoIncrementNum);
    }
    else if (mKey.IsFloat() &&
             mKey.ToFloat() >= mObjectStore->Info()->nextAutoIncrementId) {
      autoIncrementNum = floor(mKey.ToFloat());
    }

    if (keyUnset && keyPath.IsValid()) {
      // Special case where someone put an object into an autoIncrement'ing
      // objectStore with no key in its keyPath set. We needed to figure out
      // which row id we would get above before we could set that properly.

      LittleEndian::writeUint64((char*)mCloneWriteInfo.mCloneBuffer.data() +
                                mCloneWriteInfo.mOffsetToKeyProp,
                                ReinterpretDoubleAsUInt64(static_cast<double>(
                                                          autoIncrementNum)));
    }
  }

  mKey.BindToStatement(stmt, NS_LITERAL_CSTRING("key_value"));


  // Compress the bytes before adding into the database.
  const char* uncompressed =
    reinterpret_cast<const char*>(mCloneWriteInfo.mCloneBuffer.data());
  size_t uncompressedLength = mCloneWriteInfo.mCloneBuffer.nbytes();

  // We don't have a smart pointer class that calls moz_free, so we need to
  // manage | compressed | manually.
  {
    size_t compressedLength = snappy::MaxCompressedLength(uncompressedLength);
    // moz_malloc is equivalent to NS_Alloc, which we use because mozStorage
    // expects to be able to free the adopted pointer with NS_Free.
    char* compressed = (char*)moz_malloc(compressedLength);
    NS_ENSURE_TRUE(compressed, NS_ERROR_OUT_OF_MEMORY);

    snappy::RawCompress(uncompressed, uncompressedLength, compressed,
                        &compressedLength);

    uint8_t* dataBuffer = reinterpret_cast<uint8_t*>(compressed);
    size_t dataBufferLength = compressedLength;

    // If this call succeeds, | compressed | is now owned by the statement, and
    // we are no longer responsible for it.
    rv = stmt->BindAdoptedBlobByName(NS_LITERAL_CSTRING("data"), dataBuffer,
                                     dataBufferLength);
    if (NS_FAILED(rv)) {
      moz_free(compressed);
    }
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

  // Handle blobs
  uint32_t length = mCloneWriteInfo.mFiles.Length();
  if (length) {
    nsRefPtr<FileManager> fileManager = mDatabase->Manager();

    nsCOMPtr<nsIFile> directory = fileManager->GetDirectory();
    IDB_ENSURE_TRUE(directory, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

    nsCOMPtr<nsIFile> journalDirectory = fileManager->EnsureJournalDirectory();
    IDB_ENSURE_TRUE(journalDirectory, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

    nsAutoString fileIds;

    for (uint32_t index = 0; index < length; index++) {
      StructuredCloneFile& cloneFile = mCloneWriteInfo.mFiles[index];

      FileInfo* fileInfo = cloneFile.mFileInfo;
      nsIInputStream* inputStream = cloneFile.mInputStream;

      int64_t id = fileInfo->Id();
      if (inputStream) {
        // Create a journal file first
        nsCOMPtr<nsIFile> nativeFile =
          fileManager->GetFileForId(journalDirectory, id);
        IDB_ENSURE_TRUE(nativeFile, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

        rv = nativeFile->Create(nsIFile::NORMAL_FILE_TYPE, 0644);
        IDB_ENSURE_TRUE(nativeFile, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

        // Now we can copy the blob
        nativeFile = fileManager->GetFileForId(directory, id);
        IDB_ENSURE_TRUE(nativeFile, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

        IDBDatabase* database = mObjectStore->Transaction()->Database();
        nsRefPtr<FileOutputStream> outputStream =
          FileOutputStream::Create(database->Type(), database->Group(),
                                   database->Origin(), nativeFile);
        IDB_ENSURE_TRUE(outputStream, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

        rv = CopyData(inputStream, outputStream);
        NS_ENSURE_SUCCESS(rv, rv);

        cloneFile.mFile->AddFileInfo(fileInfo);
      }

      if (index) {
        fileIds.Append(NS_LITERAL_STRING(" "));
      }
      fileIds.AppendInt(id);
    }

    rv = stmt->BindStringByName(NS_LITERAL_CSTRING("file_ids"), fileIds);
  }
  else {
    rv = stmt->BindNullByName(NS_LITERAL_CSTRING("file_ids"));
  }
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  rv = stmt->Execute();
  if (rv == NS_ERROR_STORAGE_CONSTRAINT) {
    NS_ASSERTION(!keyUnset, "Generated key had a collision!?");
    return NS_ERROR_DOM_INDEXEDDB_CONSTRAINT_ERR;
  }
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  int64_t objectDataId;
  rv = aConnection->GetLastInsertRowID(&objectDataId);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  // Update our indexes if needed.
  if (mOverwrite || !mIndexUpdateInfo.IsEmpty()) {
    rv = IDBObjectStore::UpdateIndexes(mTransaction, osid, mKey, mOverwrite,
                                       objectDataId, mIndexUpdateInfo);
    if (rv == NS_ERROR_STORAGE_CONSTRAINT) {
      return NS_ERROR_DOM_INDEXEDDB_CONSTRAINT_ERR;
    }
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

  if (autoIncrementNum) {
    mObjectStore->Info()->nextAutoIncrementId = autoIncrementNum + 1;
  }

  return NS_OK;
}

nsresult
AddHelper::GetSuccessResult(JSContext* aCx,
                            JS::MutableHandle<JS::Value> aVal)
{
  NS_ASSERTION(!mKey.IsUnset(), "Badness!");

  mCloneWriteInfo.mCloneBuffer.clear();

  return mKey.ToJSVal(aCx, aVal);
}

void
AddHelper::ReleaseMainThreadObjects()
{
  IDBObjectStore::ClearCloneWriteInfo(mCloneWriteInfo);
  ObjectStoreHelper::ReleaseMainThreadObjects();
}

nsresult
AddHelper::PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "AddHelper::PackArgumentsForParentProcess");

  MOZ_CRASH("Remove me!");
  /*
  AddPutParams commonParams;
  commonParams.cloneInfo() = mCloneWriteInfo;
  commonParams.key() = mKey;
  commonParams.indexUpdateInfos().AppendElements(mIndexUpdateInfo);

  const nsTArray<StructuredCloneFile>& files = mCloneWriteInfo.mFiles;

  if (!files.IsEmpty()) {
    uint32_t fileCount = files.Length();

    nsTArray<PBlobChild*>& blobsChild = commonParams.blobsChild();
    blobsChild.SetCapacity(fileCount);

    ContentChild* contentChild = ContentChild::GetSingleton();
    NS_ASSERTION(contentChild, "This should never be null!");

    for (uint32_t index = 0; index < fileCount; index++) {
      const StructuredCloneFile& file = files[index];

      NS_ASSERTION(file.mFile, "This should never be null!");
      NS_ASSERTION(!file.mFileInfo, "This is not yet supported!");

      BlobChild* actor =
        contentChild->GetOrCreateActorForBlob(file.mFile);
      if (!actor) {
        IDB_REPORT_INTERNAL_ERR();
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }
      blobsChild.AppendElement(actor);
    }
  }

  if (mOverwrite) {
    PutParams putParams;
    putParams.commonParams() = commonParams;
    aParams = putParams;
  }
  else {
    AddParams addParams;
    addParams.commonParams() = commonParams;
    aParams = addParams;
  }
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
AddHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "AddHelper::SendResponseToChildProcess");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response =  aResultCode;
  }
  else if (mOverwrite) {
    PutResponse putResponse;
    putResponse.key() = mKey;
    response = putResponse;
  }
  else {
    AddResponse addResponse;
    addResponse.key() = mKey;
    response = addResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
AddHelper::UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TAddResponse ||
               aResponseValue.type() == ResponseValue::TPutResponse,
               "Bad response type!");

  mKey = mOverwrite ?
         aResponseValue.get_PutResponse().key() :
         aResponseValue.get_AddResponse().key();
         */
  return NS_OK;
}

nsresult
GetHelper::DoDatabaseWork(mozIStorageConnection* /* aConnection */)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(mKeyRange, "Must have a key range here!");

  PROFILER_LABEL("IndexedDB", "GetHelper::DoDatabaseWork [IDBObjectStore.cpp]");

  nsCString keyRangeClause;
  mKeyRange->GetBindingClause(NS_LITERAL_CSTRING("key_value"), keyRangeClause);

  NS_ASSERTION(!keyRangeClause.IsEmpty(), "Huh?!");

  nsCString query = NS_LITERAL_CSTRING("SELECT data, file_ids FROM object_data "
                                       "WHERE object_store_id = :osid") +
                    keyRangeClause + NS_LITERAL_CSTRING(" LIMIT 1");

  nsCOMPtr<mozIStorageStatement> stmt = mTransaction->GetCachedStatement(query);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv =
    stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"), mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  rv = mKeyRange->BindToStatement(stmt);
  NS_ENSURE_SUCCESS(rv, rv);

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (hasResult) {
    rv = IDBObjectStore::GetStructuredCloneReadInfoFromStatement(stmt, 0, 1,
      mDatabase, mCloneReadInfo);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

nsresult
GetHelper::GetSuccessResult(JSContext* aCx,
                            JS::MutableHandle<JS::Value> aVal)
{
  MOZ_CRASH("Remove me!");
}

void
GetHelper::ReleaseMainThreadObjects()
{
  MOZ_CRASH("Remove me!");
}

nsresult
GetHelper::PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams)
{
  MOZ_CRASH("Remove me!");
  /*
  GetParams params;

  mKeyRange->ToSerialized(params.keyRange());

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
GetHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  nsTArray<PBlobParent*> blobsParent;

  if (NS_SUCCEEDED(aResultCode)) {
    IDBDatabase* database = mObjectStore->Transaction()->Database();
    NS_ASSERTION(database, "This should never be null!");

    ContentParent* contentParent = database->GetContentParent();
    NS_ASSERTION(contentParent, "This should never be null!");

    FileManager* fileManager = database->Manager();
    NS_ASSERTION(fileManager, "This should never be null!");

    const nsTArray<StructuredCloneFile>& files = mCloneReadInfo.mFiles;

    aResultCode =
      IDBObjectStore::ConvertBlobsToActors(contentParent, fileManager, files,
                                           blobsParent);
    if (NS_FAILED(aResultCode)) {
      NS_WARNING("ConvertBlobsToActors failed!");
    }
  }

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    GetResponse getResponse;
    getResponse.cloneInfo() = mCloneReadInfo;
    getResponse.blobsParent().SwapElements(blobsParent);
    response = getResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
GetHelper::UnpackResponseFromParentProcess(const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TGetResponse,
               "Bad response type!");

  const GetResponse& getResponse = aResponseValue.get_GetResponse();
  const SerializedStructuredCloneReadInfo& cloneInfo = getResponse.cloneInfo();

  NS_ASSERTION((!cloneInfo.dataLength && !cloneInfo.data) ||
               (cloneInfo.dataLength && cloneInfo.data),
               "Inconsistent clone info!");

  if (!mCloneReadInfo.SetFromSerialized(cloneInfo)) {
    IDB_WARNING("Failed to copy clone buffer!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  IDBObjectStore::ConvertActorsToBlobs(getResponse.blobsChild(),
                                       mCloneReadInfo.mFiles);
                                       */
  return NS_OK;
}

nsresult
DeleteHelper::DoDatabaseWork(mozIStorageConnection* /*aConnection */)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(mKeyRange, "Must have a key range here!");

  PROFILER_LABEL("IndexedDB", "DeleteHelper::DoDatabaseWork");

  nsCString keyRangeClause;
  mKeyRange->GetBindingClause(NS_LITERAL_CSTRING("key_value"), keyRangeClause);

  NS_ASSERTION(!keyRangeClause.IsEmpty(), "Huh?!");

  nsCString query = NS_LITERAL_CSTRING("DELETE FROM object_data "
                                       "WHERE object_store_id = :osid") +
                    keyRangeClause;

  nsCOMPtr<mozIStorageStatement> stmt = mTransaction->GetCachedStatement(query);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"),
                                      mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  rv = mKeyRange->BindToStatement(stmt);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = stmt->Execute();
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

nsresult
DeleteHelper::GetSuccessResult(JSContext* aCx,
                               JS::MutableHandle<JS::Value> aVal)
{
  aVal.setUndefined();
  return NS_OK;
}

nsresult
DeleteHelper::PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(mKeyRange, "This should never be null!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "DeleteHelper::PackArgumentsForParentProcess");

  MOZ_CRASH("Remove me!");
  /*
  DeleteParams params;

  mKeyRange->ToSerialized(params.keyRange());

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
DeleteHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "DeleteHelper::SendResponseToChildProcess");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    response = DeleteResponse();
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
DeleteHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TDeleteResponse,
               "Bad response type!");
*/
  return NS_OK;
}

nsresult
ClearHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(aConnection, "Passed a null connection!");

  PROFILER_LABEL("IndexedDB", "ClearHelper::DoDatabaseWork");

  nsCOMPtr<mozIStorageStatement> stmt =
    mTransaction->GetCachedStatement(
      NS_LITERAL_CSTRING("DELETE FROM object_data "
                         "WHERE object_store_id = :osid"));
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"),
                                      mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  rv = stmt->Execute();
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

nsresult
ClearHelper::PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "ClearHelper::PackArgumentsForParentProcess");

  MOZ_CRASH("Remove me!");
  /*
  aParams = ClearParams();
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
ClearHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "ClearHelper::SendResponseToChildProcess");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    response = ClearResponse();
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
ClearHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TClearResponse,
               "Bad response type!");
*/
  return NS_OK;
}

nsresult
OpenCursorHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_LABEL("IndexedDB",
                 "OpenCursorHelper::DoDatabaseWork [IDBObjectStore.cpp]");

  NS_NAMED_LITERAL_CSTRING(keyValue, "key_value");

  nsCString keyRangeClause;
  if (mKeyRange) {
    mKeyRange->GetBindingClause(keyValue, keyRangeClause);
  }

  nsAutoCString directionClause;
  switch (mDirection) {
    case IDBCursor::NEXT:
    case IDBCursor::NEXT_UNIQUE:
      directionClause.AssignLiteral(" ORDER BY key_value ASC");
      break;

    case IDBCursor::PREV:
    case IDBCursor::PREV_UNIQUE:
      directionClause.AssignLiteral(" ORDER BY key_value DESC");
      break;

    default:
      NS_NOTREACHED("Unknown direction type!");
  }

  nsCString firstQuery = NS_LITERAL_CSTRING("SELECT key_value, data, file_ids "
                                            "FROM object_data "
                                            "WHERE object_store_id = :id") +
                         keyRangeClause + directionClause +
                         NS_LITERAL_CSTRING(" LIMIT 1");

  nsCOMPtr<mozIStorageStatement> stmt =
    mTransaction->GetCachedStatement(firstQuery);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("id"),
                                      mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    rv = mKeyRange->BindToStatement(stmt);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (!hasResult) {
    mKey.Unset();
    return NS_OK;
  }

  rv = mKey.SetFromStatement(stmt, 0);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = IDBObjectStore::GetStructuredCloneReadInfoFromStatement(stmt, 1, 2,
    mDatabase, mCloneReadInfo);
  NS_ENSURE_SUCCESS(rv, rv);

  // Now we need to make the query to get the next match.
  keyRangeClause.Truncate();
  nsAutoCString continueToKeyRangeClause;

  NS_NAMED_LITERAL_CSTRING(currentKey, "current_key");
  NS_NAMED_LITERAL_CSTRING(rangeKey, "range_key");

  switch (mDirection) {
    case IDBCursor::NEXT:
    case IDBCursor::NEXT_UNIQUE:
      AppendConditionClause(keyValue, currentKey, false, false,
                            keyRangeClause);
      AppendConditionClause(keyValue, currentKey, false, true,
                            continueToKeyRangeClause);
      if (mKeyRange && !mKeyRange->Upper().IsUnset()) {
        AppendConditionClause(keyValue, rangeKey, true,
                              !mKeyRange->UpperOpen(), keyRangeClause);
        AppendConditionClause(keyValue, rangeKey, true,
                              !mKeyRange->UpperOpen(),
                              continueToKeyRangeClause);
        mRangeKey = mKeyRange->Upper();
      }
      break;

    case IDBCursor::PREV:
    case IDBCursor::PREV_UNIQUE:
      AppendConditionClause(keyValue, currentKey, true, false, keyRangeClause);
      AppendConditionClause(keyValue, currentKey, true, true,
                           continueToKeyRangeClause);
      if (mKeyRange && !mKeyRange->Lower().IsUnset()) {
        AppendConditionClause(keyValue, rangeKey, false,
                              !mKeyRange->LowerOpen(), keyRangeClause);
        AppendConditionClause(keyValue, rangeKey, false,
                              !mKeyRange->LowerOpen(),
                              continueToKeyRangeClause);
        mRangeKey = mKeyRange->Lower();
      }
      break;

    default:
      NS_NOTREACHED("Unknown direction type!");
  }

  NS_NAMED_LITERAL_CSTRING(queryStart, "SELECT key_value, data, file_ids "
                                       "FROM object_data "
                                       "WHERE object_store_id = :id");

  mContinueQuery = queryStart + keyRangeClause + directionClause +
                   NS_LITERAL_CSTRING(" LIMIT ");

  mContinueToQuery = queryStart + continueToKeyRangeClause + directionClause +
                     NS_LITERAL_CSTRING(" LIMIT ");

  return NS_OK;
}

nsresult
OpenCursorHelper::EnsureCursor()
{
  if (mCursor || mKey.IsUnset()) {
    return NS_OK;
  }

  MOZ_CRASH("Fix");
  //mSerializedCloneReadInfo = mCloneReadInfo;

  NS_ASSERTION(!mSerializedCloneReadInfo.data().IsEmpty(),
               "Shouldn't be possible!");

  nsRefPtr<IDBCursor> cursor =
    IDBCursor::Create(mRequest, mTransaction, mObjectStore, mDirection,
                      mRangeKey, mContinueQuery, mContinueToQuery, mKey,
                      Move(mCloneReadInfo));
  IDB_ENSURE_TRUE(cursor, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  NS_ASSERTION(!mCloneReadInfo.mCloneBuffer.data(), "Should have swapped!");

  mCursor.swap(cursor);
  return NS_OK;
}

nsresult
OpenCursorHelper::GetSuccessResult(JSContext* aCx,
                                   JS::MutableHandle<JS::Value> aVal)
{
  nsresult rv = EnsureCursor();
  NS_ENSURE_SUCCESS(rv, rv);

  if (mCursor) {
    rv = WrapNative(aCx, mCursor, aVal);
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }
  else {
    aVal.setUndefined();
  }

  return NS_OK;
}

void
OpenCursorHelper::ReleaseMainThreadObjects()
{
  mKeyRange = nullptr;
  IDBObjectStore::ClearCloneReadInfo(mCloneReadInfo);

  mCursor = nullptr;

  // These don't need to be released on the main thread but they're only valid
  // as long as mCursor is set.
  mSerializedCloneReadInfo.data().Clear();

  ObjectStoreHelper::ReleaseMainThreadObjects();
}

nsresult
OpenCursorHelper::PackArgumentsForParentProcess(
                                              ObjectStoreRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenCursorHelper::PackArgumentsForParentProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  OpenCursorParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  }
  else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  params.direction() = mDirection;

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
OpenCursorHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");
  NS_ASSERTION(!mCursor, "Shouldn't have this yet!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenCursorHelper::SendResponseToChildProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  nsTArray<PBlobParent*> blobsParent;

  if (NS_SUCCEEDED(aResultCode)) {
    IDBDatabase* database = mObjectStore->Transaction()->Database();
    NS_ASSERTION(database, "This should never be null!");

    ContentParent* contentParent = database->GetContentParent();
    NS_ASSERTION(contentParent, "This should never be null!");

    FileManager* fileManager = database->Manager();
    NS_ASSERTION(fileManager, "This should never be null!");

    const nsTArray<StructuredCloneFile>& files = mCloneReadInfo.mFiles;

    aResultCode =
      IDBObjectStore::ConvertBlobsToActors(contentParent, fileManager, files,
                                           blobsParent);
    if (NS_FAILED(aResultCode)) {
      NS_WARNING("ConvertBlobsToActors failed!");
    }
  }

  if (NS_SUCCEEDED(aResultCode)) {
    nsresult rv = EnsureCursor();
    if (NS_FAILED(rv)) {
      NS_WARNING("EnsureCursor failed!");
      aResultCode = rv;
    }
  }

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    OpenCursorResponse openCursorResponse;

    if (!mCursor) {
      openCursorResponse = mozilla::void_t();
    }
    else {
      IndexedDBObjectStoreParent* objectStoreActor =
        mObjectStore->GetActorParent();
      NS_ASSERTION(objectStoreActor, "Must have an actor here!");

      IndexedDBRequestParentBase* requestActor = mRequest->GetActorParent();
      NS_ASSERTION(requestActor, "Must have an actor here!");

      NS_ASSERTION(mSerializedCloneReadInfo.data &&
                   mSerializedCloneReadInfo.dataLength,
                   "Shouldn't be possible!");

      ObjectStoreCursorConstructorParams params;
      params.requestParent() = requestActor;
      params.direction() = mDirection;
      params.key() = mKey;
      params.optionalCloneInfo() = mSerializedCloneReadInfo;
      params.blobsParent().SwapElements(blobsParent);

      if (!objectStoreActor->OpenCursor(mCursor, params, openCursorResponse)) {
        return Error;
      }
    }

    response = openCursorResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
OpenCursorHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TOpenCursorResponse,
               "Bad response type!");
  NS_ASSERTION(aResponseValue.get_OpenCursorResponse().type() ==
               OpenCursorResponse::Tvoid_t ||
               aResponseValue.get_OpenCursorResponse().type() ==
               OpenCursorResponse::TPIndexedDBCursorChild,
               "Bad response union type!");
  NS_ASSERTION(!mCursor, "Shouldn't have this yet!");

  const OpenCursorResponse& response =
    aResponseValue.get_OpenCursorResponse();

  switch (response.type()) {
    case OpenCursorResponse::Tvoid_t:
      break;

    case OpenCursorResponse::TPIndexedDBCursorChild: {
      IndexedDBCursorChild* actor =
        static_cast<IndexedDBCursorChild*>(
          response.get_PIndexedDBCursorChild());

      mCursor = actor->ForgetStrongCursor();
      NS_ASSERTION(mCursor, "This should never be null!");

    } break;

    default:
      MOZ_CRASH();
  }
  */
  return NS_OK;
}

nsresult
OpenKeyCursorHelper::DoDatabaseWork(mozIStorageConnection* /* aConnection */)
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());

  PROFILER_LABEL("IndexedDB",
                 "OpenKeyCursorHelper::DoDatabaseWork [IDBObjectStore.cpp]");

  NS_NAMED_LITERAL_CSTRING(keyValue, "key_value");
  NS_NAMED_LITERAL_CSTRING(id, "id");
  NS_NAMED_LITERAL_CSTRING(openLimit, " LIMIT ");

  nsAutoCString queryStart = NS_LITERAL_CSTRING("SELECT ") + keyValue +
                             NS_LITERAL_CSTRING(" FROM object_data WHERE "
                                                "object_store_id = :") +
                             id;

  nsAutoCString keyRangeClause;
  if (mKeyRange) {
    mKeyRange->GetBindingClause(keyValue, keyRangeClause);
  }

  nsAutoCString directionClause = NS_LITERAL_CSTRING(" ORDER BY ") + keyValue;
  switch (mDirection) {
    case IDBCursor::NEXT:
    case IDBCursor::NEXT_UNIQUE:
      directionClause.AppendLiteral(" ASC");
      break;

    case IDBCursor::PREV:
    case IDBCursor::PREV_UNIQUE:
      directionClause.AppendLiteral(" DESC");
      break;

    default:
      MOZ_ASSUME_UNREACHABLE("Unknown direction type!");
  }

  nsCString firstQuery = queryStart + keyRangeClause + directionClause +
                         openLimit + NS_LITERAL_CSTRING("1");

  nsCOMPtr<mozIStorageStatement> stmt =
    mTransaction->GetCachedStatement(firstQuery);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(id, mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    rv = mKeyRange->BindToStatement(stmt);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (!hasResult) {
    mKey.Unset();
    return NS_OK;
  }

  rv = mKey.SetFromStatement(stmt, 0);
  NS_ENSURE_SUCCESS(rv, rv);

  // Now we need to make the query to get the next match.
  keyRangeClause.Truncate();
  nsAutoCString continueToKeyRangeClause;

  NS_NAMED_LITERAL_CSTRING(currentKey, "current_key");
  NS_NAMED_LITERAL_CSTRING(rangeKey, "range_key");

  switch (mDirection) {
    case IDBCursor::NEXT:
    case IDBCursor::NEXT_UNIQUE:
      AppendConditionClause(keyValue, currentKey, false, false,
                            keyRangeClause);
      AppendConditionClause(keyValue, currentKey, false, true,
                            continueToKeyRangeClause);
      if (mKeyRange && !mKeyRange->Upper().IsUnset()) {
        AppendConditionClause(keyValue, rangeKey, true,
                              !mKeyRange->UpperOpen(), keyRangeClause);
        AppendConditionClause(keyValue, rangeKey, true,
                              !mKeyRange->UpperOpen(),
                              continueToKeyRangeClause);
        mRangeKey = mKeyRange->Upper();
      }
      break;

    case IDBCursor::PREV:
    case IDBCursor::PREV_UNIQUE:
      AppendConditionClause(keyValue, currentKey, true, false, keyRangeClause);
      AppendConditionClause(keyValue, currentKey, true, true,
                            continueToKeyRangeClause);
      if (mKeyRange && !mKeyRange->Lower().IsUnset()) {
        AppendConditionClause(keyValue, rangeKey, false,
                              !mKeyRange->LowerOpen(), keyRangeClause);
        AppendConditionClause(keyValue, rangeKey, false,
                              !mKeyRange->LowerOpen(),
                              continueToKeyRangeClause);
        mRangeKey = mKeyRange->Lower();
      }
      break;

    default:
      MOZ_ASSUME_UNREACHABLE("Unknown direction type!");
  }

  mContinueQuery = queryStart + keyRangeClause + directionClause + openLimit;
  mContinueToQuery = queryStart + continueToKeyRangeClause + directionClause +
                     openLimit;

  return NS_OK;
}

nsresult
OpenKeyCursorHelper::EnsureCursor()
{
  MOZ_ASSERT(NS_IsMainThread());

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::EnsureCursor "
                             "[IDBObjectStore.cpp]");

  if (mCursor || mKey.IsUnset()) {
    return NS_OK;
  }

  mCursor = IDBCursor::Create(mRequest, mTransaction, mObjectStore, mDirection,
                              mRangeKey, mContinueQuery, mContinueToQuery,
                              mKey);
  IDB_ENSURE_TRUE(mCursor, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

nsresult
OpenKeyCursorHelper::GetSuccessResult(JSContext* aCx,
                                      JS::MutableHandle<JS::Value> aVal)
{
  MOZ_ASSERT(NS_IsMainThread());

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::GetSuccessResult "
                             "[IDBObjectStore.cpp]");

  nsresult rv = EnsureCursor();
  NS_ENSURE_SUCCESS(rv, rv);

  if (mCursor) {
    rv = WrapNative(aCx, mCursor, aVal);
    IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }
  else {
    aVal.setUndefined();
  }

  return NS_OK;
}

void
OpenKeyCursorHelper::ReleaseMainThreadObjects()
{
  MOZ_ASSERT(NS_IsMainThread());

  mKeyRange = nullptr;
  mCursor = nullptr;

  ObjectStoreHelper::ReleaseMainThreadObjects();
}

nsresult
OpenKeyCursorHelper::PackArgumentsForParentProcess(
                                              ObjectStoreRequestParams& aParams)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!IndexedDatabaseManager::IsMainProcess());

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::"
                             "PackArgumentsForParentProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  OpenKeyCursorParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  }
  else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  params.direction() = mDirection;

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
OpenKeyCursorHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(!mCursor);

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::SendResponseToChildProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  MOZ_ASSERT(actor);

  if (NS_SUCCEEDED(aResultCode)) {
    nsresult rv = EnsureCursor();
    if (NS_FAILED(rv)) {
      NS_WARNING("EnsureCursor failed!");
      aResultCode = rv;
    }
  }

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  } else {
    OpenCursorResponse openCursorResponse;

    if (!mCursor) {
      openCursorResponse = mozilla::void_t();
    }
    else {
      IndexedDBObjectStoreParent* objectStoreActor =
        mObjectStore->GetActorParent();
      MOZ_ASSERT(objectStoreActor);

      IndexedDBRequestParentBase* requestActor = mRequest->GetActorParent();
      MOZ_ASSERT(requestActor);

      ObjectStoreCursorConstructorParams params;
      params.requestParent() = requestActor;
      params.direction() = mDirection;
      params.key() = mKey;
      params.optionalCloneInfo() = mozilla::void_t();

      if (!objectStoreActor->OpenCursor(mCursor, params, openCursorResponse)) {
        return Error;
      }
    }

    response = openCursorResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
OpenKeyCursorHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!IndexedDatabaseManager::IsMainProcess());
  MOZ_ASSERT(aResponseValue.type() == ResponseValue::TOpenCursorResponse);
  MOZ_ASSERT(aResponseValue.get_OpenCursorResponse().type() ==
               OpenCursorResponse::Tvoid_t ||
             aResponseValue.get_OpenCursorResponse().type() ==
               OpenCursorResponse::TPIndexedDBCursorChild);
  MOZ_ASSERT(!mCursor);

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "OpenKeyCursorHelper::"
                             "UnpackResponseFromParentProcess "
                             "[IDBObjectStore.cpp]");

  const OpenCursorResponse& response =
    aResponseValue.get_OpenCursorResponse();

  switch (response.type()) {
    case OpenCursorResponse::Tvoid_t:
      break;

    case OpenCursorResponse::TPIndexedDBCursorChild: {
      IndexedDBCursorChild* actor =
        static_cast<IndexedDBCursorChild*>(
          response.get_PIndexedDBCursorChild());

      mCursor = actor->ForgetStrongCursor();
      NS_ASSERTION(mCursor, "This should never be null!");

    } break;

    default:
      MOZ_CRASH("Unknown response union type!");
  }
  */
  return NS_OK;
}

nsresult
GetAllHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_LABEL("IndexedDB",
                 "GetAllHelper::DoDatabaseWork [IDBObjectStore.cpp]");

  NS_NAMED_LITERAL_CSTRING(lowerKeyName, "lower_key");
  NS_NAMED_LITERAL_CSTRING(upperKeyName, "upper_key");

  nsAutoCString keyRangeClause;
  if (mKeyRange) {
    if (!mKeyRange->Lower().IsUnset()) {
      keyRangeClause = NS_LITERAL_CSTRING(" AND key_value");
      if (mKeyRange->LowerOpen()) {
        keyRangeClause.AppendLiteral(" > :");
      }
      else {
        keyRangeClause.AppendLiteral(" >= :");
      }
      keyRangeClause.Append(lowerKeyName);
    }

    if (!mKeyRange->Upper().IsUnset()) {
      keyRangeClause += NS_LITERAL_CSTRING(" AND key_value");
      if (mKeyRange->UpperOpen()) {
        keyRangeClause.AppendLiteral(" < :");
      }
      else {
        keyRangeClause.AppendLiteral(" <= :");
      }
      keyRangeClause.Append(upperKeyName);
    }
  }

  nsAutoCString limitClause;
  if (mLimit != UINT32_MAX) {
    limitClause.AssignLiteral(" LIMIT ");
    limitClause.AppendInt(mLimit);
  }

  nsCString query = NS_LITERAL_CSTRING("SELECT data, file_ids FROM object_data "
                                       "WHERE object_store_id = :osid") +
                    keyRangeClause +
                    NS_LITERAL_CSTRING(" ORDER BY key_value ASC") +
                    limitClause;

  mCloneReadInfos.SetCapacity(50);

  nsCOMPtr<mozIStorageStatement> stmt = mTransaction->GetCachedStatement(query);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"),
                                      mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    if (!mKeyRange->Lower().IsUnset()) {
      rv = mKeyRange->Lower().BindToStatement(stmt, lowerKeyName);
      NS_ENSURE_SUCCESS(rv, rv);
    }
    if (!mKeyRange->Upper().IsUnset()) {
      rv = mKeyRange->Upper().BindToStatement(stmt, upperKeyName);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  bool hasResult;
  while (NS_SUCCEEDED((rv = stmt->ExecuteStep(&hasResult))) && hasResult) {
    if (mCloneReadInfos.Capacity() == mCloneReadInfos.Length()) {
      mCloneReadInfos.SetCapacity(mCloneReadInfos.Capacity() * 2);
    }

    StructuredCloneReadInfo* readInfo = mCloneReadInfos.AppendElement();
    NS_ASSERTION(readInfo, "Shouldn't fail since SetCapacity succeeded!");

    rv = IDBObjectStore::GetStructuredCloneReadInfoFromStatement(stmt, 0, 1,
      mDatabase, *readInfo);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

nsresult
GetAllHelper::GetSuccessResult(JSContext* aCx,
                               JS::MutableHandle<JS::Value> aVal)
{
  NS_ASSERTION(mCloneReadInfos.Length() <= mLimit, "Too many results!");

  nsresult rv = ConvertToArrayAndCleanup(aCx, mCloneReadInfos, aVal);

  NS_ASSERTION(mCloneReadInfos.IsEmpty(),
               "Should have cleared in ConvertToArrayAndCleanup");
  NS_ENSURE_SUCCESS(rv, rv);

  return NS_OK;
}

void
GetAllHelper::ReleaseMainThreadObjects()
{
  mKeyRange = nullptr;
  for (uint32_t index = 0; index < mCloneReadInfos.Length(); index++) {
    IDBObjectStore::ClearCloneReadInfo(mCloneReadInfos[index]);
  }
  ObjectStoreHelper::ReleaseMainThreadObjects();
}

nsresult
GetAllHelper::PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "GetAllHelper::PackArgumentsForParentProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  GetAllParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  }
  else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  params.limit() = mLimit;

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
GetAllHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "GetAllHelper::SendResponseToChildProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  GetAllResponse getAllResponse;
  if (NS_SUCCEEDED(aResultCode) && !mCloneReadInfos.IsEmpty()) {
    IDBDatabase* database = mObjectStore->Transaction()->Database();
    NS_ASSERTION(database, "This should never be null!");

    ContentParent* contentParent = database->GetContentParent();
    NS_ASSERTION(contentParent, "This should never be null!");

    FileManager* fileManager = database->Manager();
    NS_ASSERTION(fileManager, "This should never be null!");

    uint32_t length = mCloneReadInfos.Length();

    nsTArray<SerializedStructuredCloneReadInfo>& infos =
      getAllResponse.cloneInfos();
    infos.SetCapacity(length);

    nsTArray<BlobArray>& blobArrays = getAllResponse.blobs();
    blobArrays.SetCapacity(length);

    for (uint32_t index = 0;
         NS_SUCCEEDED(aResultCode) && index < length;
         index++) {
      // Append the structured clone data.
      const StructuredCloneReadInfo& clone = mCloneReadInfos[index];
      SerializedStructuredCloneReadInfo* info = infos.AppendElement();
      *info = clone;

      // Now take care of the files.
      const nsTArray<StructuredCloneFile>& files = clone.mFiles;
      BlobArray* blobArray = blobArrays.AppendElement();
      nsTArray<PBlobParent*>& blobs = blobArray->blobsParent();

      aResultCode =
        IDBObjectStore::ConvertBlobsToActors(contentParent, fileManager, files,
                                             blobs);
      if (NS_FAILED(aResultCode)) {
        NS_WARNING("ConvertBlobsToActors failed!");
        break;
      }
    }
  }

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    response = getAllResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
GetAllHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TGetAllResponse,
               "Bad response type!");

  const GetAllResponse& getAllResponse = aResponseValue.get_GetAllResponse();
  const nsTArray<SerializedStructuredCloneReadInfo>& cloneInfos =
    getAllResponse.cloneInfos();
  const nsTArray<BlobArray>& blobArrays = getAllResponse.blobs();

  mCloneReadInfos.SetCapacity(cloneInfos.Length());

  for (uint32_t index = 0; index < cloneInfos.Length(); index++) {
    const SerializedStructuredCloneReadInfo srcInfo = cloneInfos[index];
    const nsTArray<PBlobChild*>& blobs = blobArrays[index].blobsChild();

    StructuredCloneReadInfo* destInfo = mCloneReadInfos.AppendElement();
    if (!destInfo->SetFromSerialized(srcInfo)) {
      IDB_WARNING("Failed to copy clone buffer!");
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    IDBObjectStore::ConvertActorsToBlobs(blobs, destInfo->mFiles);
  }
  */
  return NS_OK;
}

nsresult
GetAllKeysHelper::DoDatabaseWork(mozIStorageConnection* /* aConnection */)
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());

  PROFILER_LABEL("IndexedDB",
                 "GetAllKeysHelper::DoDatabaseWork [IDObjectStore.cpp]");

  NS_NAMED_LITERAL_CSTRING(keyValue, "key_value");

  nsAutoCString keyRangeClause;
  if (mKeyRange) {
    mKeyRange->GetBindingClause(keyValue, keyRangeClause);
  }

  nsAutoCString limitClause;
  if (mLimit != UINT32_MAX) {
    limitClause = NS_LITERAL_CSTRING(" LIMIT ");
    limitClause.AppendInt(mLimit);
  }

  NS_NAMED_LITERAL_CSTRING(osid, "osid");

  nsCString query = NS_LITERAL_CSTRING("SELECT ") + keyValue +
                    NS_LITERAL_CSTRING(" FROM object_data WHERE "
                                       "object_store_id = :") +
                    osid + keyRangeClause +
                    NS_LITERAL_CSTRING(" ORDER BY key_value ASC") +
                    limitClause;

  nsCOMPtr<mozIStorageStatement> stmt = mTransaction->GetCachedStatement(query);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(osid, mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    rv = mKeyRange->BindToStatement(stmt);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  mKeys.SetCapacity(std::min<uint32_t>(50, mLimit));

  bool hasResult;
  while(NS_SUCCEEDED((rv = stmt->ExecuteStep(&hasResult))) && hasResult) {
    if (mKeys.Capacity() == mKeys.Length()) {
      mKeys.SetCapacity(mKeys.Capacity() * 2);
    }

    Key* key = mKeys.AppendElement();
    NS_ASSERTION(key, "This shouldn't fail!");

    rv = key->SetFromStatement(stmt, 0);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  return NS_OK;
}

nsresult
GetAllKeysHelper::GetSuccessResult(JSContext* aCx,
                                   JS::MutableHandle<JS::Value> aVal)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mKeys.Length() <= mLimit);

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "GetAllKeysHelper::GetSuccessResult "
                             "[IDBObjectStore.cpp]");

  nsTArray<Key> keys;
  mKeys.SwapElements(keys);

  JS::Rooted<JSObject*> array(aCx, JS_NewArrayObject(aCx, 0));
  if (!array) {
    IDB_WARNING("Failed to make array!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  if (!keys.IsEmpty()) {
    if (!JS_SetArrayLength(aCx, array, keys.Length())) {
      IDB_WARNING("Failed to set array length!");
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    for (uint32_t index = 0, count = keys.Length(); index < count; index++) {
      const Key& key = keys[index];
      MOZ_ASSERT(!key.IsUnset());

      JS::Rooted<JS::Value> value(aCx);
      nsresult rv = key.ToJSVal(aCx, &value);
      if (NS_FAILED(rv)) {
        NS_WARNING("Failed to get jsval for key!");
        return rv;
      }

      if (!JS_SetElement(aCx, array, index, value)) {
        IDB_WARNING("Failed to set array element!");
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }
    }
  }

  aVal.setObject(*array);
  return NS_OK;
}

void
GetAllKeysHelper::ReleaseMainThreadObjects()
{
  MOZ_ASSERT(NS_IsMainThread());

  mKeyRange = nullptr;

  ObjectStoreHelper::ReleaseMainThreadObjects();
}

nsresult
GetAllKeysHelper::PackArgumentsForParentProcess(
                                              ObjectStoreRequestParams& aParams)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!IndexedDatabaseManager::IsMainProcess());

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "GetAllKeysHelper::PackArgumentsForParentProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  GetAllKeysParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  } else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  params.limit() = mLimit;

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
GetAllKeysHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(IndexedDatabaseManager::IsMainProcess());

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "GetAllKeysHelper::SendResponseToChildProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  MOZ_ASSERT(actor);

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    GetAllKeysResponse getAllKeysResponse;
    getAllKeysResponse.keys().AppendElements(mKeys);
    response = getAllKeysResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
GetAllKeysHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!IndexedDatabaseManager::IsMainProcess());
  MOZ_CRASH("Remove me!");
  /*
  MOZ_ASSERT(aResponseValue.type() == ResponseValue::TGetAllKeysResponse);

  mKeys.AppendElements(aResponseValue.get_GetAllKeysResponse().keys());
  */
  return NS_OK;
}

nsresult
CountHelper::DoDatabaseWork(mozIStorageConnection* aConnection)
{
  NS_ASSERTION(!NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_LABEL("IndexedDB",
                 "CountHelper::DoDatabaseWork [IDBObjectStore.cpp]");

  NS_NAMED_LITERAL_CSTRING(lowerKeyName, "lower_key");
  NS_NAMED_LITERAL_CSTRING(upperKeyName, "upper_key");

  nsAutoCString keyRangeClause;
  if (mKeyRange) {
    if (!mKeyRange->Lower().IsUnset()) {
      keyRangeClause = NS_LITERAL_CSTRING(" AND key_value");
      if (mKeyRange->LowerOpen()) {
        keyRangeClause.AppendLiteral(" > :");
      }
      else {
        keyRangeClause.AppendLiteral(" >= :");
      }
      keyRangeClause.Append(lowerKeyName);
    }

    if (!mKeyRange->Upper().IsUnset()) {
      keyRangeClause += NS_LITERAL_CSTRING(" AND key_value");
      if (mKeyRange->UpperOpen()) {
        keyRangeClause.AppendLiteral(" < :");
      }
      else {
        keyRangeClause.AppendLiteral(" <= :");
      }
      keyRangeClause.Append(upperKeyName);
    }
  }

  nsCString query = NS_LITERAL_CSTRING("SELECT count(*) FROM object_data "
                                       "WHERE object_store_id = :osid") +
                    keyRangeClause;

  nsCOMPtr<mozIStorageStatement> stmt = mTransaction->GetCachedStatement(query);
  IDB_ENSURE_TRUE(stmt, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mozStorageStatementScoper scoper(stmt);

  nsresult rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"),
                                      mObjectStore->Id());
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  if (mKeyRange) {
    if (!mKeyRange->Lower().IsUnset()) {
      rv = mKeyRange->Lower().BindToStatement(stmt, lowerKeyName);
      NS_ENSURE_SUCCESS(rv, rv);
    }
    if (!mKeyRange->Upper().IsUnset()) {
      rv = mKeyRange->Upper().BindToStatement(stmt, upperKeyName);
      NS_ENSURE_SUCCESS(rv, rv);
    }
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  IDB_ENSURE_SUCCESS(rv, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  IDB_ENSURE_TRUE(hasResult, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);

  mCount = stmt->AsInt64(0);
  return NS_OK;
}

nsresult
CountHelper::GetSuccessResult(JSContext* aCx,
                              JS::MutableHandle<JS::Value> aVal)
{
  aVal.setNumber(static_cast<double>(mCount));
  return NS_OK;
}

void
CountHelper::ReleaseMainThreadObjects()
{
  mKeyRange = nullptr;
  ObjectStoreHelper::ReleaseMainThreadObjects();
}

nsresult
CountHelper::PackArgumentsForParentProcess(ObjectStoreRequestParams& aParams)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(!IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "CountHelper::PackArgumentsForParentProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  CountParams params;

  if (mKeyRange) {
    SerializedKeyRange keyRange;
    mKeyRange->ToSerialized(keyRange);
    params.optionalKeyRange() = keyRange;
  }
  else {
    params.optionalKeyRange() = mozilla::void_t();
  }

  aParams = params;
  */
  return NS_OK;
}

AsyncConnectionHelper::ChildProcessSendResult
CountHelper::SendResponseToChildProcess(nsresult aResultCode)
{
  NS_ASSERTION(NS_IsMainThread(), "Wrong thread!");
  NS_ASSERTION(IndexedDatabaseManager::IsMainProcess(), "Wrong process!");

  PROFILER_MAIN_THREAD_LABEL("IndexedDB",
                             "CountHelper::SendResponseToChildProcess "
                             "[IDBObjectStore.cpp]");

  MOZ_CRASH("Remove me!");
  /*
  IndexedDBRequestParentBase* actor = mRequest->GetActorParent();
  NS_ASSERTION(actor, "How did we get this far without an actor?");

  ResponseValue response;
  if (NS_FAILED(aResultCode)) {
    response = aResultCode;
  }
  else {
    CountResponse countResponse = mCount;
    response = countResponse;
  }

  if (!actor->SendResponse(response)) {
    return Error;
  }
  */
  return Success_Sent;
}

nsresult
CountHelper::UnpackResponseFromParentProcess(
                                            const ResponseValue& aResponseValue)
{
  MOZ_CRASH("Remove me!");
  /*
  NS_ASSERTION(aResponseValue.type() == ResponseValue::TCountResponse,
               "Bad response type!");

  mCount = aResponseValue.get_CountResponse().count();
  */
  return NS_OK;
}
