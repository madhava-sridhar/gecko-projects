/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ActorsParent.h"

#include <algorithm>
#include "DatabaseInfo.h"
#include "js/StructuredClone.h"
#include "KeyPath.h"
#include "mozilla/Atomics.h"
#include "mozilla/LazyIdleThread.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/storage.h"
#include "mozilla/unused.h"
#include "mozilla/dom/quota/AcquireListener.h"
#include "mozilla/dom/quota/OriginOrPatternString.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozIStorageConnection.h"
#include "mozIStorageProgressHandler.h"
#include "nsCOMPtr.h"
#include "nsEscape.h"
#include "nsIEventTarget.h"
#include "nsIFile.h"
#include "nsIFileURL.h"
#include "nsNetUtil.h"
#include "nsThreadUtils.h"
#include "nsTraceRefcnt.h"
#include "ProfilerHelpers.h"
#include "snappy/snappy.h"

using namespace mozilla;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

namespace {

// If JS_STRUCTURED_CLONE_VERSION changes then we need to update our major
// schema version.
static_assert(JS_STRUCTURED_CLONE_VERSION == 2,
              "Need to update the major schema version.");

// Major schema version. Bump for almost everything.
const uint32_t kMajorSchemaVersion = 14;

// Minor schema version. Should almost always be 0 (maybe bump on release
// branches if we have to).
const uint32_t kMinorSchemaVersion = 0;

// The schema version we store in the SQLite database is a (signed) 32-bit
// integer. The major version is left-shifted 4 bits so the max value is
// 0xFFFFFFF. The minor version occupies the lower 4 bits and its max is 0xF.
static_assert(kMajorSchemaVersion <= 0xFFFFFFF,
              "Major version needs to fit in 28 bits.");
static_assert(kMinorSchemaVersion <= 0xF,
              "Minor version needs to fit in 4 bits.");

int32_t
MakeSchemaVersion(uint32_t aMajorSchemaVersion,
                  uint32_t aMinorSchemaVersion)
{
  return int32_t((aMajorSchemaVersion << 4) + aMinorSchemaVersion);
}

const int32_t kSQLiteSchemaVersion =
  int32_t((kMajorSchemaVersion << 4) + kMinorSchemaVersion);

uint32_t
HashName(const nsAString& aName)
{
  struct Helper
  {
    static uint32_t
    RotateBitsLeft32(uint32_t aValue, uint8_t aBits)
    {
      MOZ_ASSERT(aBits < 32);
      return (aValue << aBits) | (aValue >> (32 - aBits));
    }
  };

  static const uint32_t kGoldenRatioU32 = 0x9e3779b9u;

  const char16_t* str = aName.BeginReading();
  size_t length = aName.Length();

  uint32_t hash = 0;
  for (size_t i = 0; i < length; i++) {
    hash = kGoldenRatioU32 * (Helper::RotateBitsLeft32(hash, 5) ^ str[i]);
  }

  return hash;
}

nsresult
ClampErrorCode(nsresult aErrorCode)
{
  MOZ_ASSERT(NS_FAILED(aErrorCode));

  if (NS_ERROR_GET_MODULE(aErrorCode) != NS_ERROR_MODULE_DOM_INDEXEDDB) {
#ifdef DEBUG
    nsAutoCString message("Converting non-IndexedDB error code (0x");
    message.AppendPrintf("%X) to NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR",
                         aErrorCode);
    NS_WARNING(message.get());
#endif
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  return aErrorCode;
}

void
GetDatabaseFilename(const nsAString& aName,
                    nsAString& aDatabaseFilename)
{
  aDatabaseFilename.AppendInt(HashName(aName));

  nsCString escapedName;
  if (!NS_Escape(NS_ConvertUTF16toUTF8(aName), escapedName, url_XPAlphas)) {
    MOZ_CRASH("Can't escape database name!");
  }

  const char* forwardIter = escapedName.BeginReading();
  const char* backwardIter = escapedName.EndReading() - 1;

  nsCString substring;
  while (forwardIter <= backwardIter && substring.Length() < 21) {
    if (substring.Length() % 2) {
      substring.Append(*backwardIter--);
    } else {
      substring.Append(*forwardIter++);
    }
  }

  aDatabaseFilename.Append(NS_ConvertASCIItoUTF16(substring));
}

nsresult
CreateFileTables(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "CreateFileTables");

  // Table `file`
  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE file ("
      "id INTEGER PRIMARY KEY, "
      "refcount INTEGER NOT NULL"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_insert_trigger "
    "AFTER INSERT ON object_data "
    "FOR EACH ROW "
    "WHEN NEW.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(NULL, NEW.file_ids); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_update_trigger "
    "AFTER UPDATE OF file_ids ON object_data "
    "FOR EACH ROW "
    "WHEN OLD.file_ids IS NOT NULL OR NEW.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(OLD.file_ids, NEW.file_ids); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_delete_trigger "
    "AFTER DELETE ON object_data "
    "FOR EACH ROW WHEN OLD.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(OLD.file_ids, NULL); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER file_update_trigger "
    "AFTER UPDATE ON file "
    "FOR EACH ROW WHEN NEW.refcount = 0 "
    "BEGIN "
      "DELETE FROM file WHERE id = OLD.id; "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
CreateTables(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "CreateTables");

  // Table `database`
  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE database ("
      "name TEXT NOT NULL, "
      "version INTEGER NOT NULL DEFAULT 0"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Table `object_store`
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_store ("
      "id INTEGER PRIMARY KEY, "
      "auto_increment INTEGER NOT NULL DEFAULT 0, "
      "name TEXT NOT NULL, "
      "key_path TEXT, "
      "UNIQUE (name)"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Table `object_data`
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_data ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id INTEGER NOT NULL, "
      "key_value BLOB DEFAULT NULL, "
      "file_ids TEXT, "
      "data BLOB NOT NULL, "
      "UNIQUE (object_store_id, key_value), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Table `index`
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_store_index ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id INTEGER NOT NULL, "
      "name TEXT NOT NULL, "
      "key_path TEXT NOT NULL, "
      "unique_index INTEGER NOT NULL, "
      "multientry INTEGER NOT NULL, "
      "UNIQUE (object_store_id, name), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Table `index_data`
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE index_data ("
      "index_id INTEGER NOT NULL, "
      "value BLOB NOT NULL, "
      "object_data_key BLOB NOT NULL, "
      "object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, object_data_key), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE, "
      "FOREIGN KEY (object_data_id) REFERENCES object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Need this to make cascading deletes from object_data and object_store fast.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX index_data_object_data_id_index "
    "ON index_data (object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Table `unique_index_data`
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE unique_index_data ("
      "index_id INTEGER NOT NULL, "
      "value BLOB NOT NULL, "
      "object_data_key BLOB NOT NULL, "
      "object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, object_data_key), "
      "UNIQUE (index_id, value), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE "
      "FOREIGN KEY (object_data_id) REFERENCES object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Need this to make cascading deletes from object_data and object_store fast.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX unique_index_data_object_data_id_index "
    "ON unique_index_data (object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = CreateFileTables(aConnection);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(kSQLiteSchemaVersion);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom4To5(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom4To5");

  nsresult rv;

  // All we changed is the type of the version column, so lets try to
  // convert that to an integer, and if we fail, set it to 0.
  nsCOMPtr<mozIStorageStatement> stmt;
  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT name, version, dataVersion "
    "FROM database"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsString name;
  int32_t intVersion;
  int64_t dataVersion;

  {
    mozStorageStatementScoper scoper(stmt);

    bool hasResults;
    rv = stmt->ExecuteStep(&hasResults);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    if (NS_WARN_IF(!hasResults)) {
      return NS_ERROR_FAILURE;
    }

    nsString version;
    rv = stmt->GetString(1, version);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    intVersion = version.ToInteger(&rv);
    if (NS_FAILED(rv)) {
      intVersion = 0;
    }

    rv = stmt->GetString(0, name);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->GetInt64(2, &dataVersion);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE database"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE database ("
      "name TEXT NOT NULL, "
      "version INTEGER NOT NULL DEFAULT 0, "
      "dataVersion INTEGER NOT NULL"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "INSERT INTO database (name, version, dataVersion) "
    "VALUES (:name, :version, :dataVersion)"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  {
    mozStorageStatementScoper scoper(stmt);

    rv = stmt->BindStringParameter(0, name);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->BindInt32Parameter(1, intVersion);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->BindInt64Parameter(2, dataVersion);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->Execute();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  rv = aConnection->SetSchemaVersion(5);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom5To6(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom5To6");

  // First, drop all the indexes we're no longer going to use.
  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP INDEX key_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP INDEX ai_key_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP INDEX value_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP INDEX ai_value_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Now, reorder the columns of object_data to put the blob data last. We do
  // this by copying into a temporary table, dropping the original, then copying
  // back into a newly created table.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id, "
      "key_value, "
      "data "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT id, object_store_id, key_value, data "
      "FROM object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_data ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id INTEGER NOT NULL, "
      "key_value DEFAULT NULL, "
      "data BLOB NOT NULL, "
      "UNIQUE (object_store_id, key_value), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO object_data "
      "SELECT id, object_store_id, key_value, data "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // We need to add a unique constraint to our ai_object_data table. Copy all
  // the data out of it using a temporary table as before.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id, "
      "data "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT id, object_store_id, data "
      "FROM ai_object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE ai_object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE ai_object_data ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "object_store_id INTEGER NOT NULL, "
      "data BLOB NOT NULL, "
      "UNIQUE (object_store_id, id), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO ai_object_data "
      "SELECT id, object_store_id, data "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Fix up the index_data table. We're reordering the columns as well as
  // changing the primary key from being a simple id to being a composite.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "index_id, "
      "value, "
      "object_data_key, "
      "object_data_id "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT index_id, value, object_data_key, object_data_id "
      "FROM index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE index_data ("
      "index_id INTEGER NOT NULL, "
      "value NOT NULL, "
      "object_data_key NOT NULL, "
      "object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, object_data_key), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE, "
      "FOREIGN KEY (object_data_id) REFERENCES object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT OR IGNORE INTO index_data "
      "SELECT index_id, value, object_data_key, object_data_id "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX index_data_object_data_id_index "
    "ON index_data (object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Fix up the unique_index_data table. We're reordering the columns as well as
  // changing the primary key from being a simple id to being a composite.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "index_id, "
      "value, "
      "object_data_key, "
      "object_data_id "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT index_id, value, object_data_key, object_data_id "
      "FROM unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE unique_index_data ("
      "index_id INTEGER NOT NULL, "
      "value NOT NULL, "
      "object_data_key NOT NULL, "
      "object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, object_data_key), "
      "UNIQUE (index_id, value), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE "
      "FOREIGN KEY (object_data_id) REFERENCES object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO unique_index_data "
      "SELECT index_id, value, object_data_key, object_data_id "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX unique_index_data_object_data_id_index "
    "ON unique_index_data (object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Fix up the ai_index_data table. We're reordering the columns as well as
  // changing the primary key from being a simple id to being a composite.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "index_id, "
      "value, "
      "ai_object_data_id "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT index_id, value, ai_object_data_id "
      "FROM ai_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE ai_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE ai_index_data ("
      "index_id INTEGER NOT NULL, "
      "value NOT NULL, "
      "ai_object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, ai_object_data_id), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE, "
      "FOREIGN KEY (ai_object_data_id) REFERENCES ai_object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT OR IGNORE INTO ai_index_data "
      "SELECT index_id, value, ai_object_data_id "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX ai_index_data_ai_object_data_id_index "
    "ON ai_index_data (ai_object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Fix up the ai_unique_index_data table. We're reordering the columns as well
  // as changing the primary key from being a simple id to being a composite.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "index_id, "
      "value, "
      "ai_object_data_id "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT index_id, value, ai_object_data_id "
      "FROM ai_unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE ai_unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE ai_unique_index_data ("
      "index_id INTEGER NOT NULL, "
      "value NOT NULL, "
      "ai_object_data_id INTEGER NOT NULL, "
      "UNIQUE (index_id, value), "
      "PRIMARY KEY (index_id, value, ai_object_data_id), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE, "
      "FOREIGN KEY (ai_object_data_id) REFERENCES ai_object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO ai_unique_index_data "
      "SELECT index_id, value, ai_object_data_id "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX ai_unique_index_data_ai_object_data_id_index "
    "ON ai_unique_index_data (ai_object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(6);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom6To7(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom6To7");

  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "id, "
      "name, "
      "key_path, "
      "auto_increment"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT id, name, key_path, auto_increment "
      "FROM object_store;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE object_store;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_store ("
      "id INTEGER PRIMARY KEY, "
      "auto_increment INTEGER NOT NULL DEFAULT 0, "
      "name TEXT NOT NULL, "
      "key_path TEXT, "
      "UNIQUE (name)"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO object_store "
      "SELECT id, auto_increment, name, nullif(key_path, '') "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(7);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom7To8(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom7To8");

  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "id, "
      "object_store_id, "
      "name, "
      "key_path, "
      "unique_index, "
      "object_store_autoincrement"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT id, object_store_id, name, key_path, "
      "unique_index, object_store_autoincrement "
      "FROM object_store_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE object_store_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_store_index ("
      "id INTEGER, "
      "object_store_id INTEGER NOT NULL, "
      "name TEXT NOT NULL, "
      "key_path TEXT NOT NULL, "
      "unique_index INTEGER NOT NULL, "
      "multientry INTEGER NOT NULL, "
      "object_store_autoincrement INTERGER NOT NULL, "
      "PRIMARY KEY (id), "
      "UNIQUE (object_store_id, name), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO object_store_index "
      "SELECT id, object_store_id, name, key_path, "
      "unique_index, 0, object_store_autoincrement "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(8);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

class CompressDataBlobsFunction MOZ_FINAL
  : public mozIStorageFunction
{
public:
  NS_DECL_ISUPPORTS

private:
  NS_IMETHOD
  OnFunctionCall(mozIStorageValueArray* aArguments,
                 nsIVariant** aResult) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aArguments);
    MOZ_ASSERT(aResult);

    PROFILER_LABEL("IndexedDB", "CompressDataBlobsFunction::OnFunctionCall");

    uint32_t argc;
    nsresult rv = aArguments->GetNumEntries(&argc);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (argc != 1) {
      NS_WARNING("Don't call me with the wrong number of arguments!");
      return NS_ERROR_UNEXPECTED;
    }

    int32_t type;
    rv = aArguments->GetTypeOfIndex(0, &type);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (type != mozIStorageStatement::VALUE_TYPE_BLOB) {
      NS_WARNING("Don't call me with the wrong type of arguments!");
      return NS_ERROR_UNEXPECTED;
    }

    const uint8_t* uncompressed;
    uint32_t uncompressedLength;
    rv = aArguments->GetSharedBlob(0, &uncompressedLength, &uncompressed);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    static const fallible_t fallible = fallible_t();
    size_t compressedLength = snappy::MaxCompressedLength(uncompressedLength);
    nsAutoArrayPtr<char> compressed(new (fallible) char[compressedLength]);
    if (NS_WARN_IF(!compressed)) {
      return NS_ERROR_OUT_OF_MEMORY;
    }

    snappy::RawCompress(reinterpret_cast<const char*>(uncompressed),
                        uncompressedLength, compressed.get(),
                        &compressedLength);

    std::pair<const void *, int> data(static_cast<void*>(compressed.get()),
                                      int(compressedLength));

    // XXX This copies the buffer again... There doesn't appear to be any way to
    //     preallocate space and write directly to a BlobVariant at the moment.
    nsCOMPtr<nsIVariant> result = new mozilla::storage::BlobVariant(data);

    result.forget(aResult);
    return NS_OK;
  }
};

nsresult
UpgradeSchemaFrom8To9_0(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom8To9_0");

  // We no longer use the dataVersion column.
  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "UPDATE database SET dataVersion = 0;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<mozIStorageFunction> compressor = new CompressDataBlobsFunction();

  NS_NAMED_LITERAL_CSTRING(compressorName, "compress");

  rv = aConnection->CreateFunction(compressorName, 1, compressor);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Turn off foreign key constraints before we do anything here.
  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "UPDATE object_data SET data = compress(data);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "UPDATE ai_object_data SET data = compress(data);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->RemoveFunction(compressorName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(MakeSchemaVersion(9, 0));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom9_0To10_0(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom9_0To10_0");

  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "ALTER TABLE object_data ADD COLUMN file_ids TEXT;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "ALTER TABLE ai_object_data ADD COLUMN file_ids TEXT;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = CreateFileTables(aConnection);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(MakeSchemaVersion(10, 0));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom10_0To11_0(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom10_0To11_0");

  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "id, "
      "object_store_id, "
      "name, "
      "key_path, "
      "unique_index, "
      "multientry"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT id, object_store_id, name, key_path, "
      "unique_index, multientry "
      "FROM object_store_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE object_store_index;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_store_index ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id INTEGER NOT NULL, "
      "name TEXT NOT NULL, "
      "key_path TEXT NOT NULL, "
      "unique_index INTEGER NOT NULL, "
      "multientry INTEGER NOT NULL, "
      "UNIQUE (object_store_id, name), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO object_store_index "
      "SELECT id, object_store_id, name, key_path, "
      "unique_index, multientry "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TRIGGER object_data_insert_trigger;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO object_data (object_store_id, key_value, data, file_ids) "
      "SELECT object_store_id, id, data, file_ids "
      "FROM ai_object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_insert_trigger "
    "AFTER INSERT ON object_data "
    "FOR EACH ROW "
    "WHEN NEW.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(NULL, NEW.file_ids); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO index_data (index_id, value, object_data_key, object_data_id) "
      "SELECT ai_index_data.index_id, ai_index_data.value, ai_index_data.ai_object_data_id, object_data.id "
      "FROM ai_index_data "
      "INNER JOIN object_store_index ON "
        "object_store_index.id = ai_index_data.index_id "
      "INNER JOIN object_data ON "
        "object_data.object_store_id = object_store_index.object_store_id AND "
        "object_data.key_value = ai_index_data.ai_object_data_id;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO unique_index_data (index_id, value, object_data_key, object_data_id) "
      "SELECT ai_unique_index_data.index_id, ai_unique_index_data.value, ai_unique_index_data.ai_object_data_id, object_data.id "
      "FROM ai_unique_index_data "
      "INNER JOIN object_store_index ON "
        "object_store_index.id = ai_unique_index_data.index_id "
      "INNER JOIN object_data ON "
        "object_data.object_store_id = object_store_index.object_store_id AND "
        "object_data.key_value = ai_unique_index_data.ai_object_data_id;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "UPDATE object_store "
      "SET auto_increment = (SELECT max(id) FROM ai_object_data) + 1 "
      "WHERE auto_increment;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE ai_unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE ai_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE ai_object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(MakeSchemaVersion(11, 0));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

class EncodeKeysFunction MOZ_FINAL
  : public mozIStorageFunction
{
public:
  NS_DECL_ISUPPORTS

private:
  NS_IMETHOD
  OnFunctionCall(mozIStorageValueArray* aArguments,
                 nsIVariant** aResult) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aArguments);
    MOZ_ASSERT(aResult);

    PROFILER_LABEL("IndexedDB", "EncodeKeysFunction::OnFunctionCall");

    uint32_t argc;
    nsresult rv = aArguments->GetNumEntries(&argc);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (argc != 1) {
      NS_WARNING("Don't call me with the wrong number of arguments!");
      return NS_ERROR_UNEXPECTED;
    }

    int32_t type;
    rv = aArguments->GetTypeOfIndex(0, &type);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    Key key;
    if (type == mozIStorageStatement::VALUE_TYPE_INTEGER) {
      int64_t intKey;
      aArguments->GetInt64(0, &intKey);
      key.SetFromInteger(intKey);
    } else if (type == mozIStorageStatement::VALUE_TYPE_TEXT) {
      nsString stringKey;
      aArguments->GetString(0, stringKey);
      key.SetFromString(stringKey);
    } else {
      NS_WARNING("Don't call me with the wrong type of arguments!");
      return NS_ERROR_UNEXPECTED;
    }

    const nsCString& buffer = key.GetBuffer();

    std::pair<const void *, int> data(static_cast<const void*>(buffer.get()),
                                      int(buffer.Length()));

    nsCOMPtr<nsIVariant> result = new mozilla::storage::BlobVariant(data);

    result.forget(aResult);
    return NS_OK;
  }
};

nsresult
UpgradeSchemaFrom11_0To12_0(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom11_0To12_0");

  NS_NAMED_LITERAL_CSTRING(encoderName, "encode");

  nsCOMPtr<mozIStorageFunction> encoder = new EncodeKeysFunction();

  nsresult rv = aConnection->CreateFunction(encoderName, 1, encoder);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id, "
      "key_value, "
      "data, "
      "file_ids "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT id, object_store_id, encode(key_value), data, file_ids "
      "FROM object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE object_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE object_data ("
      "id INTEGER PRIMARY KEY, "
      "object_store_id INTEGER NOT NULL, "
      "key_value BLOB DEFAULT NULL, "
      "file_ids TEXT, "
      "data BLOB NOT NULL, "
      "UNIQUE (object_store_id, key_value), "
      "FOREIGN KEY (object_store_id) REFERENCES object_store(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO object_data "
      "SELECT id, object_store_id, key_value, file_ids, data "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_insert_trigger "
    "AFTER INSERT ON object_data "
    "FOR EACH ROW "
    "WHEN NEW.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(NULL, NEW.file_ids); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_update_trigger "
    "AFTER UPDATE OF file_ids ON object_data "
    "FOR EACH ROW "
    "WHEN OLD.file_ids IS NOT NULL OR NEW.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(OLD.file_ids, NEW.file_ids); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TRIGGER object_data_delete_trigger "
    "AFTER DELETE ON object_data "
    "FOR EACH ROW WHEN OLD.file_ids IS NOT NULL "
    "BEGIN "
      "SELECT update_refcount(OLD.file_ids, NULL); "
    "END;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "index_id, "
      "value, "
      "object_data_key, "
      "object_data_id "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT index_id, encode(value), encode(object_data_key), object_data_id "
      "FROM index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE index_data ("
      "index_id INTEGER NOT NULL, "
      "value BLOB NOT NULL, "
      "object_data_key BLOB NOT NULL, "
      "object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, object_data_key), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE, "
      "FOREIGN KEY (object_data_id) REFERENCES object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO index_data "
      "SELECT index_id, value, object_data_key, object_data_id "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX index_data_object_data_id_index "
    "ON index_data (object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TEMPORARY TABLE temp_upgrade ("
      "index_id, "
      "value, "
      "object_data_key, "
      "object_data_id "
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO temp_upgrade "
      "SELECT index_id, encode(value), encode(object_data_key), object_data_id "
      "FROM unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE unique_index_data;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE TABLE unique_index_data ("
      "index_id INTEGER NOT NULL, "
      "value BLOB NOT NULL, "
      "object_data_key BLOB NOT NULL, "
      "object_data_id INTEGER NOT NULL, "
      "PRIMARY KEY (index_id, value, object_data_key), "
      "UNIQUE (index_id, value), "
      "FOREIGN KEY (index_id) REFERENCES object_store_index(id) ON DELETE "
        "CASCADE "
      "FOREIGN KEY (object_data_id) REFERENCES object_data(id) ON DELETE "
        "CASCADE"
    ");"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "INSERT INTO unique_index_data "
      "SELECT index_id, value, object_data_key, object_data_id "
      "FROM temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "DROP TABLE temp_upgrade;"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
    "CREATE INDEX unique_index_data_object_data_id_index "
    "ON unique_index_data (object_data_id);"
  ));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->RemoveFunction(encoderName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = aConnection->SetSchemaVersion(MakeSchemaVersion(12, 0));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom12_0To13_0(mozIStorageConnection* aConnection,
                            bool* aVacuumNeeded)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "UpgradeSchemaFrom12_0To13_0");

  nsresult rv;

#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
  int32_t defaultPageSize;
  rv = aConnection->GetDefaultPageSize(&defaultPageSize);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Enable auto_vacuum mode and update the page size to the platform default.
  nsAutoCString upgradeQuery("PRAGMA auto_vacuum = FULL; PRAGMA page_size = ");
  upgradeQuery.AppendInt(defaultPageSize);

  rv = aConnection->ExecuteSimpleSQL(upgradeQuery);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  *aVacuumNeeded = true;
#endif

  rv = aConnection->SetSchemaVersion(MakeSchemaVersion(13, 0));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
UpgradeSchemaFrom13_0To14_0(mozIStorageConnection* aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);

  // The only change between 13 and 14 was a different structured
  // clone format, but it's backwards-compatible.
  nsresult rv = aConnection->SetSchemaVersion(MakeSchemaVersion(14, 0));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

already_AddRefed<nsIFileURL>
GetDatabaseFileURL(nsIFile* aDatabaseFile,
                   PersistenceType aPersistenceType,
                   const nsACString& aGroup,
                   const nsACString& aOrigin)
{
  MOZ_ASSERT(aDatabaseFile);

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewFileURI(getter_AddRefs(uri), aDatabaseFile);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  nsCOMPtr<nsIFileURL> fileUrl = do_QueryInterface(uri);
  MOZ_ASSERT(fileUrl);

  nsAutoCString type;
  PersistenceTypeToText(aPersistenceType, type);

  rv = fileUrl->SetQuery(NS_LITERAL_CSTRING("persistenceType=") + type +
                         NS_LITERAL_CSTRING("&group=") + aGroup +
                         NS_LITERAL_CSTRING("&origin=") + aOrigin);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nullptr;
  }

  return fileUrl.forget();
}

nsresult
SetDefaultPragmas(mozIStorageConnection* aConnection)
{
  MOZ_ASSERT(aConnection);

  static const char query[] =
#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
    // Switch the journaling mode to TRUNCATE to avoid changing the directory
    // structure at the conclusion of every transaction for devices with slower
    // file systems.
    "PRAGMA journal_mode = TRUNCATE; "
#endif
    // We use foreign keys in lots of places.
    "PRAGMA foreign_keys = ON; "
    // The "INSERT OR REPLACE" statement doesn't fire the update trigger,
    // instead it fires only the insert trigger. This confuses the update
    // refcount function. This behavior changes with enabled recursive triggers,
    // so the statement fires the delete trigger first and then the insert
    // trigger.
    "PRAGMA recursive_triggers = ON;";

  nsresult rv = aConnection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(query));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
CreateDatabaseConnection(nsIFile* aDBFile,
                         nsIFile* aFMDirectory,
                         const nsAString& aName,
                         PersistenceType aPersistenceType,
                         const nsACString& aGroup,
                         const nsACString& aOrigin,
                         mozIStorageConnection** aConnection)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aDBFile);
  MOZ_ASSERT(aFMDirectory);
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "OpenDatabaseHelper::CreateDatabaseConnection");

  nsresult rv;
  bool exists;

  if (IndexedDatabaseManager::InLowDiskSpaceMode()) {
    rv = aDBFile->Exists(&exists);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (!exists) {
      NS_WARNING("Refusing to create database because disk space is low!");
      return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
    }
  }

  nsCOMPtr<nsIFileURL> dbFileUrl =
    GetDatabaseFileURL(aDBFile, aPersistenceType, aGroup, aOrigin);
  if (NS_WARN_IF(!dbFileUrl)) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<mozIStorageService> ss =
    do_GetService(MOZ_STORAGE_SERVICE_CONTRACTID);
  if (NS_WARN_IF(!ss)) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<mozIStorageConnection> connection;
  rv = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(connection));
  if (rv == NS_ERROR_FILE_CORRUPTED) {
    // If we're just opening the database during origin initialization, then
    // we don't want to erase any files. The failure here will fail origin
    // initialization too.
    if (aName.IsVoid()) {
      return rv;
    }

    // Nuke the database file.
    rv = aDBFile->Remove(false);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = aFMDirectory->Exists(&exists);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (exists) {
      bool isDirectory;
      rv = aFMDirectory->IsDirectory(&isDirectory);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
      if (NS_WARN_IF(!isDirectory)) {
        return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
      }

      rv = aFMDirectory->Remove(true);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    rv = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(connection));
  }
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = SetDefaultPragmas(connection);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = connection->EnableModule(NS_LITERAL_CSTRING("filesystem"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Check to make sure that the database schema is correct.
  int32_t schemaVersion;
  rv = connection->GetSchemaVersion(&schemaVersion);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Unknown schema will fail origin initialization too.
  if (!schemaVersion && aName.IsVoid()) {
    NS_WARNING("Unable to open IndexedDB database, schema is not set!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  if (schemaVersion > kSQLiteSchemaVersion) {
    NS_WARNING("Unable to open IndexedDB database, schema is too high!");
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  bool vacuumNeeded = false;

  if (schemaVersion != kSQLiteSchemaVersion) {
#if defined(MOZ_WIDGET_ANDROID) || defined(MOZ_WIDGET_GONK)
    if (!schemaVersion) {
      // Have to do this before opening a transaction.
      rv = connection->ExecuteSimpleSQL(NS_LITERAL_CSTRING(
        // Turn on auto_vacuum mode to reclaim disk space on mobile devices.
        "PRAGMA auto_vacuum = FULL; "
      ));
      if (rv == NS_ERROR_FILE_NO_DEVICE_SPACE) {
        // mozstorage translates SQLITE_FULL to NS_ERROR_FILE_NO_DEVICE_SPACE,
        // which we know better as NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR.
        rv = NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
      }
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }
#endif

    mozStorageTransaction transaction(connection, false,
                                  mozIStorageConnection::TRANSACTION_IMMEDIATE);

    if (!schemaVersion) {
      // Brand new file, initialize our tables.
      rv = CreateTables(connection);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      MOZ_ASSERT(NS_SUCCEEDED(connection->GetSchemaVersion(&schemaVersion)));
      MOZ_ASSERT(schemaVersion == kSQLiteSchemaVersion);

      nsCOMPtr<mozIStorageStatement> stmt;
      nsresult rv = connection->CreateStatement(NS_LITERAL_CSTRING(
        "INSERT INTO database (name) "
        "VALUES (:name)"
      ), getter_AddRefs(stmt));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      rv = stmt->BindStringByName(NS_LITERAL_CSTRING("name"), aName);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      rv = stmt->Execute();
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    } else  {
      // This logic needs to change next time we change the schema!
      static_assert(kSQLiteSchemaVersion == int32_t((14 << 4) + 0),
                    "Need upgrade code from schema version increase.");

      while (schemaVersion != kSQLiteSchemaVersion) {
        if (schemaVersion == 4) {
          rv = UpgradeSchemaFrom4To5(connection);
        } else if (schemaVersion == 5) {
          rv = UpgradeSchemaFrom5To6(connection);
        } else if (schemaVersion == 6) {
          rv = UpgradeSchemaFrom6To7(connection);
        } else if (schemaVersion == 7) {
          rv = UpgradeSchemaFrom7To8(connection);
        } else if (schemaVersion == 8) {
          rv = UpgradeSchemaFrom8To9_0(connection);
          vacuumNeeded = true;
        } else if (schemaVersion == MakeSchemaVersion(9, 0)) {
          rv = UpgradeSchemaFrom9_0To10_0(connection);
        } else if (schemaVersion == MakeSchemaVersion(10, 0)) {
          rv = UpgradeSchemaFrom10_0To11_0(connection);
        } else if (schemaVersion == MakeSchemaVersion(11, 0)) {
          rv = UpgradeSchemaFrom11_0To12_0(connection);
        } else if (schemaVersion == MakeSchemaVersion(12, 0)) {
          rv = UpgradeSchemaFrom12_0To13_0(connection, &vacuumNeeded);
        } else if (schemaVersion == MakeSchemaVersion(13, 0)) {
          rv = UpgradeSchemaFrom13_0To14_0(connection);
        } else {
          NS_WARNING("Unable to open IndexedDB database, no upgrade path is "
                     "available!");
          return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
        }
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }

        rv = connection->GetSchemaVersion(&schemaVersion);
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }
      }

      MOZ_ASSERT(schemaVersion == kSQLiteSchemaVersion);
    }

    rv = transaction.Commit();
    if (rv == NS_ERROR_FILE_NO_DEVICE_SPACE) {
      // mozstorage translates SQLITE_FULL to NS_ERROR_FILE_NO_DEVICE_SPACE,
      // which we know better as NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR.
      rv = NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
    }
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  if (vacuumNeeded) {
    rv = connection->ExecuteSimpleSQL(NS_LITERAL_CSTRING("VACUUM;"));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  connection.forget(aConnection);
  return NS_OK;
}

nsresult
LoadDatabaseInformation(mozIStorageConnection* aConnection,
                        DatabaseMetadata* aDatabaseMetadata)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);
  MOZ_ASSERT(aDatabaseMetadata);

  // Load version information.
  nsCOMPtr<mozIStorageStatement> stmt;
  nsresult rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT name, version "
    "FROM database"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (NS_WARN_IF(!hasResult)) {
    return NS_ERROR_FILE_CORRUPTED;
  }

  nsString databaseName;
  rv = stmt->GetString(0, databaseName);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (NS_WARN_IF(aDatabaseMetadata->openParams().name() != databaseName)) {
    return NS_ERROR_FILE_CORRUPTED;
  }

  int64_t version;
  rv = stmt->GetInt64(1, &version);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (NS_WARN_IF(version < 0)) {
    return NS_ERROR_FILE_CORRUPTED;
  }

  aDatabaseMetadata->openParams().version() = uint64_t(version);

   // Load object store names and ids.
  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT id, auto_increment, name, key_path "
    "FROM object_store"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  while (NS_SUCCEEDED((rv = stmt->ExecuteStep(&hasResult))) && hasResult) {
    ObjectStoreMetadata* objectStoreMetadata =
      aDatabaseMetadata->objectStores().AppendElement();

    rv = stmt->GetString(2, objectStoreMetadata->name());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->GetInt64(0, &objectStoreMetadata->id());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    int32_t columnType;
    rv = stmt->GetTypeOfIndex(3, &columnType);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (columnType == mozIStorageStatement::VALUE_TYPE_NULL) {
      objectStoreMetadata->keyPath() = KeyPath(0);
    } else {
      MOZ_ASSERT(columnType == mozIStorageStatement::VALUE_TYPE_TEXT);

      nsString keyPathSerialization;
      rv = stmt->GetString(3, keyPathSerialization);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      objectStoreMetadata->keyPath() =
        KeyPath::DeserializeFromString(keyPathSerialization);
      if (NS_WARN_IF(!objectStoreMetadata->keyPath().IsValid())) {
        return NS_ERROR_FILE_CORRUPTED;
      }
    }

    int64_t nextAutoIncrementId;
    rv = stmt->GetInt64(1, &nextAutoIncrementId);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    objectStoreMetadata->autoIncrement() = !!nextAutoIncrementId;
    objectStoreMetadata->nextAutoIncrementId() = nextAutoIncrementId;
    objectStoreMetadata->comittedAutoIncrementId() = nextAutoIncrementId;
  }

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  // Load index information
  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT id, object_store_id, name, key_path, unique_index, multientry "
    "FROM object_store_index"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsTArray<ObjectStoreMetadata>& objectStoreMetadataArray =
    aDatabaseMetadata->objectStores();

  const uint32_t objectStoreMetadataArrayLength =
    objectStoreMetadataArray.Length();

  while (NS_SUCCEEDED((rv = stmt->ExecuteStep(&hasResult))) && hasResult) {
    int64_t objectStoreId;
    rv = stmt->GetInt64(1, &objectStoreId);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    ObjectStoreMetadata* objectStoreMetadata = nullptr;
    for (uint32_t index = 0; index < objectStoreMetadataArrayLength; index++) {
      if (objectStoreMetadataArray[index].id() == objectStoreId) {
        objectStoreMetadata = &objectStoreMetadataArray[index];
        break;
      }
    }

    if (NS_WARN_IF(!objectStoreMetadata)) {
      return NS_ERROR_FILE_CORRUPTED;
    }

    IndexMetadata* indexMetadata =
      objectStoreMetadata->indexes().AppendElement();

    rv = stmt->GetInt64(0, &indexMetadata->id());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->GetString(2, indexMetadata->name());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

#ifdef DEBUG
    {
      int32_t columnType;
      rv = stmt->GetTypeOfIndex(3, &columnType);
      MOZ_ASSERT(NS_SUCCEEDED(rv));
      MOZ_ASSERT(columnType != mozIStorageStatement::VALUE_TYPE_NULL);
    }
#endif

    nsString keyPathSerialization;
    rv = stmt->GetString(3, keyPathSerialization);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    indexMetadata->keyPath() =
      KeyPath::DeserializeFromString(keyPathSerialization);
    if (NS_WARN_IF(!indexMetadata->keyPath().IsValid())) {
      return NS_ERROR_FILE_CORRUPTED;
    }

    int32_t scratch;
    rv = stmt->GetInt32(4, &scratch);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    indexMetadata->unique() = !!scratch;

    rv = stmt->GetInt32(5, &scratch);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    indexMetadata->multiEntry() = !!scratch;
  }

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

#if 0 // XXX Remove me!

// The amount of time that the LazyIdleThread thread keeps the platform thread
// alive before shutting it down.
const uint32_t kIOThreadTimeoutMS = 30000;

// A thread for opening databases and miscellaneous I/O operations that do not
// involve transactions.
StaticRefPtr<LazyIdleThread> gIOThread;

void
CreateIOThread()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!gIOThread);

  gIOThread = new LazyIdleThread(kIOThreadTimeoutMS,
                                 NS_LITERAL_CSTRING("IndexedDB I/O"),
                                 LazyIdleThread::ManualShutdown);
}

void
ShutdownIOThread()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(gIOThread);

  // First transfer ownership to this function, adding a reference because
  // StaticRefPtr doesn't have a forget() method.
  nsRefPtr<LazyIdleThread> ioThread = gIOThread;

  // Null this out so that future actors can recreate the IO thread.
  gIOThread = nullptr;

  // Now shut it down.
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(ioThread->Shutdown()));
}

#else
void CreateIOThread() { }
void ShutdownIOThread() { }
#endif

class DatabaseOperationBase
  : public nsRunnable
  , public mozIStorageProgressHandler
{
  // Uniquely tracks each operation for logging purposes. Only modified on the
  // PBackground thread.
  static uint64_t sNextSerialNumber;

protected:
  nsCOMPtr<nsIEventTarget> mOwningThread;
  const uint64_t mSerialNumber;
  nsresult mResultCode;
  Atomic<uint32_t> mActorDestroyed;

protected:
  DatabaseOperationBase()
    : mOwningThread(NS_GetCurrentThread())
    , mSerialNumber(++sNextSerialNumber)
    , mResultCode(NS_OK)
    , mActorDestroyed(0)
  {
    AssertIsOnOwningThread();
  }

  virtual
  ~DatabaseOperationBase()
  {
    MOZ_ASSERT(mActorDestroyed);
  }

public:
  NS_DECL_ISUPPORTS_INHERITED

  void
  AssertIsOnOwningThread() const
  {
    AssertIsOnBackgroundThread();

#ifdef DEBUG
    MOZ_ASSERT(mOwningThread);
    bool current;
    MOZ_ASSERT(NS_SUCCEEDED(mOwningThread->IsOnCurrentThread(&current)));
    MOZ_ASSERT(current);
#endif
  }

  void
  NoteActorDestroyed()
  {
    AssertIsOnOwningThread();
    MOZ_ASSERT(!mActorDestroyed);

    mActorDestroyed = 1;
  }

  uint64_t
  SerialNumber() const
  {
    return mSerialNumber;
  }

private:
  // Not to be overridden by subclasses.
  NS_DECL_MOZISTORAGEPROGRESSHANDLER
};

class FactoryOp
  : public DatabaseOperationBase
  , public PBackgroundIDBFactoryRequestParent
{
protected:
  enum State
  {
    // Just created on the PBackground thread, dispatched to the main thread.
    State_Initial,

    // Waiting for open allowed/open allowed on the main thread.
    State_OpenPending,

    // Waiting to do/doing work on the QuotaManager IO thread.
    State_DBWork,

    // Waiting on a version change transaction.
    State_SetVersionPending,

    // Waiting to send/sending results on the PBackground thread.
    State_SendingResults,

    // Notifying the QuotaManager that it can proceed to the next operation.
    State_UnblockingQuotaManager,

    // All done.
    State_Completed
  };

  State mState;
  nsCString mGroup;
  nsCString mASCIIOrigin;
  nsString mName;
  nsCString mDatabaseId;
  StoragePrivilege mPrivilege;
  PersistenceType mPersistenceType;

protected:
  FactoryOp(const nsACString& aGroup,
            const nsACString& aASCIIOrigin,
            StoragePrivilege aPrivilege,
            const nsAString& aName,
            PersistenceType aPersistenceType)
    : mState(State_Initial)
    , mGroup(aGroup)
    , mASCIIOrigin(aASCIIOrigin)
    , mName(aName)
    , mPrivilege(aPrivilege)
    , mPersistenceType(aPersistenceType)
  {
    QuotaManager::GetStorageId(aPersistenceType, aASCIIOrigin,
                               Client::IDB, aName, mDatabaseId);
    MOZ_ASSERT(!mDatabaseId.IsEmpty());
  }

  virtual
  ~FactoryOp()
  {
    MOZ_ASSERT(mState == State_Initial || mState == State_Completed);
  }

  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE
  {
    NoteActorDestroyed();
  }

  nsresult
  Open();

  nsresult
  SendToIOThread();

  void
  UnblockQuotaManager();
};

class OpenDatabaseOp MOZ_FINAL
  : public FactoryOp
{
  DatabaseMetadata mMetadata;
  uint64_t mRequestedVersion;
  nsString mDatabaseFilePath;
  nsRefPtr<FileManager> mFileManager;
  int64_t mLastObjectStoreId;
  int64_t mLastIndexId;

public:
  OpenDatabaseOp(const nsACString& aGroup,
                 const nsACString& aASCIIOrigin,
                 StoragePrivilege aPrivilege,
                 const OpenDatabaseRequestParams& aParams)
    : FactoryOp(aGroup, aASCIIOrigin, aPrivilege, aParams.name(),
                aParams.persistenceType())
    , mRequestedVersion(aParams.version())
    , mLastObjectStoreId(0)
    , mLastIndexId(0)
  {
    mMetadata.openParams() = aParams;
    mMetadata.nextObjectStoreId() = 0;
    mMetadata.nextIndexId() = 0;
  }

private:
  ~OpenDatabaseOp()
  { }

  NS_DECL_NSIRUNNABLE

  nsresult
  DoDatabaseWork();

  nsresult
  SetVersion();

  void
  SendResults();
};

class DeleteDatabaseOp MOZ_FINAL
  : public FactoryOp
{
public:
  DeleteDatabaseOp(const nsACString& aGroup,
                   const nsACString& aASCIIOrigin,
                   StoragePrivilege aPrivilege,
                   const DeleteDatabaseRequestParams& aParams)
    : FactoryOp(aGroup, aASCIIOrigin, aPrivilege, aParams.name(),
                aParams.persistenceType())
  { }

private:
  ~DeleteDatabaseOp()
  { }

  NS_DECL_NSIRUNNABLE

  nsresult
  DoDatabaseWork();

  void
  SendResults();
};

class CommonDatabaseOperationBase : public DatabaseOperationBase
{
protected:
  CommonDatabaseOperationBase()
  { }

  virtual
  ~CommonDatabaseOperationBase()
  { }

  // Must be overridden in subclasses. Called on the target thread to allow the
  // subclass to perform necessary database or file operations. A successful
  // return value will trigger a SendSuccessResult callback on the background
  // thread while a failure value will trigger a SendFailureResult callback.
  virtual nsresult
  DoDatabaseWork() = 0;

  // Must be overridden in subclasses. Called on the background thread to allow
  // the subclass to serialize its results and send them to the child actor. A
  // failed return value will trigger a SendFailureResult callback.
  virtual nsresult
  SendSuccessResult() = 0;

  // Must be overridden in subclasses. Called on the background thread to allow
  // the subclass to its failure code.
  virtual void
  SendFailureResult(nsresult aResultCode) = 0;

  // This callback will be called on the background thread before releasing the
  // final reference to this request object. Subclasses may perform any
  // additional cleanup here.
  virtual void
  Cleanup()
  { }

private:
  // Not to be overridden by subclasses.
  NS_DECL_NSIRUNNABLE
};

} // anonymous namespace

/*******************************************************************************
 * BackgroundFactoryParent
 ******************************************************************************/

uint64_t BackgroundFactoryParent::sFactoryInstanceCount = 0;

// static
BackgroundFactoryParent*
BackgroundFactoryParent::Create(const nsCString& aGroup,
                                const nsCString& aASCIIOrigin,
                                const StoragePrivilege& aPrivilege)
{
  AssertIsOnBackgroundThread();

  // If this is the first instance then we need to do some initialization.
  if (!(sFactoryInstanceCount++)) {
    // We need to create the IO thread here.
    CreateIOThread();
  }

  return new BackgroundFactoryParent(aGroup, aASCIIOrigin, aPrivilege);
}

BackgroundFactoryParent::BackgroundFactoryParent(
                                             const nsCString& aGroup,
                                             const nsCString& aASCIIOrigin,
                                             const StoragePrivilege& aPrivilege)
: mGroup(aGroup), mASCIIOrigin(aASCIIOrigin), mPrivilege(aPrivilege)
{
  AssertIsOnBackgroundThread();
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundFactoryParent);
}

