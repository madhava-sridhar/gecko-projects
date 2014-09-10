/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheStorageDBConnection.h"

#include "mozilla/dom/CacheQuotaRunnable.h"
#include "mozilla/dom/CacheStorageDBListener.h"
#include "mozilla/dom/CacheStorageDBSchema.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/UniquePtr.h"
#include "mozIStorageConnection.h"
#include "mozIStorageService.h"
#include "mozIStorageStatement.h"
#include "mozStorageCID.h"
#include "mozStorageHelper.h"
#include "nsIFile.h"
#include "nsNetUtil.h"

namespace mozilla {
namespace dom {

using mozilla::UniquePtr;
using mozilla::dom::cache::Namespace;
using mozilla::dom::cache::RequestId;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;
using mozilla::dom::quota::QuotaManager;

class CacheStorageDBConnection::OpenRunnable : public CacheQuotaRunnable
{
public:
  OpenRunnable(Namespace aNamespace, const nsACString& aOrigin,
               const nsACString& aBaseDomain, bool aAllowCreate,
               RequestId aRequestId, const nsAString& aKey,
               CacheStorageDBConnection* aCacheStorageDBConnection)
    : CacheQuotaRunnable(aOrigin, aBaseDomain, NS_LITERAL_CSTRING("CacheStorage"))
    , mNamespace(aNamespace)
    , mAllowCreate(aAllowCreate)
    , mRequestId(aRequestId)
    , mKey(aKey)
    , mCacheStorageDBConnection(aCacheStorageDBConnection)
    , mResult(NS_OK)
  {
    MOZ_ASSERT(mCacheStorageDBConnection);
  }

protected:
  virtual void AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection)=0;

  virtual void RunOnQuotaIOThread(const nsACString& aOrigin,
                                  const nsACString& aBaseDomain,
                                  nsIFile* aQuotaDir) MOZ_OVERRIDE
  {
    mResult = aQuotaDir->Append(NS_LITERAL_STRING("cachestorage"));
    if (NS_FAILED(mResult)) { return; }

    bool exists;
    mResult = aQuotaDir->Exists(&exists);
    if (NS_FAILED(mResult) || (!exists && !mAllowCreate)) { return; }

    if (!exists) {
      mResult = aQuotaDir->Create(nsIFile::DIRECTORY_TYPE, 0755);
      if (NS_FAILED(mResult)) { return; }
    }

    nsCOMPtr<nsIFile> dbFile;
    mResult = aQuotaDir->Clone(getter_AddRefs(dbFile));
    if (NS_FAILED(mResult)) { return; }

    mResult = dbFile->Append(NS_LITERAL_STRING("db.sqlite"));
    if (NS_FAILED(mResult)) { return; }

    mResult = dbFile->Exists(&exists);
    if (NS_FAILED(mResult) || (!exists && !mAllowCreate)) { return; }

    // XXX: Jonas tells me nsIFileURL usage off-main-thread is dangerous,
    //      but this is what IDB does to access mozIStorageConnection so
    //      it seems at least this corner case mostly works.

    nsCOMPtr<nsIFile> dbTmpDir;
    mResult = aQuotaDir->Clone(getter_AddRefs(dbTmpDir));
    if (NS_FAILED(mResult)) { return; }

    mResult = dbTmpDir->Append(NS_LITERAL_STRING("db"));
    if (NS_FAILED(mResult)) { return; }

    nsCOMPtr<nsIURI> uri;
    mResult = NS_NewFileURI(getter_AddRefs(uri), dbFile);
    if (NS_FAILED(mResult)) { return; }

    nsCOMPtr<nsIFileURL> dbFileUrl = do_QueryInterface(uri);
    if (!dbFileUrl) {
      mResult = NS_ERROR_FAILURE;
      return;
    }

    nsAutoCString type;
    PersistenceTypeToText(PERSISTENCE_TYPE_PERSISTENT, type);

    mResult = dbFileUrl->SetQuery(NS_LITERAL_CSTRING("persistenceType=") + type +
                                  NS_LITERAL_CSTRING("&group=") + aBaseDomain +
                                  NS_LITERAL_CSTRING("&origin=") + aOrigin);
    if (NS_FAILED(mResult)) { return; }

    nsCOMPtr<mozIStorageService> ss =
      do_GetService(MOZ_STORAGE_SERVICE_CONTRACTID);
    if (!ss) {
      mResult = NS_ERROR_FAILURE;
      return;
    }

    nsCOMPtr<mozIStorageConnection> conn;
    mResult = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(conn));
    if (mResult == NS_ERROR_FILE_CORRUPTED) {
      mResult = dbFile->Remove(false);
      if (NS_FAILED(mResult)) { return; }

      mResult = dbTmpDir->Exists(&exists);
      if (NS_FAILED(mResult)) { return; }

      if (exists) {
        bool isDir;
        mResult = dbTmpDir->IsDirectory(&isDir);
        if (NS_FAILED(mResult)) { return; }
        if (!isDir) {
          mResult = NS_ERROR_FAILURE;
          return;
        }
        mResult = dbTmpDir->Remove(true);
        if (NS_FAILED(mResult)) { return; }
      }

      mResult = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(conn));
    }
    if (NS_FAILED(mResult)) { return; }
    MOZ_ASSERT(conn);

