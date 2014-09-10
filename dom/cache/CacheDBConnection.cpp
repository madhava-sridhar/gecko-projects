/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheDBConnection.h"

#include "mozilla/dom/CacheDBListener.h"
#include "mozilla/dom/CacheDBSchema.h"
#include "mozilla/dom/CacheQuotaRunnable.h"
#include "mozilla/dom/CacheTypes.h"
#include "mozilla/dom/Headers.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "nsServiceManagerUtils.h"
#include "mozIStorageConnection.h"
#include "mozIStorageService.h"
#include "mozIStorageStatement.h"
#include "mozStorageCID.h"
#include "mozStorageHelper.h"
#include "nsIFile.h"
#include "nsIUUIDGenerator.h"
#include "nsNetUtil.h"

namespace mozilla {
namespace dom {

using mozilla::dom::cache::RequestId;
using mozilla::dom::quota::PERSISTENCE_TYPE_PERSISTENT;
using mozilla::dom::quota::PersistenceType;
using mozilla::dom::quota::QuotaManager;

class CacheDBConnection::OpenRunnable : public CacheQuotaRunnable
{
public:
  OpenRunnable(const nsACString& aOrigin, const nsACString& aBaseDomain,
               const nsACString& aQuotaId, bool aAllowCreate,
               const nsID& aCacheId, RequestId aRequestId,
               CacheDBConnection* aCacheDBConnection)
    : CacheQuotaRunnable(aOrigin, aBaseDomain, aQuotaId)
    , mAllowCreate(aAllowCreate)
    , mCacheId(aCacheId)
    , mRequestId(aRequestId)
    , mCacheDBConnection(aCacheDBConnection)
    , mResult(NS_OK)
  {
    MOZ_ASSERT(mCacheDBConnection);
  }

protected:
  virtual void AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection)=0;

  virtual ~OpenRunnable() { }
  virtual void RunOnQuotaIOThread(const nsACString& aOrigin,
                                  const nsACString& aBaseDomain,
                                  nsIFile* aQuotaDir) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aQuotaDir);

    mResult = aQuotaDir->Append(NS_LITERAL_STRING("cache"));
    if (NS_FAILED(mResult)) { return; }

    char cacheIdBuf[NSID_LENGTH];
    mCacheId.ToProvidedString(cacheIdBuf);
    mResult = aQuotaDir->Append(NS_ConvertUTF8toUTF16(cacheIdBuf));
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
    if (NS_WARN_IF(!dbFileUrl)) {
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
    if (NS_WARN_IF(!ss)) {
      mResult = NS_ERROR_FAILURE;
      return;
    }

    nsCOMPtr<mozIStorageConnection> conn;
    mResult = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(conn));
    if (mResult == NS_ERROR_FILE_CORRUPTED) {
      dbFile->Remove(false);
      if (NS_FAILED(mResult)) { return; }

      mResult = dbTmpDir->Exists(&exists);
      if (NS_FAILED(mResult)) { return; }

      if (exists) {
        bool isDir;
        mResult = dbTmpDir->IsDirectory(&isDir);
        if (NS_FAILED(mResult) || !isDir) { return; }
        mResult = dbTmpDir->Remove(true);
        if (NS_FAILED(mResult)) { return; }
      }

      mResult = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(conn));
    }
    if (NS_FAILED(mResult)) { return; }
    MOZ_ASSERT(conn);

    mResult = CacheDBSchema::Create(conn);
    if (NS_FAILED(mResult)) { return; }

    AfterOpenOnQuotaIOThread(conn);
  }

  const bool mAllowCreate;
  const nsID mCacheId;
  const RequestId mRequestId;
  nsRefPtr<CacheDBConnection> mCacheDBConnection;
  nsresult mResult;
};

class CacheDBConnection::MatchRunnable MOZ_FINAL :
  public CacheDBConnection::OpenRunnable
{
public:
  MatchRunnable(const nsACString& aOrigin, const nsACString& aBaseDomain,
               const nsACString& aQuotaId, const nsID& aCacheId,
               RequestId aRequestId, const PCacheRequest& aRequest,
               const PCacheQueryParams& aParams,
               CacheDBConnection* aCacheDBConnection)
    : OpenRunnable(aOrigin, aBaseDomain, aQuotaId, false, aCacheId,
                   aRequestId, aCacheDBConnection)
    , mRequest(aRequest)
    , mParams(aParams)
    , mResponseOrVoid(void_t())
  { }

protected:
  virtual ~MatchRunnable() { }

  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheDBSchema::Match(aConnection, mRequest, mParams,
                                   &mResponseOrVoid);
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    mCacheDBConnection->OnMatchComplete(mRequestId, rv, mResponseOrVoid);
  }

  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  PCacheResponseOrVoid mResponseOrVoid;
};