BackgroundFactoryParent::~BackgroundFactoryParent()
{
  AssertIsOnBackgroundThread();
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundFactoryParent);
}

void
BackgroundFactoryParent::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();

  // Shut down the IO thread if there are no more instances.
  if (!(--sFactoryInstanceCount)) {
    ShutdownIOThread();
  }
}

PBackgroundIDBFactoryRequestParent*
BackgroundFactoryParent::AllocPBackgroundIDBFactoryRequestParent(
                                            const FactoryRequestParams& aParams)
{
  AssertIsOnBackgroundThread();

  nsRefPtr<FactoryOp> op;

  switch (aParams.type()) {
    case FactoryRequestParams::TOpenDatabaseRequestParams:
      op = new OpenDatabaseOp(mGroup, mASCIIOrigin, mPrivilege, aParams);
      break;

    case FactoryRequestParams::TDeleteDatabaseRequestParams:
      op = new DeleteDatabaseOp(mGroup, mASCIIOrigin, mPrivilege, aParams);
      break;

    default:
      MOZ_CRASH("Unknown request type!");
  }

  // Transfer ownership to IPDL.
  return op.forget().get();
}

bool
BackgroundFactoryParent::RecvPBackgroundIDBFactoryRequestConstructor(
                                     PBackgroundIDBFactoryRequestParent* aActor,
                                     const FactoryRequestParams& aParams)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  auto op = static_cast<FactoryOp*>(aActor);

  // XXX This is awful, we should avoid any use of the main thread.
  //     Unfortunately we have to wait for the QuotaManager to be updated before
  //     this can be achieved.
  if (NS_WARN_IF(NS_FAILED(NS_DispatchToMainThread(op)))) {
    unused <<
      PBackgroundIDBFactoryRequestParent::
        Send__delete__(op, NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
  }

  return true;
}