    mResult = CacheStorageDBSchema::Create(conn);
    if (NS_FAILED(mResult)) { return; }

    AfterOpenOnQuotaIOThread(conn);
  }

protected:
  virtual ~OpenRunnable() { }

  const Namespace mNamespace;
  const bool mAllowCreate;
  const RequestId mRequestId;
  const nsString mKey;
  nsRefPtr<CacheStorageDBConnection> mCacheStorageDBConnection;
  nsCOMPtr<mozIStorageConnection> mConnection;
  nsresult mResult;
};

class CacheStorageDBConnection::GetRunnable MOZ_FINAL :
  public CacheStorageDBConnection::OpenRunnable
{
public:
  GetRunnable(Namespace aNamespace, const nsACString& aOrigin,
              const nsACString& aBaseDomain, RequestId aRequestId,
              const nsAString& aKey,
              CacheStorageDBConnection* aCacheStorageDBConnection)
    : OpenRunnable(aNamespace, aOrigin, aBaseDomain, false, aRequestId, aKey,
                   aCacheStorageDBConnection)
    , mSuccess(false)
  { }

protected:
  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheStorageDBSchema::Get(aConnection, mNamespace, mKey,
                                        &mSuccess, &mCacheId);
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    if (NS_FAILED(rv) || !mSuccess) {
      mCacheStorageDBConnection->OnGetComplete(mRequestId, rv, nullptr);
      return;
    }
    mCacheStorageDBConnection->OnGetComplete(mRequestId, rv, &mCacheId);
  }

private:
  virtual ~GetRunnable() { }
  nsID mCacheId;
  bool mSuccess;
};

class CacheStorageDBConnection::HasRunnable MOZ_FINAL :
  public CacheStorageDBConnection::OpenRunnable
{
public:
  HasRunnable(Namespace aNamespace, const nsACString& aOrigin,
              const nsACString& aBaseDomain, RequestId aRequestId,
              const nsAString& aKey,
              CacheStorageDBConnection* aCacheStorageDBConnection)
    : OpenRunnable(aNamespace, aOrigin, aBaseDomain, false, aRequestId, aKey,
                   aCacheStorageDBConnection)
    , mSuccess(false)
  { }

protected:
  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheStorageDBSchema::Has(aConnection, mNamespace, mKey,
                                        &mSuccess);
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    mCacheStorageDBConnection->OnHasComplete(mRequestId, rv, mSuccess);
  }

private:
  virtual ~HasRunnable() { }
  bool mSuccess;
};

class CacheStorageDBConnection::PutRunnable MOZ_FINAL :
  public CacheStorageDBConnection::OpenRunnable
{
public:
  PutRunnable(Namespace aNamespace, const nsACString& aOrigin,
              const nsACString& aBaseDomain, RequestId aRequestId,
              const nsAString& aKey, const nsID& aCacheId,
              CacheStorageDBConnection* aCacheStorageDBConnection)
    : OpenRunnable(aNamespace, aOrigin, aBaseDomain, true, aRequestId, aKey,
                   aCacheStorageDBConnection)
    , mCacheId(aCacheId)
    , mSuccess(false)
  { }

protected:
  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheStorageDBSchema::Put(aConnection, mNamespace, mKey, mCacheId,
                                        &mSuccess);
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    mCacheStorageDBConnection->OnPutComplete(mRequestId, rv, mSuccess);
  }

private:
  virtual ~PutRunnable() { }
  const nsID mCacheId;
  bool mSuccess;
};

class CacheStorageDBConnection::DeleteRunnable MOZ_FINAL :
  public CacheStorageDBConnection::OpenRunnable
{
public:
  DeleteRunnable(Namespace aNamespace, const nsACString& aOrigin,
                 const nsACString& aBaseDomain, RequestId aRequestId,
                 const nsAString& aKey,
                 CacheStorageDBConnection* aCacheStorageDBConnection)
    : OpenRunnable(aNamespace, aOrigin, aBaseDomain, false, aRequestId, aKey,
                   aCacheStorageDBConnection)
    , mSuccess(false)
  { }

protected:
  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheStorageDBSchema::Delete(aConnection, mNamespace, mKey,
                                           &mSuccess);
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    mCacheStorageDBConnection->OnDeleteComplete(mRequestId, rv, mSuccess);
  }

private:
  virtual ~DeleteRunnable() { }
  bool mSuccess;
};

