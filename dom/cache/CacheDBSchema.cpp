/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheDBSchema.h"

namespace mozilla {
namespace dom {

static const int32_t MAX_ENTRIES_PER_STATEMENT = 255;

static void
AppendListParamsToQuery(nsACString& aQuery,
                        const nsTArray<CacheDBConnection::EntryId>& aEntryIdList,
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

static nsresult
BindListParamsToQuery(mozIStorageStatement* aStatement,
                      const nsTArray<CacheDBConnection::EntryId>& aEntryIdList,
                      uint32_t aPos, int32_t aLen)
{
  MOZ_ASSERT((aPos + aLen) <= aEntryIdList.Length());
  for (int32_t i = aPos; i < aLen; ++i) {
    nsresult rv = aStatement->BindInt32Parameter(i, aEntryIdList[i]);
    NS_ENSURE_SUCCESS(rv, rv);
  }
  return NS_OK;
}

// static
nsresult
CacheDBSchema::Create(mozIStorageConnection* aConn)
{
  MOZ_ASSERT(aConn);

  nsresult rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
    // Switch the journaling mode to TRUNCATE to avoid changing the directory
    // structure at the conclusion of every transaction for devices with slower
    // file systems.
    "PRAGMA journal_mode = TRUNCATE; "
#endif
    "PRAGMA foreign_keys = ON; "
  ));
  if (NS_FAILED(rv)) { return rv; }

  int32_t schemaVersion;
  rv = aConn->GetSchemaVersion(&schemaVersion);
  if (NS_FAILED(rv)) { return rv; }

  mozStorageTransaction trans(aConn, false,
                              mozIStorageConnection::TRANSACTION_IMMEDIATE);

  if (!schemaVersion) {
    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE map ("
        "id INTEGER NOT NULL PRIMARY KEY, "
        "request_method TEXT NOT NULL, "
        "request_url TEXT NOT NULL, "
        "request_url_no_query TEXT NOT NULL, "
        "request_mode INTEGER NOT NULL, "
        "request_credentials INTEGER NOT NULL, "
        //"request_body_file TEXT NOT NULL, "
        "response_type INTEGER NOT NULL, "
        "response_status INTEGER NOT NULL, "
        "response_status_text TEXT NOT NULL "
        //"response_body_file TEXT NOT NULL "
      ");"
    ));
    if (NS_FAILED(rv)) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE request_headers ("
        "name TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "map_id INTEGER NOT NULL REFERENCES map(id) "
      ");"
    ));
    if (NS_FAILED(rv)) { return rv; }

    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE response_headers ("
        "name TEXT NOT NULL, "
        "value TEXT NOT NULL, "
        "map_id INTEGER NOT NULL REFERENCES map(id) "
      ");"
    ));
    if (NS_FAILED(rv)) { return rv; }

    rv = aConn->SetSchemaVersion(kLatestSchemaVersion);
    if (NS_FAILED(rv)) { return rv; }

    rv = aConn->GetSchemaVersion(&schemaVersion);
    if (NS_FAILED(rv)) { return rv; }
  }

  if (schemaVersion != kLatestSchemaVersion) {
    return NS_ERROR_FAILURE;
  }

  rv = trans.Commit();
  if (NS_FAILED(rv)) { return rv; }

  return rv;
}

// static
nsresult
CacheDBSchema::Match(mozIStorageConnection* aConnection,
                     const PCacheRequest& aRequest,
                     const PCacheQueryParams& aParams,
                     PCacheResponseOrVoid* aResponseOrVoidOut)
{
  MOZ_ASSERT(aConnection);
  MOZ_ASSERT(aResponseOrVoidOut);

  nsTArray<EntryId> matches;
  nsresult rv = QueryCache(aConnection, aRequest, aParams, matches);
  if (NS_FAILED(rv)) { return rv; }

  if (matches.Length() < 1) {
    *aResponseOrVoidOut = void_t();
    return rv;
  }

  PCacheResponse response;
  rv = ReadResponse(aConnection, matches[0], response);
  if (NS_FAILED(rv)) { return rv; }

  *aResponseOrVoidOut = response;
  return rv;
}

// static
nsresult
CacheDBSchema::MatchAll(mozIStorageConnection* aConnection,
                        const PCacheRequestOrVoid& aRequestOrVoid,
                        const PCacheQueryParams& aParams,
                        nsTArray<PCacheResponse>& aResponsesOut)
{
  MOZ_ASSERT(aConnection);
  nsresult rv;

  nsTArray<EntryId> matches;
  if (aRequestOrVoid.type() == PCacheRequestOrVoid::Tvoid_t) {
    rv = QueryAll(aConnection, matches);
    if (NS_FAILED(rv)) { return rv; }
  } else {
    rv = QueryCache(aConnection, aRequestOrVoid, aParams, matches);
    if (NS_FAILED(rv)) { return rv; }
  }

  // TODO: replace this with a bulk load using SQL IN clause
  for (uint32_t i = 0; i < matches.Length(); ++i) {
    PCacheResponse *response = aResponsesOut.AppendElement();
    if (!response) {
      return NS_ERROR_OUT_OF_MEMORY;
    }

    rv = ReadResponse(aConnection, matches[i], *response);
    if (NS_FAILED(rv)) { return rv; }
  }

  return rv;
}