bool
BackgroundFactoryParent::DeallocPBackgroundIDBFactoryRequestParent(
                                     PBackgroundIDBFactoryRequestParent* aActor)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  // Transfer ownership back from IPDL.
  nsRefPtr<FactoryOp> op = dont_AddRef(static_cast<FactoryOp*>(aActor));
  return true;
}

PBackgroundIDBDatabaseParent*
BackgroundFactoryParent::AllocPBackgroundIDBDatabaseParent(
                                              const DatabaseMetadata& aMetadata)
{
  AssertIsOnBackgroundThread();

  return new BackgroundDatabaseParent(aMetadata);
}

bool
BackgroundFactoryParent::DeallocPBackgroundIDBDatabaseParent(
                                           PBackgroundIDBDatabaseParent* aActor)
{
  AssertIsOnBackgroundThread();

  delete static_cast<BackgroundDatabaseParent*>(aActor);
  return true;
}

/*******************************************************************************
 * BackgroundDatabaseParent
 ******************************************************************************/

BackgroundDatabaseParent::BackgroundDatabaseParent(
                                              const DatabaseMetadata& aMetadata)
: mMetadata(aMetadata)
{
  AssertIsOnBackgroundThread();
  MOZ_COUNT_CTOR(mozilla::dom::indexedDB::BackgroundDatabaseParent);
}