class CacheDBConnection::MatchAllRunnable MOZ_FINAL :
  public CacheDBConnection::OpenRunnable
{
public:
  MatchAllRunnable(const nsACString& aOrigin, const nsACString& aBaseDomain,
                   const nsACString& aQuotaId, const nsID& aCacheId,
                   RequestId aRequestId, const PCacheRequestOrVoid& aRequestOrVoid,
                   const PCacheQueryParams& aParams,
                   CacheDBConnection* aCacheDBConnection)
    : OpenRunnable(aOrigin, aBaseDomain, aQuotaId, false, aCacheId,
                   aRequestId, aCacheDBConnection)
    , mRequestOrVoid(aRequestOrVoid)
    , mParams(aParams)
  { }

protected:
  virtual ~MatchAllRunnable() { }

  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheDBSchema::MatchAll(aConnection, mRequestOrVoid, mParams,
                                      mResponses);
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    mCacheDBConnection->OnMatchAllComplete(mRequestId, rv, mResponses);
  }

  const PCacheRequestOrVoid mRequestOrVoid;
  const PCacheQueryParams mParams;
  nsTArray<PCacheResponse> mResponses;
};

class CacheDBConnection::PutRunnable MOZ_FINAL :
  public CacheDBConnection::OpenRunnable
{
public:
  PutRunnable(const nsACString& aOrigin, const nsACString& aBaseDomain,
              const nsACString& aQuotaId, const nsID& aCacheId,
              RequestId aRequestId, const PCacheRequest& aRequest,
              const PCacheResponse& aResponse,
              CacheDBConnection* aCacheDBConnection)
    : OpenRunnable(aOrigin, aBaseDomain, aQuotaId, true, aCacheId,
                   aRequestId, aCacheDBConnection)
    , mRequest(aRequest)
    , mResponse(aResponse)
  { }

protected:
  virtual ~PutRunnable() { }

  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheDBSchema::Put(aConnection, mRequest, mResponse);
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    PCacheResponseOrVoid responseOrVoid;
    if (NS_FAILED(rv)) {
      responseOrVoid = void_t();
    } else {
      responseOrVoid = mResponse;
    }
    mCacheDBConnection->OnPutComplete(mRequestId, rv, responseOrVoid);
  }

  const PCacheRequest mRequest;
  const PCacheResponse mResponse;
};

class CacheDBConnection::DeleteRunnable MOZ_FINAL :
  public CacheDBConnection::OpenRunnable
{
public:
  DeleteRunnable(const nsACString& aOrigin, const nsACString& aBaseDomain,
                 const nsACString& aQuotaId, const nsID& aCacheId,
                 RequestId aRequestId, const PCacheRequest& aRequest,
                 const PCacheQueryParams& aParams,
                 CacheDBConnection* aCacheDBConnection)
    : OpenRunnable(aOrigin, aBaseDomain, aQuotaId, false, aCacheId,
                   aRequestId, aCacheDBConnection)
    , mRequest(aRequest)
    , mParams(aParams)
    , mSuccess(false)
  { }

protected:
  virtual ~DeleteRunnable() { }

  virtual void
  AfterOpenOnQuotaIOThread(mozIStorageConnection* aConnection) MOZ_OVERRIDE
  {
    mResult = CacheDBSchema::Delete(aConnection, mRequest, mParams, &mSuccess);
  }

  virtual void
  CompleteOnInitiatingThread(nsresult aRv) MOZ_OVERRIDE
  {
    nsresult rv = NS_FAILED(aRv) ? aRv : mResult;
    mCacheDBConnection->OnDeleteComplete(mRequestId, rv, mSuccess);
  }

  const PCacheRequest mRequest;
  const PCacheQueryParams mParams;
  bool mSuccess;
};