// static
nsresult
CacheDBSchema::Put(mozIStorageConnection* aConnection,
                   const PCacheRequest& aRequest,
                   const PCacheResponse& aResponse)
{
  MOZ_ASSERT(aConnection);

  mozStorageTransaction trans(aConnection, false,
                              mozIStorageConnection::TRANSACTION_IMMEDIATE);

  PCacheQueryParams params(false, false, false, false, false,
                           NS_LITERAL_STRING(""));
  nsTArray<EntryId> matches;
  nsresult rv = QueryCache(aConnection, aRequest, params, matches);
  if (NS_FAILED(rv)) { return rv; }

  rv = DeleteEntries(aConnection, matches);
  if (NS_FAILED(rv)) { return rv; }

  rv = InsertEntry(aConnection, aRequest, aResponse);
  if (NS_FAILED(rv)) { return rv; }

  rv = trans.Commit();
  if (NS_FAILED(rv)) { return rv; }

  return rv;
}

// static
nsresult
CacheDBSchema::Delete(mozIStorageConnection* aConnection,
                      const PCacheRequest& aRequest,
                      const PCacheQueryParams& aParams,
                      bool* aSuccessOut)
{
  MOZ_ASSERT(aConnection);
  MOZ_ASSERT(aSuccessOut);

  mozStorageTransaction trans(aConnection, false,
                              mozIStorageConnection::TRANSACTION_IMMEDIATE);

  nsTArray<EntryId> matches;
  nsresult rv = QueryCache(aConnection, aRequest, aParams, matches);
  if (NS_FAILED(rv)) { return rv; }

  if (matches.Length() < 1) {
    *aSuccessOut = false;
    return rv;
  }

  rv = DeleteEntries(aConnection, matches);
  if (NS_FAILED(rv)) { return rv; }

  rv = trans.Commit();
  if (NS_FAILED(rv)) { return rv; }

  *aSuccessOut = true;

  return rv;
}

// static
nsresult
CacheDBSchema::QueryAll(mozIStorageConnection* aConnection,
                        nsTArray<EntryId>& aEntryIdListOut)
{
  MOZ_ASSERT(aConnection);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT id FROM map"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  while(NS_SUCCEEDED(statement->ExecuteStep(&hasMoreData)) && hasMoreData) {
    EntryId* entryId = aEntryIdListOut.AppendElement();
    if (!entryId) { return NS_ERROR_OUT_OF_MEMORY; }
    rv = statement->GetInt32(0, entryId);
    if (NS_FAILED(rv)) { return rv; }
  }

  return rv;
}

// static
nsresult
CacheDBSchema::QueryCache(mozIStorageConnection* aConnection,
                          const PCacheRequest& aRequest,
                          const PCacheQueryParams& aParams,
                          nsTArray<EntryId>& aEntryIdListOut)
{
  MOZ_ASSERT(aConnection);

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
    "FROM map "
    "LEFT OUTER JOIN response_headers ON map.id=response_headers.map_id "
                                    "AND response_headers.name='vary' "
    "WHERE map."
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
    query.Append(NS_LITERAL_CSTRING(" LIKE ?1 ESCAPE '\\'"));
  } else {
    query.Append(NS_LITERAL_CSTRING("=?1"));
  }

  query.Append(NS_LITERAL_CSTRING(" GROUP BY map.id ORDER BY map.id"));

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConnection->CreateStatement(query, getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  if (aParams.prefixMatch()) {
    nsAutoString escapedUrlToMatch;
    rv = statement->EscapeStringForLIKE(urlToMatch, '\\', escapedUrlToMatch);
    if (NS_FAILED(rv)) { return rv; }
    urlToMatch = escapedUrlToMatch;
  }

  rv = statement->BindStringParameter(0, urlToMatch);
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  while(NS_SUCCEEDED(statement->ExecuteStep(&hasMoreData)) && hasMoreData) {
    EntryId entryId;
    rv = statement->GetInt32(0, &entryId);
    if (NS_FAILED(rv)) { return rv; }

    int32_t varyCount;
    rv = statement->GetInt32(1, &varyCount);
    if (NS_FAILED(rv)) { return rv; }

    if (!aParams.ignoreVary() && varyCount > 0) {
      bool matchedByVary;
      rv = MatchByVaryHeader(aConnection, aRequest, entryId, &matchedByVary);
      if (NS_FAILED(rv)) { return rv; }
      if (matchedByVary) {
        continue;
      }
    }

    aEntryIdListOut.AppendElement(entryId);
  }

  return NS_OK;
}