BackgroundDatabaseParent::~BackgroundDatabaseParent()
{
  AssertIsOnBackgroundThread();
  MOZ_COUNT_DTOR(mozilla::dom::indexedDB::BackgroundDatabaseParent);
}

void
BackgroundDatabaseParent::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();
}

/*******************************************************************************
 * Local class implementations
 ******************************************************************************/

NS_IMPL_ISUPPORTS1(CompressDataBlobsFunction, mozIStorageFunction)
NS_IMPL_ISUPPORTS1(EncodeKeysFunction, mozIStorageFunction)

uint64_t DatabaseOperationBase::sNextSerialNumber = 0;

NS_IMPL_ISUPPORTS_INHERITED1(DatabaseOperationBase,
                             nsRunnable,
                             mozIStorageProgressHandler)

NS_IMETHODIMP
DatabaseOperationBase::OnProgress(mozIStorageConnection* aConnection,
                                  bool* _retval)
{
  *_retval = !!mActorDestroyed;
  return NS_OK;
}

nsresult
FactoryOp::Open()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mState == State_Initial);

  QuotaManager* quotaManager = QuotaManager::GetOrCreate();
  if (NS_WARN_IF(!quotaManager)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  nsresult rv = quotaManager->
    WaitForOpenAllowed(OriginOrPatternString::FromOrigin(mASCIIOrigin),
                       Nullable<PersistenceType>(mPersistenceType), mDatabaseId,
                       this);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  mState = State_OpenPending;
  return NS_OK;
}

