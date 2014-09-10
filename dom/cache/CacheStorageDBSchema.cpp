/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/CacheStorageDBSchema.h"
#include "mozStorageHelper.h"

namespace mozilla {
namespace dom {

using mozilla::dom::cache::Namespace;

// static
nsresult
CacheStorageDBSchema::Create(mozIStorageConnection* aConn)
{
  MOZ_ASSERT(aConn);
  nsresult rv;

#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
  rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    // Switch the journaling mode to TRUNCATE to avoid changing the directory
    // structure at the conclusion of every transaction for devices with slower
    // file systems.
    "PRAGMA journal_mode = TRUNCATE; "
  ));
  if (NS_FAILED(rv)) { return rv; }
#endif

  int32_t schemaVersion;
  rv = aConn->GetSchemaVersion(&schemaVersion);
  if (NS_FAILED(rv)) { return rv; }

  mozStorageTransaction trans(aConn, false,
                              mozIStorageConnection::TRANSACTION_IMMEDIATE);

  if (!schemaVersion) {
    rv = aConn->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
      "CREATE TABLE caches ("
        "namespace INTEGER NOT NULL, "
        "key TEXT NOT NULL, "
        "cache_uuid TEXT NOT NULL, "
        "PRIMARY KEY(namespace, key)"
      ");"
    ));
    if (NS_FAILED(rv)) { return rv; }

    rv = aConn->SetSchemaVersion(kLatestSchemaVersion);
    if (NS_FAILED(rv)) { return rv; }

    rv = aConn->GetSchemaVersion(&schemaVersion);
    if (NS_FAILED(rv)) { return rv; }
  }

  if (schemaVersion != kLatestSchemaVersion) {
    rv = NS_ERROR_FAILURE;
    return rv;
  }

  rv = trans.Commit();
  if (NS_FAILED(rv)) { return rv; }

  return rv;
}

// static
nsresult
CacheStorageDBSchema::Get(mozIStorageConnection* aConn, Namespace aNamespace,
                          const nsAString& aKey, bool* aSuccessOut,
                          nsID* aCacheIdOut)
{
  MOZ_ASSERT(aConn);
  MOZ_ASSERT(aSuccessOut);
  MOZ_ASSERT(aCacheIdOut);

  *aSuccessOut = false;

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT cache_uuid FROM caches WHERE namespace=?1 AND key=?2"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aNamespace);
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindStringParameter(1, aKey);
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  rv = statement->ExecuteStep(&hasMoreData);
  if (NS_FAILED(rv)) { return rv; }

  if (!hasMoreData) {
    return NS_OK;
  }

  nsAutoCString uuidString;
  rv = statement->GetUTF8String(0, uuidString);
  if (NS_FAILED(rv)) { return rv; }

  if (!aCacheIdOut->Parse(uuidString.get())) {
    return NS_ERROR_FAILURE;
  }

  *aSuccessOut = true;

  return rv;
}

// static
nsresult
CacheStorageDBSchema::Has(mozIStorageConnection* aConn, Namespace aNamespace,
                          const nsAString& aKey, bool* aSuccessOut)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT count(*) FROM caches WHERE namespace=?1 AND key=?2"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aNamespace);
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindStringParameter(1, aKey);
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  rv = statement->ExecuteStep(&hasMoreData);
  if (NS_FAILED(rv)) { return rv; }

  int32_t count;
  rv = statement->GetInt32(0, &count);
  if (NS_FAILED(rv)) { return rv; }

  *aSuccessOut = count > 0;

  return rv;
}

// static
nsresult
CacheStorageDBSchema::Put(mozIStorageConnection* aConn, Namespace aNamespace,
                          const nsAString& aKey, const nsID& aCacheId,
                          bool* aSuccessOut)
{
  MOZ_ASSERT(aConn);

  mozStorageTransaction trans(aConn, false,
                              mozIStorageConnection::TRANSACTION_IMMEDIATE);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO caches (namespace, key, cache_uuid)VALUES(?1, ?2, ?3)"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aNamespace);
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindStringParameter(1, aKey);
  if (NS_FAILED(rv)) { return rv; }

  char uuidBuf[NSID_LENGTH];
  aCacheId.ToProvidedString(uuidBuf);

  rv = statement->BindUTF8StringParameter(2, nsAutoCString(uuidBuf));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->Execute();
  if (NS_FAILED(rv)) { return rv; }

  rv = trans.Commit();
  *aSuccessOut = NS_SUCCEEDED(rv);

  return rv;
}

// static
nsresult
CacheStorageDBSchema::Delete(mozIStorageConnection* aConn, Namespace aNamespace,
                             const nsAString& aKey, bool* aSuccessOut)
{
  MOZ_ASSERT(aConn);

  mozStorageTransaction trans(aConn, false,
                              mozIStorageConnection::TRANSACTION_IMMEDIATE);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "DELETE FROM caches WHERE namespace=?1 AND key=?2"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aNamespace);
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindStringParameter(1, aKey);
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->Execute();
  if (NS_FAILED(rv)) { return rv; }

  rv = trans.Commit();
  *aSuccessOut = NS_SUCCEEDED(rv);

  return rv;
}

// static
nsresult
CacheStorageDBSchema::Keys(mozIStorageConnection* aConn, Namespace aNamespace,
                           nsTArray<nsString>& aKeysOut)
{
  MOZ_ASSERT(aConn);

  nsCOMPtr<mozIStorageStatement> statement;
  nsresult rv = aConn->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT key FROM caches WHERE namespace=?1 ORDER BY rowid"
  ), getter_AddRefs(statement));
  if (NS_FAILED(rv)) { return rv; }

  rv = statement->BindInt32Parameter(0, aNamespace);
  if (NS_FAILED(rv)) { return rv; }

  bool hasMoreData;
  while(NS_SUCCEEDED(statement->ExecuteStep(&hasMoreData)) && hasMoreData) {
    nsString* key = aKeysOut.AppendElement();
    if (!key) {
      rv = NS_ERROR_OUT_OF_MEMORY;
      return rv;
    }
    rv = statement->GetString(0, *key);
    if (NS_FAILED(rv)) { return rv; }
  }

  return rv;
}

} // namespace dom
} // namespace mozilla