class CacheStorageDBConnection::KeysRunnable MOZ_FINAL :
  public CacheStorageDBConnection::OpenRunnable
{
public:
  KeysRunnable(Namespace aNamespace, const nsACString& aOrigin,
               const nsACString& aBaseDomain, RequestId aRequestId,
               CacheStorageDBConnection* aCacheStorageDBConnection)
    : OpenRunnable(aNamespace, aOrigin, aBaseDomain, false, aRequestId,
                   NS_LITERAL_STRING(""), aCacheStorageDBConnection)
  { }

protected:
  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheStorageDBSchema::Keys(aConnection, mNamespace, mKeys);
  }

  virtual void CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    if (NS_FAILED(rv)) {
      mKeys.Clear();
    }
    mCacheStorageDBConnection->OnKeysComplete(mRequestId, rv, mKeys);
  }

private:
  virtual ~KeysRunnable() { }
  nsTArray<nsString> mKeys;
};

CacheStorageDBConnection::
CacheStorageDBConnection(CacheStorageDBListener* aListener,
                         Namespace aNamespace,
                         const nsACString& aOrigin,
                         const nsACString& aBaseDomain)
  : mListener(aListener)
  , mNamespace(aNamespace)
  , mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
{
  MOZ_ASSERT(mListener);
}

void
CacheStorageDBConnection::ClearListener()
{
  MOZ_ASSERT(mListener);
  mListener = nullptr;
}

void
CacheStorageDBConnection::Get(RequestId aRequestId, const nsAString& aKey)
{
  nsRefPtr<GetRunnable> get = new GetRunnable(mNamespace, mOrigin, mBaseDomain,
                                              aRequestId, aKey, this);
  if (!get) {
    OnGetComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY, nullptr);
    return;
  }
  get->Dispatch();
}

void
CacheStorageDBConnection::Has(RequestId aRequestId, const nsAString& aKey)
{
  nsRefPtr<HasRunnable> has = new HasRunnable(mNamespace, mOrigin, mBaseDomain,
                                              aRequestId, aKey, this);
  if (!has) {
    OnHasComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY, false);
    return;
  }
  has->Dispatch();
}

void
CacheStorageDBConnection::Put(RequestId aRequestId, const nsAString& aKey,
                              const nsID& aCacheId)
{
  nsRefPtr<PutRunnable> put = new PutRunnable(mNamespace, mOrigin, mBaseDomain,
                                              aRequestId, aKey, aCacheId, this);
  if (!put) {
    OnPutComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY, false);
    return;
  }
  put->Dispatch();
}

void
CacheStorageDBConnection::Delete(RequestId aRequestId, const nsAString& aKey)
{
  nsRefPtr<DeleteRunnable> del = new DeleteRunnable(mNamespace, mOrigin,
                                                    mBaseDomain, aRequestId,
                                                    aKey, this);
  if (!del) {
    OnDeleteComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY, false);
    return;
  }
  del->Dispatch();
}

void
CacheStorageDBConnection::Keys(RequestId aRequestId)
{
  nsRefPtr<KeysRunnable> keys = new KeysRunnable(mNamespace, mOrigin,
                                                 mBaseDomain, aRequestId, this);
  if (!keys) {
    OnKeysComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY, nsTArray<nsString>());
    return;
  }
  keys->Dispatch();
}

CacheStorageDBConnection::~CacheStorageDBConnection()
{
  MOZ_ASSERT(!mListener);
}

void
CacheStorageDBConnection::OnGetComplete(RequestId aRequestId, nsresult aRv,
                                        nsID* aCacheId)
{
  // TODO: assert on owning thread
  if (mListener) {
    mListener->OnGet(aRequestId, aRv, aCacheId);
  }
}

void
CacheStorageDBConnection::OnHasComplete(cache::RequestId aRequestId,
                                        nsresult aRv, bool aSuccess)
{
  // TODO: assert on owning thread
  if (mListener) {
    mListener->OnHas(aRequestId, aRv, aSuccess);
  }
}

void
CacheStorageDBConnection::OnPutComplete(RequestId aRequestId, nsresult aRv,
                                        bool aSuccess)
{
  // TODO: assert on owning thread
  if (mListener) {
    mListener->OnPut(aRequestId, aRv, aSuccess);
  }
}

void
CacheStorageDBConnection::OnDeleteComplete(RequestId aRequestId, nsresult aRv,
                                           bool aSuccess)
{
  // TODO: assert on owning thread
  if (mListener) {
    mListener->OnDelete(aRequestId, aRv, aSuccess);
  }
}

void
CacheStorageDBConnection::OnKeysComplete(RequestId aRequestId, nsresult aRv,
                                         const nsTArray<nsString>& aKeys)
{
  // TODO: assert on owning thread
  if (mListener) {
    mListener->OnKeys(aRequestId, aRv, aKeys);
  }
}

} // namespace dom
} // namespace mozilla