nsresult
FactoryOp::SendToIOThread()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mState == State_OpenPending);

  QuotaManager* quotaManager = QuotaManager::Get();
  if (NS_WARN_IF(!quotaManager)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  // Must set this before dispatching otherwise we will race with the IO thread.
  mState = State_DBWork;

  nsresult rv = quotaManager->IOThread()->Dispatch(this, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
#ifdef DEBUG
    mState = State_OpenPending;
#endif
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  return NS_OK;
}

void
FactoryOp::UnblockQuotaManager()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mState == State_UnblockingQuotaManager);

  if (QuotaManager* quotaManager = QuotaManager::Get()) {
    quotaManager->
      AllowNextSynchronizedOp(OriginOrPatternString::FromOrigin(mASCIIOrigin),
                              Nullable<PersistenceType>(mPersistenceType),
                              mDatabaseId);
  } else {
    NS_WARNING("QuotaManager went away before we could unblock it!");
  }

  mState = State_Completed;
}

NS_IMETHODIMP
OpenDatabaseOp::Run()
{
  nsresult rv;

  switch (mState) {
    case State_Initial:
      rv = Open();
      break;

    case State_OpenPending:
      rv = SendToIOThread();
      break;

    case State_DBWork:
      rv = DoDatabaseWork();
      break;

    case State_SetVersionPending:
      rv = SetVersion();
      break;

    case State_SendingResults:
      SendResults();
      return NS_OK;

    case State_UnblockingQuotaManager:
      UnblockQuotaManager();
      return NS_OK;

    default:
      MOZ_CRASH("Bad state!");
  }

  if (NS_WARN_IF(NS_FAILED(rv))) {
    if (NS_SUCCEEDED(mResultCode)) {
      mResultCode = rv;
    }

    mState = State_SendingResults;

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(mOwningThread->Dispatch(this,
                                                         NS_DISPATCH_NORMAL)));
  }

  return NS_OK;
}

