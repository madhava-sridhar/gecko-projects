/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/DBSchema.h"

#include "ipc/IPCMessageUtils.h"
#include "mozilla/dom/cache/PCacheQueryParams.h"
#include "mozilla/dom/cache/PCacheRequest.h"
#include "mozilla/dom/cache/PCacheResponse.h"
#include "mozIStorageConnection.h"
#include "mozIStorageStatement.h"
#include "nsCOMPtr.h"
#include "nsTArray.h"

namespace mozilla {
namespace dom {
namespace cache {


const int32_t DBSchema::kLatestSchemaVersion = 1;
const int32_t DBSchema::kMaxEntriesPerStatement = 255;

using mozilla::void_t;

// static
nsresult
DBSchema::CreateSchema(mozIStorageConnection* aConn)
{
  MOZ_ASSERT(aConn);

  nsAutoCString pragmas(
#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
    // Switch the journaling mode to TRUNCATE to avoid changing the directory
    // structure at the conclusion of every transaction for devices with slower
    // file systems.
    "PRAGMA journal_mode = TRUNCATE; "
#endif
    "PRAGMA foreign_keys = ON; "
  );

  nsresult rv = aConn->ExecuteSimpleSQL(pragmas);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  int32_t schemaVersion;
  rv = aConn->GetSchemaVersion(&schemaVersion);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (schemaVersion == kLatestSchemaVersion) {
    return rv;
  }

  if (!schemaVersion) {
    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE caches ("
        "id INTEGER NOT NULL PRIMARY KEY "
      ");"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE entries ("
        "id INTEGER NOT NULL PRIMARY KEY, "
        "request_method TEXT NOT NULL, "
        "request_url TEXT NOT NULL, "
        "request_url_no_query TEXT NOT NULL, "
        "request_mode INTEGER NOT NULL, "
        "request_credentials INTEGER NOT NULL, "
        //"request_body_file TEXT NOT NULL, "
        "response_type INTEGER NOT NULL, "
        "response_status INTEGER NOT NULL, "
        "response_status_text TEXT NOT NULL, "
        //"response_body_file TEXT NOT NULL "
        "cache_id INTEGER NOT NULL REFERENCES caches(id) ON DELETE CASCADE"
      ");"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE INDEX entries_request_url_index "
                "ON entries (request_url);"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE INDEX entries_request_url_no_query_index "
                "ON entries (request_url_no_query);"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE request_headers ("
        "name TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE"
      ");"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE response_headers ("
        "name TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "entry_id INTEGER NOT NULL REFERENCES entries(id) ON DELETE CASCADE"
      ");"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    // We need an index on response_headers, but not on request_headers,
    // because we quickly need to determine if a VARY header is present.
    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE INDEX response_headers_name_index "
                "ON response_headers (name);"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE storage ("
        "namespace INTEGER NOT NULL, "
        "key TEXT NOT NULL, "
        "cache_id INTEGER NOT NULL REFERENCES caches(id), "
        "PRIMARY KEY(namespace, key) "
      ");"
    ));
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->SetSchemaVersion(kLatestSchemaVersion);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = aConn->GetSchemaVersion(&schemaVersion);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  if (schemaVersion != kLatestSchemaVersion) {
    return NS_ERROR_FAILURE;
  }

  return rv;
}

// static
nsresult
DBSchema::CreateCache(mozIStorageConnection* aConn, CacheId* aCacheIdOut)
{
  MOZ_ASSERT(aConn);
  MOZ_ASSERT(aCacheIdOut);

  nsresult rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO caches DEFAULT VALUES;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsCOMPtr<mozIStorageStatement> state;
  rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT last_insert_rowid()"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  rv = state->ExecuteStep(&hasMoreData);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  if (NS_WARN_IF(!hasMoreData)) { return NS_ERROR_UNEXPECTED; }

  rv = state->GetInt32(0, aCacheIdOut);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
DBSchema::DeleteCache(mozIStorageConnection* aConn, CacheId aCacheId)
{
  MOZ_ASSERT(aConn);

  // Dependent data removed by ON DELETE CASCADE in schema definition.

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "DELETE FROM caches WHERE id=?1;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aCacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
DBSchema::IsCacheOrphaned(mozIStorageConnection* aConn,
                          CacheId aCacheId, bool* aOrphanedOut)
{
  MOZ_ASSERT(aConn);
  MOZ_ASSERT(aOrphanedOut);

  // err on the side of not deleting user data
  *aOrphanedOut = false;

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT COUNT(*) FROM storage WHERE cache_id=?1;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aCacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  rv = state->ExecuteStep(&hasMoreData);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  MOZ_ASSERT(hasMoreData);

  int32_t refCount;
  rv = state->GetInt32(0, &refCount);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  *aOrphanedOut = refCount < 1;

  return rv;
}

// static
nsresult
DBSchema::CacheMatch(mozIStorageConnection* aConn, CacheId aCacheId,
                     const PCacheRequest& aRequest,
                     const PCacheQueryParams& aParams,
                     PCacheResponseOrVoid* aResponseOrVoidOut)
{
  MOZ_ASSERT(aConn);
  MOZ_ASSERT(aResponseOrVoidOut);

  nsTArray<EntryId> matches;
  nsresult rv = QueryCache(aConn, aCacheId, aRequest, aParams, matches);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (matches.Length() < 1) {
    *aResponseOrVoidOut = void_t();
    return rv;
  }

  PCacheResponse response;
  rv = ReadResponse(aConn, matches[0], response);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  *aResponseOrVoidOut = response;
  return rv;
}

// static
nsresult
DBSchema::CacheMatchAll(mozIStorageConnection* aConn, CacheId aCacheId,
                        const PCacheRequestOrVoid& aRequestOrVoid,
                        const PCacheQueryParams& aParams,
                        nsTArray<PCacheResponse>& aResponsesOut)
{
  MOZ_ASSERT(aConn);
  nsresult rv;

  nsTArray<EntryId> matches;
  if (aRequestOrVoid.type() == PCacheRequestOrVoid::Tvoid_t) {
    rv = QueryAll(aConn, aCacheId, matches);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  } else {
    rv = QueryCache(aConn, aCacheId, aRequestOrVoid, aParams, matches);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  // TODO: replace this with a bulk load using SQL IN clause
  for (uint32_t i = 0; i < matches.Length(); ++i) {
    PCacheResponse *response = aResponsesOut.AppendElement();
    rv = ReadResponse(aConn, matches[i], *response);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  return rv;
}

// static
nsresult
DBSchema::CachePut(mozIStorageConnection* aConn, CacheId aCacheId,
                   const PCacheRequest& aRequest,
                   const PCacheResponse& aResponse)
{
  MOZ_ASSERT(aConn);

  PCacheQueryParams params(false, false, false, false, false,
                           NS_LITERAL_STRING(""));
  nsTArray<EntryId> matches;
  nsresult rv = QueryCache(aConn, aCacheId, aRequest, params, matches);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = DeleteEntries(aConn, matches);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = InsertEntry(aConn, aCacheId, aRequest, aResponse);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
DBSchema::CacheDelete(mozIStorageConnection* aConn, CacheId aCacheId,
                      const PCacheRequest& aRequest,
                      const PCacheQueryParams& aParams, bool* aSuccessOut)
{
  MOZ_ASSERT(aConn);
  MOZ_ASSERT(aSuccessOut);

  nsTArray<EntryId> matches;
  nsresult rv = QueryCache(aConn, aCacheId, aRequest, aParams, matches);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (matches.Length() < 1) {
    *aSuccessOut = false;
    return rv;
  }

  rv = DeleteEntries(aConn, matches);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  *aSuccessOut = true;

  return rv;
}

// static
nsresult
DBSchema::StorageGetCacheId(mozIStorageConnection* aConn, Namespace aNamespace,
                            const nsAString& aKey, bool* aFoundCacheOut,
                            CacheId* aCacheIdOut)
{
  MOZ_ASSERT(aConn);
  MOZ_ASSERT(aFoundCacheOut);
  MOZ_ASSERT(aCacheIdOut);

  *aFoundCacheOut = false;

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT cache_id FROM storage WHERE namespace=?1 AND key=?2;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aNamespace);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindStringParameter(1, aKey);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  rv = state->ExecuteStep(&hasMoreData);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (!hasMoreData) {
    return rv;
  }

  rv = state->GetInt32(0, aCacheIdOut);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  *aFoundCacheOut = true;
  return rv;
}

// static
nsresult
DBSchema::StoragePutCache(mozIStorageConnection* aConn, Namespace aNamespace,
                          const nsAString& aKey, CacheId aCacheId)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO storage (namespace, key, cache_id) VALUES(?1, ?2, ?3);"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aNamespace);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindStringParameter(1, aKey);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(2, aCacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
DBSchema::StorageForgetCache(mozIStorageConnection* aConn, Namespace aNamespace,
                             const nsAString& aKey)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "DELETE FROM storage WHERE namespace=?1 AND key=?2;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aNamespace);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindStringParameter(1, aKey);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return rv;
}

// static
nsresult
DBSchema::StorageGetKeys(mozIStorageConnection* aConn, Namespace aNamespace,
                         nsTArray<nsString>& aKeysOut)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT key FROM storage WHERE namespace=?1 ORDER BY rowid;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aNamespace);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  while(NS_SUCCEEDED(state->ExecuteStep(&hasMoreData)) && hasMoreData) {
    nsString* key = aKeysOut.AppendElement();
    rv = state->GetString(0, *key);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  return rv;
}

// static
nsresult
DBSchema::QueryAll(mozIStorageConnection* aConn, CacheId aCacheId,
                   nsTArray<EntryId>& aEntryIdListOut)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT id FROM entries WHERE cache_id=?1 ORDER BY id;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aCacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  while(NS_SUCCEEDED(state->ExecuteStep(&hasMoreData)) && hasMoreData) {
    EntryId* entryId = aEntryIdListOut.AppendElement();
    rv = state->GetInt32(0, entryId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  return rv;
}

// static
nsresult
DBSchema::QueryCache(mozIStorageConnection* aConn, CacheId aCacheId,
                     const PCacheRequest& aRequest,
                     const PCacheQueryParams& aParams,
                     nsTArray<EntryId>& aEntryIdListOut)
{
  MOZ_ASSERT(aConn);

  nsTArray<PCacheRequest> requestArray;
  nsTArray<PCacheResponse> responseArray;

  // TODO: throw if new Request() would have failed:
  // TODO:    - throw if aRequest is no CORS and method is not simple method

  if (!aParams.ignoreMethod() && !aRequest.method().LowerCaseEqualsLiteral("get")
                              && !aRequest.method().LowerCaseEqualsLiteral("head"))
  {
    return NS_OK;
  }

  nsAutoCString query(
    "SELECT id, COUNT(response_headers.name) AS vary_count "
    "FROM entries "
    "LEFT OUTER JOIN response_headers ON entries.id=response_headers.entry_id "
                                    "AND response_headers.name='vary' "
    "WHERE entries.cache_id=?1 "
      "AND entries."
  );

  nsAutoString urlToMatch;
  if (aParams.ignoreSearch()) {
    urlToMatch = aRequest.urlWithoutQuery();
    query.Append(NS_LITERAL_CSTRING("request_url_no_query"));
  } else {
    urlToMatch = aRequest.url();
    query.Append(NS_LITERAL_CSTRING("request_url"));
  }

  nsAutoCString urlComparison;
  if (aParams.prefixMatch()) {
    urlToMatch.AppendLiteral("%");
    query.Append(NS_LITERAL_CSTRING(" LIKE ?2 ESCAPE '\\'"));
  } else {
    query.Append(NS_LITERAL_CSTRING("=?2"));
  }

  query.Append(NS_LITERAL_CSTRING(" GROUP BY entries.id ORDER BY entries.id;"));

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(query, getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aCacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  if (aParams.prefixMatch()) {
    nsAutoString escapedUrlToMatch;
    rv = state->EscapeStringForLIKE(urlToMatch, '\\', escapedUrlToMatch);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    urlToMatch = escapedUrlToMatch;
  }

  rv = state->BindStringParameter(1, urlToMatch);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  while(NS_SUCCEEDED(state->ExecuteStep(&hasMoreData)) && hasMoreData) {
    EntryId entryId;
    rv = state->GetInt32(0, &entryId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    int32_t varyCount;
    rv = state->GetInt32(1, &varyCount);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    if (!aParams.ignoreVary() && varyCount > 0) {
      bool matchedByVary;
      rv = MatchByVaryHeader(aConn, aRequest, entryId, &matchedByVary);
      if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
      if (matchedByVary) {
        continue;
      }
    }

    aEntryIdListOut.AppendElement(entryId);
  }

  return rv;
}

// static
nsresult
DBSchema::MatchByVaryHeader(mozIStorageConnection* aConn,
                            const PCacheRequest& aRequest,
                            EntryId entryId, bool* aSuccessOut)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT value FROM response_headers "
    "WHERE name='vary' AND entry_id=?1;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, entryId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsTArray<nsCString> varyValues;

  bool hasMoreData;
  while(NS_SUCCEEDED(state->ExecuteStep(&hasMoreData)) && hasMoreData) {
    nsCString* value = varyValues.AppendElement();
    rv = state->GetUTF8String(0, *value);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  // Should not have called this function if this was not the case
  MOZ_ASSERT(varyValues.Length() > 0);

  state->Reset();
  rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT name, value FROM request_headers "
    "WHERE AND entry_id=?1;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, entryId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsRefPtr<Headers> cachedHeaders = new Headers(nullptr);

  ErrorResult errorResult;

  while(NS_SUCCEEDED(state->ExecuteStep(&hasMoreData)) && hasMoreData) {
    nsAutoCString name;
    nsAutoCString value;
    rv = state->GetUTF8String(0, name);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
    rv = state->GetUTF8String(1, value);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    cachedHeaders->Append(name, value, errorResult);
    if (errorResult.Failed()) { return errorResult.ErrorCode(); };
  }
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  nsRefPtr<Headers> queryHeaders = new Headers(nullptr, aRequest.headers());

  for (uint32_t i = 0; i < varyValues.Length(); ++i) {
    if (varyValues[i].EqualsLiteral("*")) {
      continue;
    }

    nsAutoCString queryValue;
    queryHeaders->Get(varyValues[i], queryValue, errorResult);
    if (errorResult.Failed()) { return errorResult.ErrorCode(); };

    nsAutoCString cachedValue;
    cachedHeaders->Get(varyValues[i], cachedValue, errorResult);
    if (errorResult.Failed()) { return errorResult.ErrorCode(); };

    if (queryValue != cachedValue) {
      *aSuccessOut = false;
      return rv;
    }
  }

  *aSuccessOut = true;
  return rv;
}

// static
nsresult
DBSchema::DeleteEntries(mozIStorageConnection* aConn,
                        const nsTArray<EntryId>& aEntryIdList,
                        uint32_t aPos, int32_t aLen)
{
  MOZ_ASSERT(aConn);

  if (aEntryIdList.Length() < 1) {
    return NS_OK;
  }

  MOZ_ASSERT(aPos < aEntryIdList.Length());

  if (aLen < 0) {
    aLen = aEntryIdList.Length() - aPos;
  }

  // Sqlite limits the number of entries allowed for an IN clause,
  // so split up larger operations.
  if (aLen > kMaxEntriesPerStatement) {
    uint32_t curPos = aPos;
    int32_t remaining = aLen;
    while (remaining > 0) {
      int32_t max = kMaxEntriesPerStatement;
      int32_t curLen = std::min(max, remaining);
      nsresult rv = DeleteEntries(aConn, aEntryIdList, curPos, curLen);
      if (NS_FAILED(rv)) { return rv; }

      curPos += curLen;
      remaining -= curLen;
    }
    return NS_OK;
  }

  // Dependent records removed via ON DELETE CASCADE

  nsCOMPtr<mozIStorageStatement> state;
  nsAutoCString query(
    "DELETE FROM entries WHERE id IN ("
  );
  AppendListParamsToQuery(query, aEntryIdList, aPos, aLen);
  query.Append(NS_LITERAL_CSTRING(")"));

  nsresult rv = aConn->CreateStatement(query, getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = BindListParamsToQuery(state, aEntryIdList, aPos, aLen);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  return NS_OK;
}

// static
nsresult
DBSchema::InsertEntry(mozIStorageConnection* aConn, CacheId aCacheId,
                      const PCacheRequest& aRequest,
                      const PCacheResponse& aResponse)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO entries ("
      "request_method, "
      "request_url, "
      "request_url_no_query, "
      "request_mode, "
      "request_credentials, "
      "response_type, "
      "response_status, "
      "response_status_text, "
      "cache_id "
    ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindUTF8StringParameter(0, aRequest.method());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindStringParameter(1, aRequest.url());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindStringParameter(2, aRequest.urlWithoutQuery());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(3, static_cast<int32_t>(aRequest.mode()));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(4,
    static_cast<int32_t>(aRequest.credentials()));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(5, static_cast<int32_t>(aResponse.type()));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(6, aResponse.status());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindUTF8StringParameter(7, aResponse.statusText());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(8, aCacheId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT last_insert_rowid()"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  rv = state->ExecuteStep(&hasMoreData);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  int32_t entryId;
  rv = state->GetInt32(0, &entryId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO request_headers ("
      "name, "
      "value, "
      "entry_id "
    ") VALUES (?1, ?2, ?3)"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  const nsTArray<PHeadersEntry>& requestHeaders = aRequest.headers();
  for (uint32_t i = 0; i < requestHeaders.Length(); ++i) {
    rv = state->BindUTF8StringParameter(0, requestHeaders[i].name());
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->BindUTF8StringParameter(1, requestHeaders[i].value());
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->BindInt32Parameter(2, entryId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->Execute();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO response_headers ("
      "name, "
      "value, "
      "entry_id "
    ") VALUES (?1, ?2, ?3)"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  const nsTArray<PHeadersEntry>& responseHeaders = aResponse.headers();
  for (uint32_t i = 0; i < responseHeaders.Length(); ++i) {
    rv = state->BindUTF8StringParameter(0, responseHeaders[i].name());
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->BindUTF8StringParameter(1, responseHeaders[i].value());
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->BindInt32Parameter(2, entryId);
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->Execute();
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  return NS_OK;
}

// static
nsresult
DBSchema::ReadResponse(mozIStorageConnection* aConn,
                       EntryId aEntryId, PCacheResponse& aResponseOut)
{
  MOZ_ASSERT(aConn);
  nsCOMPtr<mozIStorageStatement> state;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT "
      "response_type, "
      "response_status, "
      "response_status_text "
    "FROM entries "
    "WHERE id=?1;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aEntryId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  bool hasMoreData;
  rv = state->ExecuteStep(&hasMoreData);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  int32_t type;
  rv = state->GetInt32(0, &type);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  aResponseOut.type() = static_cast<ResponseType>(type);

  int32_t status;
  rv = state->GetInt32(1, &status);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  aResponseOut.status() = status;

  rv = state->GetUTF8String(2, aResponseOut.statusText());
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT "
      "name, "
      "value "
    "FROM response_headers "
    "WHERE entry_id=?1;"
  ), getter_AddRefs(state));
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  rv = state->BindInt32Parameter(0, aEntryId);
  if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

  while(NS_SUCCEEDED(state->ExecuteStep(&hasMoreData)) && hasMoreData) {
    PHeadersEntry* header = aResponseOut.headers().AppendElement();

    rv = state->GetUTF8String(0, header->name());
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }

    rv = state->GetUTF8String(1, header->value());
    if (NS_WARN_IF(NS_FAILED(rv))) { return rv; }
  }

  return rv;
}

// static
void
DBSchema::AppendListParamsToQuery(nsACString& aQuery,
                                  const nsTArray<EntryId>& aEntryIdList,
                                  uint32_t aPos, int32_t aLen)
{
  MOZ_ASSERT((aPos + aLen) <= aEntryIdList.Length());
  for (int32_t i = aPos; i < aLen; ++i) {
    if (i == 0) {
      aQuery.Append(NS_LITERAL_CSTRING("?"));
    } else {
      aQuery.Append(NS_LITERAL_CSTRING(",?"));
    }
  }
}

// static
nsresult
DBSchema::BindListParamsToQuery(mozIStorageStatement* aState,
                                const nsTArray<EntryId>& aEntryIdList,
                                uint32_t aPos, int32_t aLen)
{
  MOZ_ASSERT((aPos + aLen) <= aEntryIdList.Length());
  for (int32_t i = aPos; i < aLen; ++i) {
    nsresult rv = aState->BindInt32Parameter(i, aEntryIdList[i]);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return NS_OK;
}

} // namespace cache
} // namespace dom
} // namespace mozilla
