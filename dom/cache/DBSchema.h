/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_DBSchema_h
#define mozilla_dom_cache_DBSchema_h

#include "mozilla/Attributes.h"
#include "mozilla/dom/cache/Types.h"
#include "nsError.h"
#include "nsString.h"

class mozIStorageConnection;
class mozIStorageStatement;
template<class T> class nsTArray;

namespace mozilla {
namespace dom {
namespace cache {

class PCacheQueryParams;
class PCacheRequest;
class PCacheRequestOrVoid;
class PCacheResponse;
class PCacheResponseOrVoid;

class DBSchema MOZ_FINAL
{
public:
  static nsresult CreateSchema(mozIStorageConnection* aConn);

  static nsresult CreateCache(mozIStorageConnection* aConn,
                              CacheId* aCacheIdOut);
  // TODO: improve naming (confusing with CacheDelete)
  static nsresult DeleteCache(mozIStorageConnection* aConn, CacheId aCacheId);

  static nsresult IsCacheOrphaned(mozIStorageConnection* aConn,
                                  CacheId aCacheId, bool* aOrphanedOut);

  static nsresult CacheMatch(mozIStorageConnection* aConn, CacheId aCacheId,
                             const PCacheRequest& aRequest,
                             const PCacheQueryParams& aParams,
                             PCacheResponseOrVoid* aResponseOrVoidOut);
  static nsresult CacheMatchAll(mozIStorageConnection* aConn, CacheId aCacheId,
                                const PCacheRequestOrVoid& aRequestOrVoid,
                                const PCacheQueryParams& aParams,
                                nsTArray<PCacheResponse>& aResponsesOut);
  static nsresult CachePut(mozIStorageConnection* aConn, CacheId aCacheId,
                           const PCacheRequest& aRequest,
                           const PCacheResponse& aResponse);
  static nsresult CacheDelete(mozIStorageConnection* aConn, CacheId aCacheId,
                              const PCacheRequest& aRequest,
                              const PCacheQueryParams& aParams,
                              bool* aSuccessOut);

  static nsresult StorageGetCacheId(mozIStorageConnection* aConn,
                                    Namespace aNamespace, const nsAString& aKey,
                                    bool* aFoundCacheOut, CacheId* aCacheIdOut);
  static nsresult StoragePutCache(mozIStorageConnection* aConn,
                                  Namespace aNamespace, const nsAString& aKey,
                                  CacheId aCacheId);
  static nsresult StorageForgetCache(mozIStorageConnection* aConn,
                                     Namespace aNamespace,
                                     const nsAString& aKey);
  static nsresult StorageGetKeys(mozIStorageConnection* aConn,
                                 Namespace aNamespace,
                                 nsTArray<nsString>& aKeysOut);

private:
  typedef int32_t EntryId;

  static nsresult QueryAll(mozIStorageConnection* aConn, CacheId aCacheId,
                           nsTArray<EntryId>& aEntryIdListOut);
  static nsresult QueryCache(mozIStorageConnection* aConn, CacheId aCacheId,
                             const PCacheRequest& aRequest,
                             const PCacheQueryParams& aParams,
                             nsTArray<EntryId>& aEntryIdListOut);
  static nsresult MatchByVaryHeader(mozIStorageConnection* aConn,
                                    const PCacheRequest& aRequest,
                                    EntryId entryId, bool* aSuccessOut);
  static nsresult DeleteEntries(mozIStorageConnection* aConn,
                                const nsTArray<EntryId>& aEntryIdList,
                                uint32_t aPos=0, int32_t aLen=-1);
  static nsresult InsertEntry(mozIStorageConnection* aConn, CacheId aCacheId,
                              const PCacheRequest& aRequest,
                              const PCacheResponse& aResponse);
  static nsresult ReadResponse(mozIStorageConnection* aConn,
                               EntryId aEntryId, PCacheResponse& aResponseOut);

  static void AppendListParamsToQuery(nsACString& aQuery,
                                      const nsTArray<EntryId>& aEntryIdList,
                                      uint32_t aPos, int32_t aLen);
  static nsresult BindListParamsToQuery(mozIStorageStatement* aState,
                                        const nsTArray<EntryId>& aEntryIdList,
                                        uint32_t aPos, int32_t aLen);

  DBSchema() MOZ_DELETE;
  ~DBSchema() MOZ_DELETE;

  static const int32_t kLatestSchemaVersion = 1;
  static const int32_t kMaxEntriesPerStatement = 255;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_DBSchema_h