nsresult
OpenDatabaseOp::DoDatabaseWork()
{
  AssertIsOnIOThread();
  MOZ_ASSERT(mState == State_DBWork);

  PROFILER_LABEL("IndexedDB", "OpenDatabaseHelper::DoDatabaseWork");

  if (NS_WARN_IF(QuotaManager::IsShuttingDown())) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  QuotaManager* quotaManager = QuotaManager::Get();
  MOZ_ASSERT(quotaManager);

  nsCOMPtr<nsIFile> dbDirectory;

  nsresult rv =
    quotaManager->EnsureOriginIsInitialized(mPersistenceType, mGroup,
                                            mASCIIOrigin, mPrivilege != Chrome,
                                            getter_AddRefs(dbDirectory));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbDirectory->Append(NS_LITERAL_STRING(IDB_DIRECTORY_NAME));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  bool exists;
  rv = dbDirectory->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (!exists) {
    rv = dbDirectory->Create(nsIFile::DIRECTORY_TYPE, 0755);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }
#ifdef DEBUG
  else {
    bool isDirectory;
    MOZ_ASSERT(NS_SUCCEEDED(dbDirectory->IsDirectory(&isDirectory)));
    MOZ_ASSERT(isDirectory);
  }
#endif

  nsAutoString filename;
  GetDatabaseFilename(mName, filename);

  nsCOMPtr<nsIFile> dbFile;
  rv = dbDirectory->Clone(getter_AddRefs(dbFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbFile->Append(filename + NS_LITERAL_STRING(".sqlite"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbFile->GetPath(mDatabaseFilePath);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<nsIFile> fmDirectory;
  rv = dbDirectory->Clone(getter_AddRefs(fmDirectory));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = fmDirectory->Append(filename);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<mozIStorageConnection> connection;
  rv = CreateDatabaseConnection(dbFile, fmDirectory, mName, mPersistenceType,
                                mGroup, mASCIIOrigin,
                                getter_AddRefs(connection));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = LoadDatabaseInformation(connection, &mMetadata);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  for (uint32_t i = 0; i < mMetadata.objectStores().Length(); i++) {
    const ObjectStoreMetadata& objectStoreMetadata =
      mMetadata.objectStores()[i];

    for (uint32_t j = 0; j < objectStoreMetadata.indexes().Length(); j++) {
      const IndexMetadata& indexMetadata = objectStoreMetadata.indexes()[j];
      mLastIndexId = std::max(indexMetadata.id(), mLastIndexId);
    }
    mLastObjectStoreId = std::max(objectStoreMetadata.id(), mLastObjectStoreId);
  }

  // See if we need to do a VERSION_CHANGE transaction

  uint64_t& currentVersion = mMetadata.openParams().version();

  // Optional version semantics.
  if (!mRequestedVersion) {
    // If the requested version was not specified and the database was created,
    // treat it as if version 1 were requested.
    if (currentVersion == 0) {
      mRequestedVersion = 1;
    } else {
      // Otherwise, treat it as if the current version were requested.
      mRequestedVersion = currentVersion;
    }
  }

  if (currentVersion > mRequestedVersion) {
    return NS_ERROR_DOM_INDEXEDDB_VERSION_ERR;
  }

  IndexedDatabaseManager* mgr = IndexedDatabaseManager::Get();
  MOZ_ASSERT(mgr);

  nsRefPtr<FileManager> fileManager =
    mgr->GetFileManager(mPersistenceType, mASCIIOrigin, mName);
  if (!fileManager) {
    fileManager = new FileManager(mPersistenceType, mGroup, mASCIIOrigin,
                                  mPrivilege, mMetadata.openParams().name());

    rv = fileManager->Init(fmDirectory, connection);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    mgr->AddFileManager(fileManager);
  }

  mFileManager = fileManager.forget();

  // Must set this before dispatching otherwise we will race with the owning
  // thread.
  mState = (currentVersion != mRequestedVersion) ?
           State_SetVersionPending :
           State_SendingResults;

  rv = mOwningThread->Dispatch(this, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
#ifdef DEBUG
    mState = State_DBWork;
#endif
    return rv;
  }

  return NS_OK;
}

nsresult
OpenDatabaseOp::SetVersion()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_SetVersionPending);

  MOZ_CRASH("Implement me!");

  mState = State_SendingResults;
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToCurrentThread(this)));

  return NS_OK;
}