//static
already_AddRefed<CacheDBConnection>
CacheDBConnection::Create(CacheDBListener* aListener, const nsACString& aOrigin,
       const nsACString& aBaseDomain)
{
  nsresult rv;
  nsCOMPtr<nsIUUIDGenerator> uuidgen =
    do_GetService("@mozilla.org/uuid-generator;1", &rv);
  if (NS_WARN_IF(NS_FAILED(rv) || !uuidgen)) {
    return nullptr;
  }

  nsID cacheId;
  rv = uuidgen->GenerateUUIDInPlace(&cacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  nsRefPtr<CacheDBConnection> ref = new CacheDBConnection(aListener, aOrigin,
                                                          aBaseDomain, cacheId);
  return ref.forget();
}

CacheDBConnection::CacheDBConnection(CacheDBListener* aListener,
                                     const nsACString& aOrigin,
                                     const nsACString& aBaseDomain,
                                     const nsID& aCacheId)
  : mListener(aListener)
  , mOrigin(aOrigin)
  , mBaseDomain(aBaseDomain)
  , mCacheId(aCacheId)
  , mQuotaId("Cache:")
{
  MOZ_ASSERT(mListener);

  char cacheIdBuf[NSID_LENGTH];
  mCacheId.ToProvidedString(cacheIdBuf);
  mQuotaId.Append(cacheIdBuf);
}

void
CacheDBConnection::ClearListener()
{
  MOZ_ASSERT(mListener);
  mListener = nullptr;
}

void
CacheDBConnection::Match(RequestId aRequestId, const PCacheRequest& aRequest,
                         const PCacheQueryParams& aParams)
{
  nsRefPtr<MatchRunnable> match = new MatchRunnable(mOrigin, mBaseDomain, mQuotaId,
                                                    mCacheId, aRequestId, aRequest,
                                                    aParams, this);
  if (!match) {
    OnMatchComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY,
                    PCacheResponseOrVoid(void_t()));
    return;
  }
  match->Dispatch();
}

void
CacheDBConnection::MatchAll(RequestId aRequestId,
                            const PCacheRequestOrVoid& aRequest,
                            const PCacheQueryParams& aParams)
{
  nsRefPtr<MatchAllRunnable> match = new MatchAllRunnable(mOrigin, mBaseDomain,
                                                          mQuotaId, mCacheId,
                                                          aRequestId, aRequest,
                                                          aParams, this);
  if (!match) {
    OnMatchAllComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY,
                       nsTArray<PCacheResponse>());
    return;
  }
  match->Dispatch();
}

// TODO: Make sure AddAll() doesn't delete entries it just created.

void
CacheDBConnection::Put(RequestId aRequestId, const PCacheRequest& aRequest,
                       const PCacheResponse& aResponse)
{
  nsRefPtr<PutRunnable> put = new PutRunnable(mOrigin, mBaseDomain, mQuotaId,
                                              mCacheId, aRequestId, aRequest,
                                              aResponse, this);
  if (!put) {
    OnPutComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY,
                  PCacheResponseOrVoid(void_t()));
    return;
  }
  put->Dispatch();
}

void
CacheDBConnection::Delete(cache::RequestId aRequestId,
                          const PCacheRequest& aRequest,
                          const PCacheQueryParams& aParams)
{
  nsRefPtr<DeleteRunnable> del = new DeleteRunnable(mOrigin, mBaseDomain,
                                                    mQuotaId, mCacheId,
                                                    aRequestId, aRequest,
                                                    aParams, this);
  if (!del) {
    OnDeleteComplete(aRequestId, NS_ERROR_OUT_OF_MEMORY, false);
    return;
  }
  del->Dispatch();
}

CacheDBConnection::~CacheDBConnection()
{
  MOZ_ASSERT(!mListener);
}

void
CacheDBConnection::OnMatchComplete(RequestId aRequestId, nsresult aRv,
                                   const PCacheResponseOrVoid& aResponse)
{
  if (mListener) {
    mListener->OnMatch(aRequestId, aRv, aResponse);
  }
}

void
CacheDBConnection::OnMatchAllComplete(RequestId aRequestId, nsresult aRv,
                                      const nsTArray<PCacheResponse>& aResponses)
{
  if (mListener) {
    mListener->OnMatchAll(aRequestId, aRv, aResponses);
  }
}

void
CacheDBConnection::OnPutComplete(RequestId aRequestId, nsresult aRv,
                                 const PCacheResponseOrVoid& aResponse)
{
  if (mListener) {
    mListener->OnPut(aRequestId, aRv, aResponse);
  }
}

void
CacheDBConnection::OnDeleteComplete(RequestId aRequestId, nsresult aRv,
                                    bool aSuccess)
{
  if (mListener) {
    mListener->OnDelete(aRequestId, aRv, aSuccess);
  }
}


} // namespace dom
} // namespace mozilla