// static
nsresult
CacheDBSchema::MatchByVaryHeader(mozIStorageConnection* aConnection,
                                 const PCacheRequest& aRequest,
                                 int32_t entryId, bool* aSuccessOut)
{
  MOZ_ASSERT(aConnection);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT value FROM response_headers "
    "WHERE name='vary' AND map_id=?1"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, entryId);
  if (NS_FAILED(rv)) { return rv; }

  // TODO: do this async
  // TODO: use a transaction

  nsTArray<nsCString> varyValues;

  bool hasMoreData;
  while(NS_SUCCEEDED(statement->ExecuteStep(&hasMoreData)) && hasMoreData) {
    nsCString* value = varyValues.AppendElement();
    if (!value) { return NS_ERROR_OUT_OF_MEMORY; }
    rv = statement->GetUTF8String(0, *value);
    if (NS_FAILED(rv)) { return rv; }
  }
  if (NS_FAILED(rv)) { return rv; }

  // Should not have called this function if this was not the case
  MOZ_ASSERT(varyValues.Length() > 0);

  statement->Reset();
  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT name, value FROM request_headers "
    "WHERE AND map_id=?1"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, entryId);
  if (NS_FAILED(rv)) { return rv; }

  nsRefPtr<Headers> cachedHeaders = new Headers(nullptr);
  if (!cachedHeaders) { return NS_ERROR_OUT_OF_MEMORY; }

  ErrorResult errorResult;

  while(NS_SUCCEEDED(statement->ExecuteStep(&hasMoreData)) && hasMoreData) {
    nsAutoCString name;
    nsAutoCString value;
    rv = statement->GetUTF8String(0, name);
    if (NS_FAILED(rv)) { return rv; }
    rv = statement->GetUTF8String(1, value);
    if (NS_FAILED(rv)) { return rv; }

    cachedHeaders->Append(name, value, errorResult);
    if (errorResult.Failed()) { return errorResult.ErrorCode(); };
  }
  if (NS_FAILED(rv)) { return rv; }

  nsRefPtr<Headers> queryHeaders = new Headers(nullptr, aRequest.headers());
  if (!queryHeaders) { return NS_ERROR_OUT_OF_MEMORY; }

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
      return NS_OK;
    }
  }

  *aSuccessOut = true;
  return NS_OK;
}