void
OpenDatabaseOp::SendResults()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_SendingResults);

  FactoryRequestResponse response;

  if (NS_SUCCEEDED(mResultCode)) {
    // Inform the child that we have opened a new database.
    PBackgroundIDBDatabaseParent* databaseActor =
      Manager()->SendPBackgroundIDBDatabaseConstructor(mMetadata);

    if (databaseActor) {
      // We successfully opened a database so use its actor as the success
      // result for this request.
      OpenDatabaseRequestResponse openResponse;
      openResponse.databaseParent() = databaseActor;
      response = openResponse;
    } else {
      // Child must have crashed. We must continue to the next state and unblock
      // the quota manager or else all other indexedDB operations could remain
      // blocked.
      response = NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }
  } else {
    response = ClampErrorCode(mResultCode);
  }

  unused << PBackgroundIDBFactoryRequestParent::Send__delete__(this, response);

  mState = State_UnblockingQuotaManager;

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(this)));
}

NS_IMETHODIMP
DeleteDatabaseOp::Run()
{
  nsresult rv;

  switch (mState) {
    case State_Initial:
      rv = Open();
      break;

    case State_OpenPending:
      rv = SendToIOThread();
      break;

    case State_DBWork:
      rv = DoDatabaseWork();
      break;

    case State_SendingResults:
      SendResults();
      return NS_OK;

    case State_UnblockingQuotaManager:
      UnblockQuotaManager();
      return NS_OK;

    default:
      MOZ_CRASH("Bad state!");
  }

  if (NS_WARN_IF(NS_FAILED(rv))) {
    if (NS_SUCCEEDED(mResultCode)) {
      mResultCode = rv;
    }

    mState = State_SendingResults;

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(mOwningThread->Dispatch(this,
                                                         NS_DISPATCH_NORMAL)));
  }

  return NS_OK;
}

nsresult
DeleteDatabaseOp::DoDatabaseWork()
{
  AssertIsOnIOThread();
  MOZ_ASSERT(mState == State_DBWork);

  PROFILER_LABEL("IndexedDB", "DeleteDatabaseOp::DoDatabaseWork");

  QuotaManager* quotaManager = QuotaManager::Get();
  MOZ_ASSERT(quotaManager);

  nsCOMPtr<nsIFile> directory;
  nsresult rv = quotaManager->GetDirectoryForOrigin(mPersistenceType,
                                                    mASCIIOrigin,
                                                    getter_AddRefs(directory));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = directory->Append(NS_LITERAL_STRING(IDB_DIRECTORY_NAME));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsAutoString filename;
  GetDatabaseFilename(mName, filename);

  nsCOMPtr<nsIFile> dbFile;
  rv = directory->Clone(getter_AddRefs(dbFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbFile->Append(filename + NS_LITERAL_STRING(".sqlite"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  bool exists = false;
  rv = dbFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (exists) {
    int64_t fileSize;

    if (mPrivilege != Chrome) {
      rv = dbFile->GetFileSize(&fileSize);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    rv = dbFile->Remove(false);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (mPrivilege != Chrome) {
      quotaManager->DecreaseUsageForOrigin(mPersistenceType, mGroup,
                                           mASCIIOrigin, fileSize);
    }
  }

  nsCOMPtr<nsIFile> dbJournalFile;
  rv = directory->Clone(getter_AddRefs(dbJournalFile));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbJournalFile->Append(filename + NS_LITERAL_STRING(".sqlite-journal"));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbJournalFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (exists) {
    rv = dbJournalFile->Remove(false);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  nsCOMPtr<nsIFile> fmDirectory;
  rv = directory->Clone(getter_AddRefs(fmDirectory));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = fmDirectory->Append(filename);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = fmDirectory->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (exists) {
    bool isDirectory;
    rv = fmDirectory->IsDirectory(&isDirectory);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
    if (NS_WARN_IF(!isDirectory)) {
      return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
    }

    uint64_t usage = 0;

    if (mPrivilege != Chrome) {
      rv = FileManager::GetUsage(fmDirectory, &usage);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    rv = fmDirectory->Remove(true);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (mPrivilege != Chrome) {
      quotaManager->DecreaseUsageForOrigin(mPersistenceType, mGroup,
                                           mASCIIOrigin, usage);
    }
  }

  IndexedDatabaseManager* mgr = IndexedDatabaseManager::Get();
  MOZ_ASSERT(mgr);

  mgr->InvalidateFileManager(mPersistenceType, mASCIIOrigin, mName);

  // Must set this before dispatching otherwise we will race with the owning
  // thread.
  mState = State_SendingResults;

  rv = mOwningThread->Dispatch(this, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
#ifdef DEBUG
    mState = State_DBWork;
#endif
    return rv;
  }

  return NS_OK;
}

void
DeleteDatabaseOp::SendResults()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_SendingResults);

  FactoryRequestResponse response;

  if (NS_SUCCEEDED(mResultCode)) {
    response = DeleteDatabaseRequestResponse();
  } else {
    response = ClampErrorCode(mResultCode);
  }

  unused << PBackgroundIDBFactoryRequestParent::Send__delete__(this, response);

  mState = State_UnblockingQuotaManager;

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(this)));
}

NS_IMETHODIMP
CommonDatabaseOperationBase::Run()
{
  if (!IsOnBackgroundThread()) {
    MOZ_ASSERT(mOwningThread);

    nsCOMPtr<nsIEventTarget> owningThread;
    mOwningThread.swap(owningThread);

    if (NS_WARN_IF(mActorDestroyed)) {
      // The child must have crashed so there's no reason to attempt any
      // database operations.
      return NS_OK;
    }

    mResultCode = DoDatabaseWork();

    if (NS_WARN_IF(NS_FAILED(owningThread->Dispatch(this,
                                                    NS_DISPATCH_NORMAL)))) {
      // This should only happen if the child has crashed.
      MOZ_ASSERT(mActorDestroyed);
      return NS_ERROR_FAILURE;
    }

    return NS_OK;
  }

  MOZ_ASSERT(!mOwningThread);

  if (NS_SUCCEEDED(mResultCode)) {
    // This may release the IPDL reference.
    mResultCode = SendSuccessResult();
  }

  if (NS_WARN_IF(NS_FAILED(mResultCode))) {
    // This should definitely release the IPDL reference.
    SendFailureResult(mResultCode);
  }

  Cleanup();

  return mResultCode;
}