// static
nsresult
CacheDBSchema::DeleteEntries(mozIStorageConnection* aConnection,
                             const nsTArray<EntryId>& aEntryIdList,
                             uint32_t aPos, int32_t aLen)
{
  MOZ_ASSERT(aConnection);

  if (aEntryIdList.Length() < 1) {
    return NS_OK;
  }

  MOZ_ASSERT(aPos < aEntryIdList.Length());

  if (aLen < 0) {
    aLen = aEntryIdList.Length() - aPos;
  }

  // Sqlite limits the number of entries allowed for an IN clause,
  // so split up larger operations.
  if (aLen > MAX_ENTRIES_PER_STATEMENT) {
    uint32_t curPos = aPos;
    int32_t remaining = aLen;
    while (remaining > 0) {
      int32_t curLen = std::min(MAX_ENTRIES_PER_STATEMENT, remaining);
      nsresult rv = DeleteEntries(aConnection, aEntryIdList, curPos, curLen);
      if (NS_FAILED(rv)) { return rv; }

      curPos += curLen;
      remaining -= curLen;
    }
    return NS_OK;
  }

  nsCOMPtr<mozIStorageStatement> statement;
  nsAutoCString query(
    "DELETE FROM response_headers WHERE map_id IN ("
  );
  AppendListParamsToQuery(query, aEntryIdList, aPos, aLen);
  query.Append(NS_LITERAL_CSTRING(")"));

  nsresult rv = aConnection->CreateStatement(query, getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = BindListParamsToQuery(statement, aEntryIdList, aPos, aLen);
  if (NS_FAILED(rv)) { return rv; }

  // Don't stop if this fails.  There may not be any entries in this table,
  // but we must continue to process the other tables.
  statement->Execute();

  query = NS_LITERAL_CSTRING(
    "DELETE FROM request_headers WHERE map_id IN ("
  );
  AppendListParamsToQuery(query, aEntryIdList, aPos, aLen);
  query.Append(NS_LITERAL_CSTRING(")"));

  rv = aConnection->CreateStatement(query, getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = BindListParamsToQuery(statement, aEntryIdList, aPos, aLen);
  if (NS_FAILED(rv)) { return rv; }

  // Don't stop if this fails.  There may not be any entries in this table,
  // but we must continue to process the other tables.
  statement->Execute();

  query = NS_LITERAL_CSTRING(
    "DELETE FROM map WHERE id IN ("
  );
  AppendListParamsToQuery(query, aEntryIdList, aPos, aLen);
  query.Append(NS_LITERAL_CSTRING(")"));

  rv = aConnection->CreateStatement(query, getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = BindListParamsToQuery(statement, aEntryIdList, aPos, aLen);
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->Execute();
  if (NS_FAILED(rv)) { return rv; }

  return NS_OK;
}

// static
nsresult
CacheDBSchema::InsertEntry(mozIStorageConnection* aConnection,
                           const PCacheRequest& aRequest,
                           const PCacheResponse& aResponse)
{
  MOZ_ASSERT(aConnection);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO map ("
      "request_method, "
      "request_url, "
      "request_url_no_query, "
      "request_mode, "
      "request_credentials, "
      "response_type, "
      "response_status, "
      "response_status_text "
    ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindUTF8StringParameter(0, aRequest.method());
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindStringParameter(1, aRequest.url());
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindStringParameter(2, aRequest.urlWithoutQuery());
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(3, static_cast<int32_t>(aRequest.mode()));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(4,
    static_cast<int32_t>(aRequest.credentials()));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(5, static_cast<int32_t>(aResponse.type()));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(6, aResponse.status());
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindUTF8StringParameter(7, aResponse.statusText());
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->Execute();
  if (NS_FAILED(rv)) { return rv; }

  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT last_insert_rowid()"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  rv = statement->ExecuteStep(&hasMoreData);
  if (NS_FAILED(rv)) { return rv; }

  int32_t mapId;
  rv = statement->GetInt32(0, &mapId);
  if (NS_FAILED(rv)) { return rv; }

  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO request_headers ("
      "name, "
      "value, "
      "map_id "
    ") VALUES (?1, ?2, ?3)"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  const nsTArray<PHeadersEntry>& requestHeaders = aRequest.headers().list();
  for (uint32_t i = 0; i < requestHeaders.Length(); ++i) {
    rv = statement->BindUTF8StringParameter(0, requestHeaders[i].name());
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->BindUTF8StringParameter(1, requestHeaders[i].value());
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->BindInt32Parameter(2, mapId);
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->Execute();
    if (NS_FAILED(rv)) { return rv; }
  }

  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO response_headers ("
      "name, "
      "value, "
      "map_id "
    ") VALUES (?1, ?2, ?3)"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  const nsTArray<PHeadersEntry>& responseHeaders = aResponse.headers().list();
  for (uint32_t i = 0; i < responseHeaders.Length(); ++i) {
    rv = statement->BindUTF8StringParameter(0, responseHeaders[i].name());
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->BindUTF8StringParameter(1, responseHeaders[i].value());
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->BindInt32Parameter(2, mapId);
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->Execute();
    if (NS_FAILED(rv)) { return rv; }
  }

  return NS_OK;
}

// static
nsresult
CacheDBSchema::ReadResponse(mozIStorageConnection* aConnection,
                            EntryId aEntryId, PCacheResponse& aResponseOut)
{
  MOZ_ASSERT(aConnection);
  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT "
      "response_type, "
      "response_status, "
      "response_status_text "
    "FROM map "
    "WHERE id=?1 "
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aEntryId);
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  rv = statement->ExecuteStep(&hasMoreData);
  if (NS_FAILED(rv)) { return rv; }

  int32_t type;
  rv = statement->GetInt32(0, &type);
  if (NS_FAILED(rv)) { return rv; }
  aResponseOut.type() = static_cast<ResponseType>(type);

  int32_t status;
  rv = statement->GetInt32(1, &status);
  if (NS_FAILED(rv)) { return rv; }
  aResponseOut.status() = status;

  rv = statement->GetUTF8String(2, aResponseOut.statusText());
  if (NS_FAILED(rv)) { return rv; }

  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT "
      "name, "
      "value "
    "FROM response_headers "
    "WHERE map_id=?1 "
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aEntryId);
  if (NS_FAILED(rv)) { return rv; }

  while(NS_SUCCEEDED(statement->ExecuteStep(&hasMoreData)) && hasMoreData) {
    PHeadersEntry* header = aResponseOut.headers().list().AppendElement();
    if (!header) { return NS_ERROR_OUT_OF_MEMORY; }

    rv = statement->GetUTF8String(0, header->name());
    if (NS_FAILED(rv)) { return rv; }

    rv = statement->GetUTF8String(1, header->value());
    if (NS_FAILED(rv)) { return rv; }
  }

  return NS_OK;
}


} // namespace dom
} // namespace mozilla
