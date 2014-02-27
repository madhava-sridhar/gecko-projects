/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ActorsParent.h"

#include <algorithm>
#include "DatabaseInfo.h"
#include "FileInfo.h"
#include "FileManager.h"
#include "IDBObjectStore.h"
#include "IDBTransaction.h"
#include "IndexedDatabaseManager.h"
#include "js/StructuredClone.h"
#include "KeyPath.h"
#include "mozilla/LazyIdleThread.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/storage.h"
#include "mozilla/unused.h"
#include "mozilla/dom/indexedDB/IDBTransaction.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBDatabaseParent.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBFactoryRequestParent.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBTransactionParent.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBVersionChangeTransactionParent.h"
#include "mozilla/dom/quota/StoragePrivilege.h"
#include "mozilla/dom/quota/AcquireListener.h"
#include "mozilla/dom/quota/OriginOrPatternString.h"
#include "mozilla/dom/quota/QuotaManager.h"
#include "mozilla/ipc/BackgroundParent.h"
#include "mozilla/ipc/PBackground.h"
#include "nsClassHashtable.h"
#include "nsEscape.h"
#include "nsHashKeys.h"
#include "nsIEventTarget.h"
#include "nsIFile.h"
#include "nsIFileURL.h"
#include "nsInterfaceHashtable.h"
#include "nsISupports.h"
#include "nsNetUtil.h"
#include "nsPrintfCString.h"
#include "nsThreadUtils.h"
#include "ProfilerHelpers.h"
#include "ReportInternalError.h"
#include "snappy/snappy.h"
#include "TransactionThreadPool.h"

using namespace mozilla;
using namespace mozilla::dom::indexedDB;
using namespace mozilla::dom::quota;
using namespace mozilla::ipc;

/*******************************************************************************
 * Constants
 ******************************************************************************/

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

const int32_t kSQLiteSchemaVersion =
  int32_t((kMajorSchemaVersion << 4) + kMinorSchemaVersion);

} // anonymous namespace

/*******************************************************************************
 * Metadata classes
 ******************************************************************************/

namespace {

struct FullIndexMetadata
{
  friend class nsAutoPtr<FullIndexMetadata>;

  IndexMetadata mCommonMetadata;
  bool mDeleted;

  FullIndexMetadata()
    : mDeleted(false)
  {
    // This can happen either on the QuotaManager IO thread or on a
    // versionchange transaction thread. These threads can never race so this is
    // totally safe.

    MOZ_COUNT_CTOR(mozilla::dom::indexedDB::FullIndexMetadata);

    mCommonMetadata.id() = 0;
    mCommonMetadata.unique() = false;
    mCommonMetadata.multiEntry() = false;
  }

private:
  ~FullIndexMetadata()
  {
    MOZ_COUNT_DTOR(mozilla::dom::indexedDB::FullIndexMetadata);
  }
};

struct FullObjectStoreMetadata
{
  friend class nsAutoPtr<FullObjectStoreMetadata>;

  ObjectStoreMetadata mCommonMetadata;
  nsTArray<nsAutoPtr<FullIndexMetadata>> mIndexes;

  // These two members are only ever touched on a transaction thread!
  int64_t mNextAutoIncrementId;
  int64_t mComittedAutoIncrementId;

  bool mDeleted;

  FullObjectStoreMetadata()
    : mNextAutoIncrementId(0)
    , mComittedAutoIncrementId(0)
    , mDeleted(false)
  {
    // This can happen either on the QuotaManager IO thread or on a
    // versionchange transaction thread. These threads can never race so this is
    // totally safe.

    MOZ_COUNT_CTOR(mozilla::dom::indexedDB::FullObjectStoreMetadata);

    mCommonMetadata.id() = 0;
    mCommonMetadata.autoIncrement() = false;
  }

private:
  ~FullObjectStoreMetadata()
  {
    MOZ_COUNT_DTOR(mozilla::dom::indexedDB::FullObjectStoreMetadata);
  }
};

struct FullDatabaseMetadata
{
  friend class nsAutoPtr<FullDatabaseMetadata>;

  DatabaseMetadata mCommonMetadata;
  nsCString mDatabaseId;
  nsString mFilePath;
  int64_t mNextObjectStoreId;
  int64_t mNextIndexId;
  nsTArray<nsAutoPtr<FullObjectStoreMetadata>> mObjectStores;

  FullDatabaseMetadata()
    : mNextObjectStoreId(0)
    , mNextIndexId(0)
  {
    AssertIsOnBackgroundThread();

    MOZ_COUNT_CTOR(mozilla::dom::indexedDB::FullDatabaseMetadata);

    mCommonMetadata.version() = 0;
    mCommonMetadata.persistenceType() = PERSISTENCE_TYPE_TEMPORARY;
  }

private:
  ~FullDatabaseMetadata()
  {
    MOZ_COUNT_DTOR(mozilla::dom::indexedDB::FullDatabaseMetadata);
  }
};

} // anonymous namespace

/*******************************************************************************
 * SQLite functions
 ******************************************************************************/

namespace {

int32_t
MakeSchemaVersion(uint32_t aMajorSchemaVersion,
                  uint32_t aMinorSchemaVersion)
{
  return int32_t((aMajorSchemaVersion << 4) + aMinorSchemaVersion);
}

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
ClampResultCode(nsresult aResultCode)
{
  if (NS_SUCCEEDED(aResultCode) ||
      NS_ERROR_GET_MODULE(aResultCode) == NS_ERROR_MODULE_DOM_INDEXEDDB) {
    return aResultCode;
  }

  switch (aResultCode) {
    case NS_ERROR_FILE_NO_DEVICE_SPACE:
      return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
    case NS_ERROR_STORAGE_CONSTRAINT:
      return NS_ERROR_DOM_INDEXEDDB_CONSTRAINT_ERR;
  }

#ifdef DEBUG
  {
    nsPrintfCString message("Converting non-IndexedDB error code (0x%X) to "
                            "NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR",
                            aResultCode);
    NS_WARNING(message.get());
  }
#endif

  IDB_REPORT_INTERNAL_ERR();
  return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
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

nsresult
GetDatabaseFileURL(nsIFile* aDatabaseFile,
                   PersistenceType aPersistenceType,
                   const nsACString& aGroup,
                   const nsACString& aOrigin,
                   nsIFileURL** aResult)
{
  MOZ_ASSERT(aDatabaseFile);
  MOZ_ASSERT(aResult);

  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewFileURI(getter_AddRefs(uri), aDatabaseFile);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<nsIFileURL> fileUrl = do_QueryInterface(uri);
  MOZ_ASSERT(fileUrl);

  nsAutoCString type;
  PersistenceTypeToText(aPersistenceType, type);

  rv = fileUrl->SetQuery(NS_LITERAL_CSTRING("persistenceType=") + type +
                         NS_LITERAL_CSTRING("&group=") + aGroup +
                         NS_LITERAL_CSTRING("&origin=") + aOrigin);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  fileUrl.forget(aResult);
  return NS_OK;
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

  PROFILER_LABEL("IndexedDB", "CreateDatabaseConnection");

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

  nsCOMPtr<nsIFileURL> dbFileUrl;
  rv = GetDatabaseFileURL(aDBFile, aPersistenceType, aGroup, aOrigin,
                          getter_AddRefs(dbFileUrl));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<mozIStorageService> ss =
    do_GetService(MOZ_STORAGE_SERVICE_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
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
GetDatabaseConnection(const nsAString& aDatabaseFilePath,
                      PersistenceType aPersistenceType,
                      const nsACString& aGroup,
                      const nsACString& aOrigin,
                      mozIStorageConnection** aConnection)
{
  MOZ_ASSERT(!NS_IsMainThread());
  MOZ_ASSERT(!IsOnBackgroundThread());
  MOZ_ASSERT(!aDatabaseFilePath.IsEmpty());
  MOZ_ASSERT(StringEndsWith(aDatabaseFilePath, NS_LITERAL_STRING(".sqlite")));
  MOZ_ASSERT(aConnection);

  PROFILER_LABEL("IndexedDB", "GetDatabaseConnection");

  nsresult rv;
  nsCOMPtr<nsIFile> dbFile =
    do_CreateInstance(NS_LOCAL_FILE_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = dbFile->InitWithPath(aDatabaseFilePath);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  bool exists;
  rv = dbFile->Exists(&exists);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (NS_WARN_IF(!exists)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  nsCOMPtr<nsIFileURL> dbFileUrl;
  rv = GetDatabaseFileURL(dbFile, aPersistenceType, aGroup, aOrigin,
                          getter_AddRefs(dbFileUrl));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<mozIStorageService> ss =
    do_GetService(MOZ_STORAGE_SERVICE_CONTRACTID, &rv);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsCOMPtr<mozIStorageConnection> connection;
  rv = ss->OpenDatabaseWithFileURL(dbFileUrl, getter_AddRefs(connection));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = SetDefaultPragmas(connection);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  connection.forget(aConnection);
  return NS_OK;
}

nsresult
LoadDatabaseInformation(mozIStorageConnection* aConnection,
                        FullDatabaseMetadata* aDatabaseMetadata)
{
  AssertIsOnIOThread();
  MOZ_ASSERT(aConnection);
  MOZ_ASSERT(aDatabaseMetadata);

  nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& objectStoreMetadataArray =
    aDatabaseMetadata->mObjectStores;

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

  if (NS_WARN_IF(aDatabaseMetadata->mCommonMetadata.name() != databaseName)) {
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

  aDatabaseMetadata->mCommonMetadata.version() = uint64_t(version);

   // Load object store names and ids.
  rv = aConnection->CreateStatement(NS_LITERAL_CSTRING(
    "SELECT id, auto_increment, name, key_path "
    "FROM object_store"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  while (NS_SUCCEEDED((rv = stmt->ExecuteStep(&hasResult))) && hasResult) {
    nsAutoPtr<FullObjectStoreMetadata> metadata(new FullObjectStoreMetadata());

    rv = stmt->GetString(2, metadata->mCommonMetadata.name());
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->GetInt64(0, &(metadata->mCommonMetadata.id()));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    int32_t columnType;
    rv = stmt->GetTypeOfIndex(3, &columnType);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (columnType == mozIStorageStatement::VALUE_TYPE_NULL) {
      metadata->mCommonMetadata.keyPath() = KeyPath(0);
    } else {
      MOZ_ASSERT(columnType == mozIStorageStatement::VALUE_TYPE_TEXT);

      nsString keyPathSerialization;
      rv = stmt->GetString(3, keyPathSerialization);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      metadata->mCommonMetadata.keyPath() =
        KeyPath::DeserializeFromString(keyPathSerialization);
      if (NS_WARN_IF(!metadata->mCommonMetadata.keyPath().IsValid())) {
        return NS_ERROR_FILE_CORRUPTED;
      }
    }

    int64_t nextAutoIncrementId;
    rv = stmt->GetInt64(1, &nextAutoIncrementId);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    metadata->mCommonMetadata.autoIncrement() = !!nextAutoIncrementId;
    metadata->mNextAutoIncrementId = nextAutoIncrementId;
    metadata->mComittedAutoIncrementId = nextAutoIncrementId;

    objectStoreMetadataArray.AppendElement(metadata.forget());
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

  const uint32_t objectStoreMetadataArrayLength =
    objectStoreMetadataArray.Length();

  while (NS_SUCCEEDED((rv = stmt->ExecuteStep(&hasResult))) && hasResult) {
    int64_t objectStoreId;
    rv = stmt->GetInt64(1, &objectStoreId);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    FullObjectStoreMetadata* objectStoreMetadata = nullptr;
    for (uint32_t index = 0; index < objectStoreMetadataArrayLength; index++) {
      if (objectStoreMetadataArray[index]->mCommonMetadata.id() ==
          objectStoreId) {
        objectStoreMetadata = objectStoreMetadataArray[index];
        break;
      }
    }

    if (NS_WARN_IF(!objectStoreMetadata)) {
      return NS_ERROR_FILE_CORRUPTED;
    }

    nsAutoPtr<FullIndexMetadata> indexMetadata(new FullIndexMetadata());

    rv = stmt->GetInt64(0, &(indexMetadata->mCommonMetadata.id()));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->GetString(2, indexMetadata->mCommonMetadata.name());
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

    indexMetadata->mCommonMetadata.keyPath() =
      KeyPath::DeserializeFromString(keyPathSerialization);
    if (NS_WARN_IF(!indexMetadata->mCommonMetadata.keyPath().IsValid())) {
      return NS_ERROR_FILE_CORRUPTED;
    }

    int32_t scratch;
    rv = stmt->GetInt32(4, &scratch);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    indexMetadata->mCommonMetadata.unique() = !!scratch;

    rv = stmt->GetInt32(5, &scratch);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    indexMetadata->mCommonMetadata.multiEntry() = !!scratch;

    objectStoreMetadata->mIndexes.AppendElement(indexMetadata.forget());
  }

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

} // anonymous namespace

/*******************************************************************************
 * Actor class declarations
 ******************************************************************************/

namespace {

class TransactionBase;

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
  Atomic<bool> mActorDestroyed;

protected:
  DatabaseOperationBase()
    : mOwningThread(NS_GetCurrentThread())
    , mSerialNumber(++sNextSerialNumber)
    , mResultCode(NS_OK)
    , mActorDestroyed(false)
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

    mActorDestroyed = true;
  }

  bool
  IsActorDestroyed() const
  {
    AssertIsOnOwningThread();
    return mActorDestroyed;
  }

  uint64_t
  SerialNumber() const
  {
    return mSerialNumber;
  }

  nsresult
  ResultCode() const
  {
    return mResultCode;
  }

  void
  SetFailureCode(nsresult aErrorCode)
  {
    MOZ_ASSERT(NS_SUCCEEDED(mResultCode));
    MOZ_ASSERT(NS_FAILED(aErrorCode));
    mResultCode = aErrorCode;
  }

private:
  // Not to be overridden by subclasses.
  NS_DECL_MOZISTORAGEPROGRESSHANDLER
};

class CommonDatabaseOperationBase
  : public DatabaseOperationBase
{
  nsRefPtr<TransactionBase> mTransaction;

public:
  void
  AssertIsOnTransactionThread() const
#ifdef DEBUG
  ;
#else
  { }
#endif

  void
  DispatchToTransactionThreadPool();

protected:
  CommonDatabaseOperationBase(TransactionBase* aTransaction)
    : mTransaction(aTransaction)
  {
    MOZ_ASSERT(aTransaction);
  }

  virtual
  ~CommonDatabaseOperationBase()
  {
    MOZ_ASSERT(!mTransaction,
               "CommonDatabaseOperationBase::Cleanup() was not called by a "
               "subclass!");
  }

  // Must be overridden in subclasses. Called on the target thread to allow the
  // subclass to perform necessary database or file operations. A successful
  // return value will trigger a SendSuccessResult callback on the background
  // thread while a failure value will trigger a SendFailureResult callback.
  virtual nsresult
  DoDatabaseWork(TransactionBase* aTransaction) = 0;

  // Must be overridden in subclasses. Called on the background thread to allow
  // the subclass to serialize its results and send them to the child actor. A
  // failed return value will trigger a SendFailureResult callback.
  virtual nsresult
  SendSuccessResult() = 0;

  // Must be overridden in subclasses. Called on the background thread to allow
  // the subclass to send its failure code. Returning false will cause the
  // transaction to be aborted with aResultCode. Returning true will not cause
  // the transaction to be aborted.
  virtual bool
  SendFailureResult(nsresult aResultCode) = 0;

  // This callback will be called on the background thread before releasing the
  // final reference to this request object. Subclasses may perform any
  // additional cleanup here but must always call the base class implementation.
  virtual void
  Cleanup()
  {
    AssertIsOnOwningThread();
    MOZ_ASSERT(mTransaction);

    mTransaction = nullptr;
  }

private:
  // Not to be overridden by subclasses.
  NS_DECL_NSIRUNNABLE
};

class VersionChangeTransaction;

class Database MOZ_FINAL
  : public PBackgroundIDBDatabaseParent
{
  friend class VersionChangeTransaction;

  nsRefPtr<BackgroundFactoryParent> mFactory;
  FullDatabaseMetadata* mMetadata;
  nsRefPtr<FileManager> mFileManager;

  bool mClosed;
  bool mInvalidated;

public:
  // Created by OpenDatabaseOp.
  Database(BackgroundFactoryParent* aFactory,
           FullDatabaseMetadata* aMetadata,
           FileManager* mFileManager);

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(Database)

  void
  Invalidate();

  const nsCString&
  Group() const
  {
    MOZ_ASSERT(mFactory);
    return mFactory->Group();
  }

  const nsCString&
  Origin() const
  {
    MOZ_ASSERT(mFactory);
    return mFactory->Origin();
  }

  StoragePrivilege
  Privilege() const
  {
    MOZ_ASSERT(mFactory);
    return mFactory->Privilege();
  }

  PersistenceType
  Type() const
  {
    MOZ_ASSERT(mMetadata);
    return mMetadata->mCommonMetadata.persistenceType();
  }

  const nsCString&
  Id() const
  {
    MOZ_ASSERT(mMetadata);
    return mMetadata->mDatabaseId;
  }

  const nsString&
  FilePath() const
  {
    MOZ_ASSERT(mMetadata);
    return mMetadata->mFilePath;
  }

  FileManager*
  Manager() const
  {
    return mFileManager;
  }

  FullDatabaseMetadata*
  Metadata() const
  {
    return mMetadata;
  }

private:
  // Reference counted.
  ~Database()
  {
    MOZ_ASSERT(mClosed);
  }

  bool
  CloseInternal();

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual PBackgroundIDBTransactionParent*
  AllocPBackgroundIDBTransactionParent(
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    const Mode& aMode)
                                    MOZ_OVERRIDE;

  virtual bool
  RecvPBackgroundIDBTransactionConstructor(
                                    PBackgroundIDBTransactionParent* aActor,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    const Mode& aMode)
                                    MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBTransactionParent(
                                        PBackgroundIDBTransactionParent* aActor)
                                        MOZ_OVERRIDE;

  virtual PBackgroundIDBVersionChangeTransactionParent*
  AllocPBackgroundIDBVersionChangeTransactionParent(
                                              const uint64_t& aCurrentVersion,
                                              const uint64_t& aRequestedVersion,
                                              const int64_t& aNextObjectStoreId,
                                              const int64_t& aNextIndexId)
                                              MOZ_OVERRIDE;

  virtual bool
  DeallocPBackgroundIDBVersionChangeTransactionParent(
                           PBackgroundIDBVersionChangeTransactionParent* aActor)
                           MOZ_OVERRIDE;

  virtual bool
  RecvDeleteMe() MOZ_OVERRIDE;

  virtual bool
  RecvBlocked() MOZ_OVERRIDE;

  virtual bool
  RecvClose() MOZ_OVERRIDE;
};

class TransactionBase
{
  class UpdateRefcountFunction;

  class CommitOp;
  friend class CommitOp;

public:
  class CachedStatement;

protected:
  typedef IDBTransaction::Mode Mode;

  nsRefPtr<Database> mDatabase;
  nsCOMPtr<mozIStorageConnection> mConnection;
  nsRefPtr<UpdateRefcountFunction> mUpdateFileRefcountFunction;
  nsInterfaceHashtable<nsCStringHashKey, mozIStorageStatement>
    mCachedStatements;
  nsTArray<FullObjectStoreMetadata*>
    mModifiedAutoIncrementObjectStoreMetadataArray;
  const uint64_t mTransactionId;
  Atomic<bool> mActorDestroyed;
  Mode mMode;
  bool mCommittedOrAborted;

#ifdef DEBUG
  nsCOMPtr<nsIEventTarget> mTransactionThread;
#endif

public:
  void
  AssertIsOnTransactionThread() const
  {
#ifdef DEBUG
    MOZ_ASSERT(mTransactionThread);

    bool current;
    MOZ_ASSERT(NS_SUCCEEDED(mTransactionThread->IsOnCurrentThread(&current)));
    MOZ_ASSERT(current);
#endif
  }

  bool
  IsActorDestroyed() const
  {
    AssertIsOnBackgroundThread();
    return mActorDestroyed;
  }

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(TransactionBase)

  nsresult
  GetCachedStatement(const nsACString& aQuery,
                     CachedStatement* aCachedStatement);

  template<int N>
  nsresult
  GetCachedStatement(const char (&aQuery)[N],
                     CachedStatement* aCachedStatement)
  {
    return GetCachedStatement(NS_LITERAL_CSTRING(aQuery), aCachedStatement);
  }

  nsresult
  EnsureConnection();

  void
  Abort(nsresult aResultCode);

  mozIStorageConnection*
  Connection() const
  {
    AssertIsOnTransactionThread();
    MOZ_ASSERT(mConnection);
    return mConnection;
  }

  uint64_t
  TransactionId() const
  {
    return mTransactionId;
  }

  Mode
  GetMode() const
  {
    return mMode;
  }

  Database*
  GetDatabase() const
  {
    AssertIsOnBackgroundThread();
    MOZ_ASSERT(mDatabase);
    return mDatabase;
  }

protected:
  TransactionBase(Database* aDatabase,
                  Mode aMode);

  virtual
  ~TransactionBase();

  void
  NoteActorDestroyed()
  {
    AssertIsOnBackgroundThread();
    MOZ_ASSERT(!mActorDestroyed);

    mActorDestroyed = true;
  }

  virtual void
  UpdateMetadata(nsresult aResult)
  { }

  virtual bool
  SendCompleteNotification(nsresult aResult) = 0;

  bool
  CommitOrAbort(nsresult aResultCode);

private:
  // Only called by CommitOp.
  void
  ReleaseTransactionThreadObjects();

  // Only called by CommitOp.
  void
  ReleaseBackgroundThreadObjects();
};

class OpenDatabaseOp;

class TransactionBase::UpdateRefcountFunction MOZ_FINAL
  : public mozIStorageFunction
{
  class FileInfoEntry
  {
    friend class UpdateRefcountFunction;

    nsRefPtr<FileInfo> mFileInfo;
    int32_t mDelta;

  public:
    FileInfoEntry(FileInfo* aFileInfo)
      : mFileInfo(aFileInfo)
      , mDelta(0)
    { }
  };

  enum UpdateType
  {
    eIncrement,
    eDecrement
  };

  class DatabaseUpdateFunction
  {
    nsCOMPtr<mozIStorageConnection> mConnection;
    nsCOMPtr<mozIStorageStatement> mUpdateStatement;
    nsCOMPtr<mozIStorageStatement> mSelectStatement;
    nsCOMPtr<mozIStorageStatement> mInsertStatement;

    UpdateRefcountFunction* mFunction;

    nsresult mErrorCode;

  public:
    DatabaseUpdateFunction(mozIStorageConnection* aConnection,
                           UpdateRefcountFunction* aFunction)
      : mConnection(aConnection)
      , mFunction(aFunction)
      , mErrorCode(NS_OK)
    { }

    bool
    Update(int64_t aId, int32_t aDelta);

    nsresult
    ErrorCode() const
    {
      return mErrorCode;
    }

  private:
    nsresult
    UpdateInternal(int64_t aId, int32_t aDelta);
  };

  FileManager* mFileManager;
  nsClassHashtable<nsUint64HashKey, FileInfoEntry> mFileInfoEntries;

  nsTArray<int64_t> mJournalsToCreateBeforeCommit;
  nsTArray<int64_t> mJournalsToRemoveAfterCommit;
  nsTArray<int64_t> mJournalsToRemoveAfterAbort;

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_MOZISTORAGEFUNCTION

  UpdateRefcountFunction(FileManager* aFileManager)
  : mFileManager(aFileManager)
  { }

  void
  ClearFileInfoEntries()
  {
    mFileInfoEntries.Clear();
  }

  nsresult
  WillCommit(mozIStorageConnection* aConnection);

  void
  DidCommit();

  void
  DidAbort();

private:
  ~UpdateRefcountFunction()
  { }

  nsresult
  ProcessValue(mozIStorageValueArray* aValues,
               int32_t aIndex,
               UpdateType aUpdateType);

  nsresult
  CreateJournals();

  nsresult
  RemoveJournals(const nsTArray<int64_t>& aJournals);

  static PLDHashOperator
  DatabaseUpdateCallback(const uint64_t& aKey,
                         FileInfoEntry* aValue,
                         void* aUserArg);

  static PLDHashOperator
  FileInfoUpdateCallback(const uint64_t& aKey,
                         FileInfoEntry* aValue,
                         void* aUserArg);
};

class TransactionBase::CommitOp MOZ_FINAL
  : public DatabaseOperationBase
  , public TransactionThreadPool::FinishCallback
{
  friend class TransactionBase;

  nsRefPtr<TransactionBase> mTransaction;
  nsresult mResultCode;

private:
  CommitOp(TransactionBase* aTransaction,
           nsresult aResultCode)
    : mTransaction(aTransaction)
    , mResultCode(aResultCode)
  {
    MOZ_ASSERT(aTransaction);
  }

  ~CommitOp()
  { }

  // Writes new autoIncrement counts to database.
  nsresult
  WriteAutoIncrementCounts();

  // Updates counts after a database activity has finished.
  void
  CommitOrRollbackAutoIncrementCounts();

  NS_DECL_NSIRUNNABLE

  virtual void
  TransactionFinishedBeforeUnblock() MOZ_OVERRIDE;

  virtual void
  TransactionFinishedAfterUnblock() MOZ_OVERRIDE;

public:
  void
  AssertIsOnTransactionThread() const
  {
    MOZ_ASSERT(mTransaction);
    mTransaction->AssertIsOnTransactionThread();
  }

  NS_DECL_ISUPPORTS_INHERITED
};

class TransactionBase::CachedStatement MOZ_FINAL
{
  friend class TransactionBase;

  nsCOMPtr<mozIStorageStatement> mStatement;
  Maybe<mozStorageStatementScoper> mScoper;

public:
  CachedStatement()
  { }

  ~CachedStatement()
  { }

  operator mozIStorageStatement*()
  {
    return mStatement;
  }

  mozIStorageStatement*
  operator->()
  {
    MOZ_ASSERT(mStatement);
    return mStatement;
  }

private:
  // Only called by TransactionBase.
  void
  Assign(already_AddRefed<mozIStorageStatement> aStatement)
  {
    mScoper.destroyIfConstructed();

    mStatement = aStatement;

    if (mStatement) {
      mScoper.construct(mStatement);
    }
  }

  // No funny business allowed.
  CachedStatement(const CachedStatement&) MOZ_DELETE;
  CachedStatement& operator=(const CachedStatement&) MOZ_DELETE;
};

class NormalTransaction MOZ_FINAL
  : public TransactionBase
  , public PBackgroundIDBTransactionParent
{
  friend class Database;

  nsTArray<FullObjectStoreMetadata*> mObjectStores;

private:
  // This constructor is only called by Database.
  NormalTransaction(Database* aDatabase,
                    nsTArray<FullObjectStoreMetadata*>& aObjectStores,
                    TransactionBase::Mode aMode);

  // Reference counted.
  ~NormalTransaction()
  { }

  // Only called by TransactionBase.
  virtual bool
  SendCompleteNotification(nsresult aResult) MOZ_OVERRIDE;

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual bool
  RecvDeleteMe() MOZ_OVERRIDE;

  virtual bool
  RecvCommit() MOZ_OVERRIDE;

  virtual bool
  RecvAbort(const nsresult& aResultCode) MOZ_OVERRIDE;
};

class VersionChangeTransaction MOZ_FINAL
  : public TransactionBase
  , public PBackgroundIDBVersionChangeTransactionParent
{
  nsRefPtr<OpenDatabaseOp> mOpenDatabaseOp;
  nsAutoPtr<FullDatabaseMetadata> mOldMetadata;

public:
  // This constructor is called by OpenDatabaseOp.
  VersionChangeTransaction(OpenDatabaseOp* aOpenDatabaseOp);

private:
  // Reference counted.
  ~VersionChangeTransaction();

  void
  CopyDatabaseMetadata();

  // Only called by TransactionBase.
  virtual void
  UpdateMetadata(nsresult aResult) MOZ_OVERRIDE;

  // Only called by TransactionBase.
  virtual bool
  SendCompleteNotification(nsresult aResult) MOZ_OVERRIDE;

  // IPDL methods are only called by IPDL.
  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

  virtual bool
  RecvDeleteMe() MOZ_OVERRIDE;

  virtual bool
  RecvCommit() MOZ_OVERRIDE;

  virtual bool
  RecvAbort(const nsresult& aResultCode) MOZ_OVERRIDE;

  virtual bool
  RecvCreateObjectStore(const ObjectStoreMetadata& aMetadata) MOZ_OVERRIDE;

  virtual bool
  RecvDeleteObjectStore(const int64_t& aObjectStoreId) MOZ_OVERRIDE;

  virtual bool
  RecvCreateIndex(const int64_t& aObjectStoreId,
                  const IndexMetadata& aMetadata) MOZ_OVERRIDE;

  virtual bool
  RecvDeleteIndex(const int64_t& aObjectStoreId,
                  const int64_t& aIndexId) MOZ_OVERRIDE;
};

class FactoryOp
  : public DatabaseOperationBase
  , public PBackgroundIDBFactoryRequestParent
{
protected:
  enum State
  {
    // Just created on the PBackground thread, dispatched to the main thread.
    // Next step is OpenPending.
    State_Initial,

    // Waiting for open allowed/open allowed on the main thread. Next step is
    // DatabaseWork.
    State_OpenPending,

    // Waiting to do/doing work on the QuotaManager IO thread. Next step is
    // either BeginVersionChange or SendingResults.
    State_DatabaseWork,

    // Starting a version change transaction. Need to notify other databases
    // that a version change is about to happen, and maybe tell the request
    // that a version change has been blocked. If databases are notified then
    // the next step is WaitingForOtherDatabasesToClose. Otherwise the next step
    // is DispatchToTransactionThreadPool.
    State_BeginVersionChange,

    // Waiting for other databases to close. This state may persist until all
    // databases are closed. If a database is blocked then the next state is
    // BlockedWaitingForOtherDatabasesToClose. If all databases close then the
    // next state is DispatchToTransactionThreadPool.
    State_WaitingForOtherDatabasesToClose,

    // Waiting for other databases to close after sending the blocked
    // notification. This state will  persist until all databases are closed.
    // Once all databases close then the next state is
    // DispatchToTransactionThreadPool.
    State_BlockedWaitingForOtherDatabasesToClose,

    // Waiting to be dispatched to the transaction thread pool. The next step is
    // DatabaseWorkVersionChange.
    State_DispatchToTransactionThreadPool,

    // Waiting to do/doing work on the transaction thread pool. Thereafter we
    // wait until the version change transaction commits. Next step is
    // SendingResults.
    State_DatabaseWorkVersionChange,

    // Waiting to send/sending results on the PBackground thread. Next step is
    // UnblockingQuotaManager.
    State_SendingResults,

    // Notifying the QuotaManager that it can proceed to the next operation.
    // Next step is Completed.
    State_UnblockingQuotaManager,

    // All done.
    State_Completed
  };

  State mState;
  nsCString mGroup;
  nsCString mOrigin;
  nsString mName;
  nsCString mDatabaseId;
  StoragePrivilege mPrivilege;
  PersistenceType mPersistenceType;

protected:
  FactoryOp(const nsACString& aGroup,
            const nsACString& aOrigin,
            StoragePrivilege aPrivilege,
            const nsAString& aName,
            PersistenceType aPersistenceType);

  virtual
  ~FactoryOp();

  virtual void
  ActorDestroy(ActorDestroyReason aWhy) MOZ_OVERRIDE;

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
  friend class Database;
  friend class VersionChangeTransaction;

  class VersionChangeOp;

  // mOwnedMetadata is owned by this class until it is transferred to
  // gLiveDatabaseHashtable. At that point mOwnedMetadata is set to null.
  // mMetadata always points to the metadata regardless of ownership.
  nsAutoPtr<FullDatabaseMetadata> mOwnedMetadata;
  FullDatabaseMetadata* mMetadata;

  uint64_t mRequestedVersion;
  nsString mDatabaseFilePath;
  nsRefPtr<FileManager> mFileManager;
  int64_t mLastObjectStoreId;
  int64_t mLastIndexId;

  nsRefPtr<Database> mDatabase;
  nsRefPtr<VersionChangeTransaction> mVersionChangeTransaction;
  nsTArray<Database*> mMaybeBlockedDatabases;

public:
  OpenDatabaseOp(const nsACString& aGroup,
                 const nsACString& aOrigin,
                 StoragePrivilege aPrivilege,
                 const OpenDatabaseRequestParams& aParams)
    : FactoryOp(aGroup, aOrigin, aPrivilege, aParams.metadata().name(),
                aParams.metadata().persistenceType())
    , mOwnedMetadata(new FullDatabaseMetadata())
    , mMetadata(mOwnedMetadata)
    , mRequestedVersion(aParams.metadata().version())
    , mLastObjectStoreId(0)
    , mLastIndexId(0)
  {
    mMetadata->mCommonMetadata = aParams.metadata();

    MOZ_ASSERT(!mDatabaseId.IsEmpty());
    mMetadata->mDatabaseId = mDatabaseId;
  }

private:
  ~OpenDatabaseOp()
  { }

  NS_DECL_NSIRUNNABLE

  nsresult
  DoDatabaseWork();

  nsresult
  BeginVersionChange();

  void
  NoteDatabaseDone(Database* aDatabase);

  void
  NoteDatabaseBlocked(Database* aDatabase);

  nsresult
  DispatchToTransactionThreadPool();

  nsresult
  DoDatabaseWorkVersionChange();

  void
  SendResults();

  nsresult
  EnsureDatabaseActor();

  void
  AssertMetadataConsistency(const FullDatabaseMetadata* aMetadata)
#ifdef DEBUG
  ;
#else
  { }
#endif
};

class OpenDatabaseOp::VersionChangeOp MOZ_FINAL
  : public CommonDatabaseOperationBase
{
  friend class OpenDatabaseOp;

  nsRefPtr<OpenDatabaseOp> mOpenDatabaseOp;

private:
  VersionChangeOp(OpenDatabaseOp* aOpenDatabaseOp)
    : CommonDatabaseOperationBase(aOpenDatabaseOp->mVersionChangeTransaction)
    , mOpenDatabaseOp(aOpenDatabaseOp)
  {
    MOZ_ASSERT(aOpenDatabaseOp);
  }

  ~VersionChangeOp()
  { }

  virtual nsresult
  DoDatabaseWork(TransactionBase* aTransaction) MOZ_OVERRIDE;

  virtual nsresult
  SendSuccessResult() MOZ_OVERRIDE;

  virtual bool
  SendFailureResult(nsresult aResultCode) MOZ_OVERRIDE;

  virtual void
  Cleanup() MOZ_OVERRIDE;
};

class DeleteDatabaseOp MOZ_FINAL
  : public FactoryOp
{
public:
  DeleteDatabaseOp(const nsACString& aGroup,
                   const nsACString& aOrigin,
                   StoragePrivilege aPrivilege,
                   const DeleteDatabaseRequestParams& aParams)
    : FactoryOp(aGroup, aOrigin, aPrivilege, aParams.metadata().name(),
                aParams.metadata().persistenceType())
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

class VersionChangeOp
  : public CommonDatabaseOperationBase
{
protected:
  VersionChangeOp(VersionChangeTransaction* aTransaction)
    : CommonDatabaseOperationBase(aTransaction)
  { }

  virtual
  ~VersionChangeOp()
  { }

private:
  virtual nsresult
  SendSuccessResult() MOZ_OVERRIDE;

  virtual bool
  SendFailureResult(nsresult aResultCode) MOZ_OVERRIDE;

  virtual void
  Cleanup() MOZ_OVERRIDE;
};

class CreateObjectStoreOp MOZ_FINAL
  : public VersionChangeOp
{
  friend class VersionChangeTransaction;

  const ObjectStoreMetadata mMetadata;

private:
  // Only created by VersionChangeTransaction.
  CreateObjectStoreOp(VersionChangeTransaction* aTransaction,
                      const ObjectStoreMetadata& aMetadata)
    : VersionChangeOp(aTransaction)
    , mMetadata(aMetadata)
  {
    MOZ_ASSERT(aMetadata.id());
  }

  ~CreateObjectStoreOp()
  { }

  virtual nsresult
  DoDatabaseWork(TransactionBase* aTransaction) MOZ_OVERRIDE;
};

class DeleteObjectStoreOp MOZ_FINAL
  : public VersionChangeOp
{
  friend class VersionChangeTransaction;

  const int64_t mId;

private:
  // Only created by VersionChangeTransaction.
  DeleteObjectStoreOp(VersionChangeTransaction* aTransaction,
                      const int64_t aId)
    : VersionChangeOp(aTransaction)
    , mId(aId)
  {
    MOZ_ASSERT(aId);
  }

  ~DeleteObjectStoreOp()
  { }

  virtual nsresult
  DoDatabaseWork(TransactionBase* aTransaction) MOZ_OVERRIDE;
};

class CreateIndexOp MOZ_FINAL
  : public VersionChangeOp
{
  friend class VersionChangeTransaction;

  const int64_t mObjectStoreId;
  const IndexMetadata mMetadata;

private:
  // Only created by VersionChangeTransaction.
  CreateIndexOp(VersionChangeTransaction* aTransaction,
                const int64_t aObjectStoreId,
                const IndexMetadata& aMetadata)
    : VersionChangeOp(aTransaction)
    , mObjectStoreId(aObjectStoreId)
    , mMetadata(aMetadata)
  {
    MOZ_ASSERT(aObjectStoreId);
    MOZ_ASSERT(aMetadata.id());
  }

  ~CreateIndexOp()
  { }

  nsresult
  InsertDataFromObjectStore(TransactionBase* aTransaction);

  virtual nsresult
  DoDatabaseWork(TransactionBase* aTransaction) MOZ_OVERRIDE;
};

class DeleteIndexOp MOZ_FINAL
  : public VersionChangeOp
{
  friend class VersionChangeTransaction;

  const int64_t mObjectStoreId;
  const int64_t mIndexId;

private:
  // Only created by VersionChangeTransaction.
  DeleteIndexOp(VersionChangeTransaction* aTransaction,
                const int64_t aObjectStoreId,
                const int64_t aIndexId)
    : VersionChangeOp(aTransaction)
    , mObjectStoreId(aObjectStoreId)
    , mIndexId(aIndexId)
  {
    MOZ_ASSERT(aObjectStoreId);
    MOZ_ASSERT(aIndexId);
  }

  ~DeleteIndexOp()
  { }

  virtual nsresult
  DoDatabaseWork(TransactionBase* aTransaction) MOZ_OVERRIDE;
};

} // anonymous namespace

/*******************************************************************************
 * Other class declarations
 ******************************************************************************/

namespace {

struct DatabaseActorInfo
{
  friend class nsAutoPtr<DatabaseActorInfo>;

  nsAutoPtr<FullDatabaseMetadata> mMetadata;
  nsTArray<Database*> mLiveDatabases;
  nsRefPtr<OpenDatabaseOp> mWaitingOpenOp;

  DatabaseActorInfo(FullDatabaseMetadata* aMetadata,
                    Database* aDatabase)
    : mMetadata(aMetadata)
  {
    MOZ_ASSERT(aDatabase);
    MOZ_COUNT_CTOR(DatabaseActorInfo);
    mLiveDatabases.AppendElement(aDatabase);
  }

private:
  ~DatabaseActorInfo()
  {
    MOZ_ASSERT(mLiveDatabases.IsEmpty());
    MOZ_ASSERT(!mWaitingOpenOp);
    MOZ_COUNT_DTOR(DatabaseActorInfo);
  }
};

} // anonymous namespace

/*******************************************************************************
 * Globals
 ******************************************************************************/

namespace {

// Maps a database id to information about live database actors.
typedef nsClassHashtable<nsCStringHashKey, DatabaseActorInfo>
        DatabaseActorHashtable;

StaticAutoPtr<DatabaseActorHashtable> gLiveDatabaseHashtable;

StaticRefPtr<nsRunnable> gStartTransactionRunnable;

} // anonymous namespace

/*******************************************************************************
 * BackgroundFactoryParent
 ******************************************************************************/

uint64_t BackgroundFactoryParent::sFactoryInstanceCount = 0;

// static
already_AddRefed<BackgroundFactoryParent>
BackgroundFactoryParent::Create(const nsCString& aGroup,
                                const nsCString& aOrigin,
                                const StoragePrivilege& aPrivilege)
{
  AssertIsOnBackgroundThread();

  // If this is the first instance then we need to do some initialization.
  if (!sFactoryInstanceCount) {
    if (NS_WARN_IF(!TransactionThreadPool::GetOrCreate())) {
      return nullptr;
    }

    MOZ_ASSERT(!gLiveDatabaseHashtable);
    gLiveDatabaseHashtable = new DatabaseActorHashtable();

    MOZ_ASSERT(!gStartTransactionRunnable);
    gStartTransactionRunnable = new nsRunnable();
  }

  nsRefPtr<BackgroundFactoryParent> actor =
    new BackgroundFactoryParent(aGroup, aOrigin, aPrivilege);

  sFactoryInstanceCount++;

  return actor.forget();
}

BackgroundFactoryParent::BackgroundFactoryParent(
                                             const nsCString& aGroup,
                                             const nsCString& aOrigin,
                                             const StoragePrivilege& aPrivilege)
  : mGroup(aGroup)
  , mOrigin(aOrigin)
  , mPrivilege(aPrivilege)
{
  AssertIsOnBackgroundThread();
}

BackgroundFactoryParent::~BackgroundFactoryParent()
{ }

void
BackgroundFactoryParent::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();

  // Clean up if there are no more instances.
  if (!(--sFactoryInstanceCount)) {
    TransactionThreadPool::Shutdown();

    MOZ_ASSERT(gStartTransactionRunnable);
    gStartTransactionRunnable = nullptr;

    MOZ_ASSERT(gLiveDatabaseHashtable);
    MOZ_ASSERT(!gLiveDatabaseHashtable->Count());
    gLiveDatabaseHashtable = nullptr;
  }
}

bool
BackgroundFactoryParent::RecvDeleteMe()
{
  AssertIsOnBackgroundThread();

  return Send__delete__(this);
}

PBackgroundIDBFactoryRequestParent*
BackgroundFactoryParent::AllocPBackgroundIDBFactoryRequestParent(
                                            const FactoryRequestParams& aParams)
{
  AssertIsOnBackgroundThread();

  nsRefPtr<FactoryOp> op;

  switch (aParams.type()) {
    case FactoryRequestParams::TOpenDatabaseRequestParams: {
      const OpenDatabaseRequestParams& params =
         aParams.get_OpenDatabaseRequestParams();
      const DatabaseMetadata& metadata = params.metadata();

      if (NS_WARN_IF(
            metadata.persistenceType() != PERSISTENCE_TYPE_PERSISTENT &&
            metadata.persistenceType() != PERSISTENCE_TYPE_TEMPORARY)) {
        return nullptr;
      }

      op = new OpenDatabaseOp(mGroup, mOrigin, mPrivilege, params);
      break;
    }

    case FactoryRequestParams::TDeleteDatabaseRequestParams: {
      const DeleteDatabaseRequestParams& params =
         aParams.get_DeleteDatabaseRequestParams();
      const DatabaseMetadata& metadata = params.metadata();

      if (NS_WARN_IF(metadata.version())) {
        return nullptr;
      }

      if (NS_WARN_IF(
            metadata.persistenceType() != PERSISTENCE_TYPE_PERSISTENT &&
            metadata.persistenceType() != PERSISTENCE_TYPE_TEMPORARY)) {
        return nullptr;
      }

      op = new DeleteDatabaseOp(mGroup, mOrigin, mPrivilege, params);
      break;
    }

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
  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(op)));
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
                                   const DatabaseSpec& aSpec,
                                   PBackgroundIDBFactoryRequestParent* aRequest)
{
  MOZ_CRASH("PBackgroundIDBDatabaseParent actors should be constructed "
            "manually!");
}

bool
BackgroundFactoryParent::DeallocPBackgroundIDBDatabaseParent(
                                           PBackgroundIDBDatabaseParent* aActor)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  nsRefPtr<Database> database = dont_AddRef(static_cast<Database*>(aActor));
  return true;
}

/*******************************************************************************
 * Database
 ******************************************************************************/

Database::Database(BackgroundFactoryParent* aFactory,
                   FullDatabaseMetadata* aMetadata,
                   FileManager* aFileManager)
  : mFactory(aFactory)
  , mMetadata(aMetadata)
  , mFileManager(aFileManager)
  , mClosed(false)
  , mInvalidated(false)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aFactory);
  MOZ_ASSERT(aMetadata);
  MOZ_ASSERT(aFileManager);
}

void
Database::Invalidate()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!mInvalidated);

  mInvalidated = true;

  unused << SendInvalidate();

  MOZ_ALWAYS_TRUE(CloseInternal());
}

bool
Database::CloseInternal()
{
  AssertIsOnBackgroundThread();

  if (mClosed) {
    if (NS_WARN_IF(!mInvalidated)) {
      // Kill misbehaving child.
      return false;
    }

    // Ignore harmless race.
    return true;
  }

  mClosed = true;

  DatabaseActorInfo* info;
  MOZ_ALWAYS_TRUE(gLiveDatabaseHashtable->Get(Id(), &info));

  MOZ_ASSERT(info->mLiveDatabases.Contains(this));

  if (info->mWaitingOpenOp) {
    info->mWaitingOpenOp->NoteDatabaseDone(this);
  }

  MOZ_ALWAYS_TRUE(info->mLiveDatabases.RemoveElement(this));

  MOZ_ASSERT(!info->mLiveDatabases.Contains(this));

  if (info->mLiveDatabases.IsEmpty()) {
    MOZ_ASSERT(!info->mWaitingOpenOp);
    gLiveDatabaseHashtable->Remove(Id());
  }

  mMetadata = nullptr;

  return true;
}

void
Database::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();

  if (!mClosed) {
    MOZ_ALWAYS_TRUE(CloseInternal());
  }
}

PBackgroundIDBTransactionParent*
Database::AllocPBackgroundIDBTransactionParent(
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    const Mode& aMode)
{
  AssertIsOnBackgroundThread();

  if (NS_WARN_IF(aObjectStoreNames.IsEmpty())) {
    return nullptr;
  }

  if (NS_WARN_IF(aMode != IDBTransaction::READ_ONLY &&
                 aMode != IDBTransaction::READ_WRITE)) {
    return nullptr;
  }

  const nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& objectStores =
    mMetadata->mObjectStores;
  const uint32_t objectStoreCount = objectStores.Length();
  const uint32_t nameCount = aObjectStoreNames.Length();

  if (NS_WARN_IF(nameCount > objectStoreCount)) {
    return nullptr;
  }

  FallibleTArray<FullObjectStoreMetadata*> fallibleObjectStores;
  if (NS_WARN_IF(
        !fallibleObjectStores.SetCapacity(nameCount))) {
    return nullptr;
  }

  for (uint32_t nameIndex = 0; nameIndex < nameCount; nameIndex++) {
    const nsString& name = aObjectStoreNames[nameIndex];

    for (uint32_t objIndex = 0; objIndex < objectStoreCount; objIndex++) {
      const nsAutoPtr<FullObjectStoreMetadata>& metadata =
        objectStores[objIndex];
      if (metadata->mCommonMetadata.name() == name) {
        MOZ_ALWAYS_TRUE(fallibleObjectStores.AppendElement(metadata));
        break;
      }
    }
  }

  nsTArray<FullObjectStoreMetadata*> infallibleObjectStores;
  infallibleObjectStores.SwapElements(fallibleObjectStores);

  nsRefPtr<NormalTransaction> transaction =
    new NormalTransaction(this, infallibleObjectStores, aMode);

  MOZ_ASSERT(infallibleObjectStores.IsEmpty());

  return transaction.forget().get();
}

bool
Database::RecvPBackgroundIDBTransactionConstructor(
                                    PBackgroundIDBTransactionParent* aActor,
                                    const nsTArray<nsString>& aObjectStoreNames,
                                    const Mode& aMode)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  auto transaction = static_cast<NormalTransaction*>(aActor);

  TransactionThreadPool* threadPool = TransactionThreadPool::GetOrCreate();
  if (!threadPool) {
    transaction->Abort(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return true;
  }

  // Add a placeholder for this transaction immediately.
  nsresult rv = threadPool->Dispatch(transaction->TransactionId(),
                                     mMetadata->mDatabaseId, aObjectStoreNames,
                                     aMode, gStartTransactionRunnable, false,
                                     nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    transaction->Abort(NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR);
    return true;
  }

  return true;
}

bool
Database::DeallocPBackgroundIDBTransactionParent(
                                        PBackgroundIDBTransactionParent* aActor)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  nsRefPtr<NormalTransaction> transaction =
    dont_AddRef(static_cast<NormalTransaction*>(aActor));
  return true;
}

PBackgroundIDBVersionChangeTransactionParent*
Database::AllocPBackgroundIDBVersionChangeTransactionParent(
                                              const uint64_t& aCurrentVersion,
                                              const uint64_t& aRequestedVersion,
                                              const int64_t& aNextObjectStoreId,
                                              const int64_t& aNextIndexId)
{
  MOZ_CRASH("PBackgroundIDBVersionChangeTransactionParent actors should be "
            "constructed manually!");
}

bool
Database::DeallocPBackgroundIDBVersionChangeTransactionParent(
                           PBackgroundIDBVersionChangeTransactionParent* aActor)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aActor);

  nsRefPtr<VersionChangeTransaction> transaction =
    dont_AddRef(static_cast<VersionChangeTransaction*>(aActor));
  return true;
}

bool
Database::RecvDeleteMe()
{
  AssertIsOnBackgroundThread();

  return Send__delete__(this);
}

bool
Database::RecvBlocked()
{
  AssertIsOnBackgroundThread();

  DatabaseActorInfo* info;
  MOZ_ALWAYS_TRUE(gLiveDatabaseHashtable->Get(Id(), &info));

  MOZ_ASSERT(info->mLiveDatabases.Contains(this));
  MOZ_ASSERT(info->mWaitingOpenOp);

  info->mWaitingOpenOp->NoteDatabaseBlocked(this);

  return true;
}

bool
Database::RecvClose()
{
  AssertIsOnBackgroundThread();

  if (NS_WARN_IF(!CloseInternal())) {
    return false;
  }

  return true;
}

/*******************************************************************************
 * TransactionBase
 ******************************************************************************/

TransactionBase::TransactionBase(Database* aDatabase,
                                 IDBTransaction::Mode aMode)
  : mDatabase(aDatabase)
  , mTransactionId(TransactionThreadPool::NextTransactionId())
  , mActorDestroyed(false)
  , mMode(aMode)
  , mCommittedOrAborted(false)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aDatabase);
}

TransactionBase::~TransactionBase()
{
}

nsresult
TransactionBase::EnsureConnection()
{
#ifdef DEBUG
  if (!mTransactionThread) {
    mTransactionThread = NS_GetCurrentThread();
    MOZ_ASSERT(mTransactionThread);
  }
#endif

  AssertIsOnTransactionThread();

  PROFILER_LABEL("IndexedDB", "TransactionBase::EnsureConnection");

  if (!mConnection) {
    nsCOMPtr<mozIStorageConnection> connection;
    nsresult rv =
      GetDatabaseConnection(mDatabase->FilePath(), mDatabase->Type(),
                            mDatabase->Group(), mDatabase->Origin(),
                            getter_AddRefs(connection));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    nsRefPtr<UpdateRefcountFunction> function;
    nsCString beginTransaction;

    if (mMode == IDBTransaction::READ_ONLY) {
      beginTransaction.AssignLiteral("BEGIN TRANSACTION;");
    } else {
      function = new UpdateRefcountFunction(mDatabase->Manager());

      rv = connection->CreateFunction(NS_LITERAL_CSTRING("update_refcount"), 2,
                                      function);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      beginTransaction.AssignLiteral("BEGIN IMMEDIATE TRANSACTION;");
    }

    nsCOMPtr<mozIStorageStatement> stmt;
    rv = connection->CreateStatement(beginTransaction, getter_AddRefs(stmt));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    rv = stmt->Execute();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    function.swap(mUpdateFileRefcountFunction);
    connection.swap(mConnection);
  }

  return NS_OK;
}

void
TransactionBase::Abort(nsresult aResultCode)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(NS_FAILED(aResultCode));

  CommitOrAbort(aResultCode);
}

bool
TransactionBase::CommitOrAbort(nsresult aResultCode)
{
  AssertIsOnBackgroundThread();

  if (mCommittedOrAborted) {
    return false;
  }

  mCommittedOrAborted = true;

  nsRefPtr<CommitOp> commitOp =
    new CommitOp(this, ClampResultCode(aResultCode));

  TransactionThreadPool* threadPool = TransactionThreadPool::Get();
  MOZ_ASSERT(threadPool);

  threadPool->Dispatch(TransactionId(), GetDatabase()->Id(), commitOp, true,
                       commitOp);
  return true;
}

nsresult
TransactionBase::GetCachedStatement(const nsACString& aQuery,
                                    CachedStatement* aCachedStatement)
{
  AssertIsOnTransactionThread();
  MOZ_ASSERT(!aQuery.IsEmpty());
  MOZ_ASSERT(aCachedStatement);
  MOZ_ASSERT(mConnection);

  nsCOMPtr<mozIStorageStatement> stmt;

  if (!mCachedStatements.Get(aQuery, getter_AddRefs(stmt))) {
    nsresult rv = mConnection->CreateStatement(aQuery, getter_AddRefs(stmt));
    if (NS_FAILED(rv)) {
#ifdef DEBUG
      nsCString msg;
      MOZ_ALWAYS_TRUE(NS_SUCCEEDED(mConnection->GetLastErrorString(msg)));

      nsAutoCString error =
        NS_LITERAL_CSTRING("The statement '") + aQuery +
        NS_LITERAL_CSTRING("' failed to compile with the error message '") +
        msg + NS_LITERAL_CSTRING("'.");

      NS_WARNING(error.get());
#endif
      return rv;
    }

    mCachedStatements.Put(aQuery, stmt);
  }

  aCachedStatement->Assign(stmt.forget());
  return NS_OK;
}

void
TransactionBase::ReleaseTransactionThreadObjects()
{
  AssertIsOnTransactionThread();

  mCachedStatements.Clear();

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(mConnection->Close()));
  mConnection = nullptr;
}

void
TransactionBase::ReleaseBackgroundThreadObjects()
{
  AssertIsOnBackgroundThread();

  if (mUpdateFileRefcountFunction) {
    mUpdateFileRefcountFunction->ClearFileInfoEntries();
    mUpdateFileRefcountFunction = nullptr;
  }
}

/*******************************************************************************
 * NormalTransaction
 ******************************************************************************/

NormalTransaction::NormalTransaction(
                              Database* aDatabase,
                              nsTArray<FullObjectStoreMetadata*>& aObjectStores,
                              TransactionBase::Mode aMode)
  : TransactionBase(aDatabase, aMode)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!aObjectStores.IsEmpty());

  mObjectStores.SwapElements(aObjectStores);
}

bool
NormalTransaction::SendCompleteNotification(nsresult aResult)
{
  AssertIsOnBackgroundThread();

  return IsActorDestroyed() ? true : SendComplete(aResult);
}

void
NormalTransaction::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();

  CommitOrAbort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR);

  NoteActorDestroyed();
}

bool
NormalTransaction::RecvDeleteMe()
{
  AssertIsOnBackgroundThread();

  return Send__delete__(this);
}

bool
NormalTransaction::RecvCommit()
{
  AssertIsOnBackgroundThread();

  bool result = CommitOrAbort(NS_OK);
  MOZ_ASSERT(result);

  return result;
}

bool
NormalTransaction::RecvAbort(const nsresult& aResultCode)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(NS_FAILED(aResultCode));

  if (NS_SUCCEEDED(aResultCode)) {
    return false;
  }

  bool result = CommitOrAbort(aResultCode);
  MOZ_ASSERT(result);

  return result;
}

/*******************************************************************************
 * VersionChangeTransaction
 ******************************************************************************/

VersionChangeTransaction::VersionChangeTransaction(
                                                OpenDatabaseOp* aOpenDatabaseOp)
  : TransactionBase(aOpenDatabaseOp->mDatabase, IDBTransaction::VERSION_CHANGE)
  , mOpenDatabaseOp(aOpenDatabaseOp)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(aOpenDatabaseOp);

  CopyDatabaseMetadata();
}

VersionChangeTransaction::~VersionChangeTransaction()
{
}

void
VersionChangeTransaction::CopyDatabaseMetadata()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(!mOldMetadata);

  const FullDatabaseMetadata* origDBMetadata = mDatabase->Metadata();
  MOZ_ASSERT(origDBMetadata);

  // FullDatabaseMetadata contains a nsTArray of pointers that we need to
  // duplicate so we can't just use the copy constructor.
  mOldMetadata = new FullDatabaseMetadata();

  mOldMetadata->mCommonMetadata = origDBMetadata->mCommonMetadata;
  mOldMetadata->mDatabaseId = origDBMetadata->mDatabaseId;
  mOldMetadata->mFilePath = origDBMetadata->mFilePath;
  mOldMetadata->mNextObjectStoreId = origDBMetadata->mNextObjectStoreId;
  mOldMetadata->mNextIndexId = origDBMetadata->mNextIndexId;

  const nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& origObjectStores =
    origDBMetadata->mObjectStores;

  for (uint32_t objCount = origObjectStores.Length(), objIndex = 0;
       objIndex < objCount;
       objIndex++) {
    const nsAutoPtr<FullObjectStoreMetadata>& origObjMetadata =
      origObjectStores[objIndex];

    // FullObjectStoreMetadata contains a nsTArray of pointers that we need to
    // duplicate so we can't just use the copy constructor.
    nsAutoPtr<FullObjectStoreMetadata> oldObjMetadata(
      new FullObjectStoreMetadata());

    oldObjMetadata->mCommonMetadata = origObjMetadata->mCommonMetadata;
    oldObjMetadata->mNextAutoIncrementId =
      origObjMetadata->mNextAutoIncrementId;
    oldObjMetadata->mComittedAutoIncrementId =
      origObjMetadata->mComittedAutoIncrementId;

    for (uint32_t idxCount = origObjMetadata->mIndexes.Length(), idxIndex = 0;
         idxIndex < idxCount;
         idxIndex++) {
    const nsAutoPtr<FullIndexMetadata>& origIdxMetadata =
      origObjMetadata->mIndexes[idxIndex];

      // FullIndexMetadata doesn't contain any non-copyable pointers so we can 
      // just use the copy constructor.
      oldObjMetadata->mIndexes.AppendElement(
        new FullIndexMetadata(*origIdxMetadata));
    }

    mOldMetadata->mObjectStores.AppendElement(oldObjMetadata.forget());
  }
}

void
VersionChangeTransaction::UpdateMetadata(nsresult aResult)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(mDatabase);
  MOZ_ASSERT(mOpenDatabaseOp);
  MOZ_ASSERT(mOpenDatabaseOp->mDatabase);
  MOZ_ASSERT(!mOpenDatabaseOp->mDatabaseId.IsEmpty());
  MOZ_ASSERT(mOldMetadata);

  nsAutoPtr<FullDatabaseMetadata> oldMetadata = mOldMetadata.forget();

  DatabaseActorInfo* info = nullptr;
  gLiveDatabaseHashtable->Get(oldMetadata->mDatabaseId, &info);

  if (NS_SUCCEEDED(aResult)) {
    MOZ_ASSERT(info);
    MOZ_ASSERT(!info->mLiveDatabases.IsEmpty());

    // Take care of any deleted objectStores or indexes.
    FullDatabaseMetadata* dbMetadata = info->mMetadata;

    for (uint32_t objCount = dbMetadata->mObjectStores.Length(), objIndex = 0;
         objIndex < objCount;
         /* incremented conditionally */) {
      FullObjectStoreMetadata* objMetadata =
        dbMetadata->mObjectStores[objIndex];

      if (objMetadata->mDeleted) {
        dbMetadata->mObjectStores.RemoveElementAt(objIndex);
        objCount--;
        continue;
      }

      objIndex++;

      for (uint32_t idxCount = objMetadata->mIndexes.Length(), idxIndex = 0;
           idxIndex < idxCount;
           /* incremented conditionally */) {
        if (objMetadata->mIndexes[idxIndex]->mDeleted) {
          objMetadata->mIndexes.RemoveElementAt(idxIndex);
          idxCount--;
          continue;
        }

        idxIndex++;
      }
    }

    return;
  }

  if (info) {
    for (uint32_t count = info->mLiveDatabases.Length(), index = 0;
          index < count;
          index++) {
      info->mLiveDatabases[index]->mMetadata = oldMetadata;
    }

    info->mMetadata = oldMetadata.forget();
    return;
  }
}

bool
VersionChangeTransaction::SendCompleteNotification(nsresult aResult)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(mOpenDatabaseOp);

  nsRefPtr<OpenDatabaseOp> openDatabaseOp;
  mOpenDatabaseOp.swap(openDatabaseOp);

  if (!IsActorDestroyed()) {
    if (NS_FAILED(aResult) && NS_SUCCEEDED(openDatabaseOp->mResultCode)) {
      openDatabaseOp->mResultCode = aResult;
    }

    openDatabaseOp->mState = OpenDatabaseOp::State_SendingResults;

    bool result = SendComplete(aResult);

    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(openDatabaseOp->Run()));

    return result;
  }

  return true;
}

void
VersionChangeTransaction::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();

  CommitOrAbort(NS_ERROR_DOM_INDEXEDDB_ABORT_ERR);

  NoteActorDestroyed();
}

bool
VersionChangeTransaction::RecvDeleteMe()
{
  AssertIsOnBackgroundThread();

  return Send__delete__(this);
}

bool
VersionChangeTransaction::RecvCommit()
{
  AssertIsOnBackgroundThread();

  bool result = CommitOrAbort(NS_OK);
  MOZ_ASSERT(result);

  return result;
}

bool
VersionChangeTransaction::RecvAbort(const nsresult& aResultCode)
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(NS_FAILED(aResultCode));

  if (NS_SUCCEEDED(aResultCode)) {
    return false;
  }

  bool result = CommitOrAbort(aResultCode);
  MOZ_ASSERT(result);

  return result;
}

bool
VersionChangeTransaction::RecvCreateObjectStore(
                                           const ObjectStoreMetadata& aMetadata)
{
  AssertIsOnBackgroundThread();

  if (!aMetadata.id()) {
    return false;
  }

  FullDatabaseMetadata* dbMetadata = mDatabase->Metadata();
  MOZ_ASSERT(dbMetadata);

  if (aMetadata.id() != dbMetadata->mNextObjectStoreId) {
    return false;
  }

  nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& objectStores =
    dbMetadata->mObjectStores;

  for (uint32_t count = objectStores.Length(), index = 0;
       index < count;
       index++) {
    const FullObjectStoreMetadata* metadata = objectStores[index];

    MOZ_ASSERT(metadata->mCommonMetadata.id() != aMetadata.id());

    if (aMetadata.name() == metadata->mCommonMetadata.name() &&
        !metadata->mDeleted) {
      return false;
    }
  }

  nsAutoPtr<FullObjectStoreMetadata> newMetadata(new FullObjectStoreMetadata());
  newMetadata->mCommonMetadata = aMetadata;
  newMetadata->mNextAutoIncrementId = aMetadata.autoIncrement() ? 1 : 0;
  newMetadata->mComittedAutoIncrementId = newMetadata->mNextAutoIncrementId;

  objectStores.AppendElement(newMetadata.forget());
  dbMetadata->mNextObjectStoreId++;

  nsRefPtr<CreateObjectStoreOp> op = new CreateObjectStoreOp(this, aMetadata);
  op->DispatchToTransactionThreadPool();

  return true;
}

bool
VersionChangeTransaction::RecvDeleteObjectStore(const int64_t& aObjectStoreId)
{
  AssertIsOnBackgroundThread();

  if (!aObjectStoreId) {
    return false;
  }

  FullDatabaseMetadata* dbMetadata = mDatabase->Metadata();
  MOZ_ASSERT(dbMetadata);

  if (aObjectStoreId >= dbMetadata->mNextObjectStoreId) {
    return false;
  }

  nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& objectStores =
    dbMetadata->mObjectStores;

  bool valid = false;

  for (uint32_t count = objectStores.Length(), index = 0;
       index < count;
       index++) {
    nsAutoPtr<FullObjectStoreMetadata>& metadata = objectStores[index];

    if (aObjectStoreId == metadata->mCommonMetadata.id() &&
        !metadata->mDeleted) {
      // This metadata entry will actually be deleted when the transaction
      // commits.
      metadata->mDeleted = true;
      valid = true;
      break;
    }
  }

  if (!valid) {
    return false;
  }

  nsRefPtr<DeleteObjectStoreOp> op =
    new DeleteObjectStoreOp(this, aObjectStoreId);
  op->DispatchToTransactionThreadPool();

  return true;
}

bool
VersionChangeTransaction::RecvCreateIndex(const int64_t& aObjectStoreId,
                                          const IndexMetadata& aMetadata)
{
  AssertIsOnBackgroundThread();

  if (!aObjectStoreId || !aMetadata.id()) {
    return false;
  }

  FullDatabaseMetadata* dbMetadata = mDatabase->Metadata();
  MOZ_ASSERT(dbMetadata);

  if (aMetadata.id() != dbMetadata->mNextIndexId) {
    return false;
  }

  nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& objectStores =
    dbMetadata->mObjectStores;

  FullObjectStoreMetadata* foundObjectStoreMetadata = nullptr;

  for (uint32_t objCount = objectStores.Length(), objIndex = 0;
       objIndex < objCount;
       objIndex++) {
    FullObjectStoreMetadata* objMetadata = objectStores[objIndex];

    if (aObjectStoreId == objMetadata->mCommonMetadata.id() &&
        !objMetadata->mDeleted) {
      nsTArray<nsAutoPtr<FullIndexMetadata>>& indexes = objMetadata->mIndexes;

      for (uint32_t idxCount = indexes.Length(), idxIndex = 0;
           idxIndex < idxCount;
           idxIndex++) {
        const FullIndexMetadata* idxMetadata = indexes[idxIndex];

        MOZ_ASSERT(idxMetadata->mCommonMetadata.id() != aMetadata.id());

        if (aMetadata.name() == idxMetadata->mCommonMetadata.name() &&
            !idxMetadata->mDeleted) {
          return false;
        }
      }

      foundObjectStoreMetadata = objMetadata;
      break;
    }
  }

  if (!foundObjectStoreMetadata) {
    return false;
  }

  nsAutoPtr<FullIndexMetadata> newMetadata(new FullIndexMetadata());
  newMetadata->mCommonMetadata = aMetadata;

  foundObjectStoreMetadata->mIndexes.AppendElement(newMetadata.forget());
  dbMetadata->mNextIndexId++;

  nsRefPtr<CreateIndexOp> op =
    new CreateIndexOp(this, aObjectStoreId, aMetadata);
  op->DispatchToTransactionThreadPool();

  return true;
}

bool
VersionChangeTransaction::RecvDeleteIndex(const int64_t& aObjectStoreId,
                                          const int64_t& aIndexId)
{
  AssertIsOnBackgroundThread();

  if (!aObjectStoreId || !aIndexId) {
    return false;
  }

  FullDatabaseMetadata* dbMetadata = mDatabase->Metadata();
  MOZ_ASSERT(dbMetadata);

  if (aObjectStoreId >= dbMetadata->mNextObjectStoreId ||
      aIndexId >= dbMetadata->mNextIndexId) {
    return false;
  }

  nsTArray<nsAutoPtr<FullObjectStoreMetadata>>& objectStores =
    dbMetadata->mObjectStores;

  bool valid = false;

  for (uint32_t objCount = objectStores.Length(), objIndex = 0;
       objIndex < objCount;
       objIndex++) {
    nsAutoPtr<FullObjectStoreMetadata>& objMetadata = objectStores[objIndex];

    if (aObjectStoreId == objMetadata->mCommonMetadata.id() &&
        !objMetadata->mDeleted) {
      nsTArray<nsAutoPtr<FullIndexMetadata>>& indexes = objMetadata->mIndexes;

      for (uint32_t idxCount = indexes.Length(), idxIndex = 0;
           idxIndex < idxCount;
           idxIndex++) {
        nsAutoPtr<FullIndexMetadata>& idxMetadata = indexes[idxIndex];

        if (aIndexId == idxMetadata->mCommonMetadata.id() &&
            !idxMetadata->mDeleted) {
          // This metadata entry will actually be deleted when the transaction
          // commits.
          idxMetadata->mDeleted = true;
          valid = true;
          break;
        }
      }
    }
  }

  if (!valid) {
    return false;
  }

  nsRefPtr<DeleteIndexOp> op =
    new DeleteIndexOp(this, aObjectStoreId, aIndexId);
  op->DispatchToTransactionThreadPool();

  return true;
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
  *_retval = mActorDestroyed;
  return NS_OK;
}

FactoryOp::FactoryOp(const nsACString& aGroup,
                     const nsACString& aOrigin,
                     StoragePrivilege aPrivilege,
                     const nsAString& aName,
                     PersistenceType aPersistenceType)
  : mState(State_Initial)
  , mGroup(aGroup)
  , mOrigin(aOrigin)
  , mName(aName)
  , mPrivilege(aPrivilege)
  , mPersistenceType(aPersistenceType)
{
  QuotaManager::GetStorageId(aPersistenceType, aOrigin, Client::IDB, aName,
                             mDatabaseId);
  MOZ_ASSERT(!mDatabaseId.IsEmpty());
}

FactoryOp::~FactoryOp()
{
  MOZ_ASSERT_IF(!mActorDestroyed,
                mState == State_Initial || mState == State_Completed);
}

void
FactoryOp::ActorDestroy(ActorDestroyReason aWhy)
{
  AssertIsOnBackgroundThread();

  NoteActorDestroyed();
}

nsresult
FactoryOp::Open()
{
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(mState == State_Initial);

  if (mActorDestroyed) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  QuotaManager* quotaManager = QuotaManager::GetOrCreate();
  if (NS_WARN_IF(!quotaManager)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  nsresult rv = quotaManager->
    WaitForOpenAllowed(OriginOrPatternString::FromOrigin(mOrigin),
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

  if (mActorDestroyed) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  QuotaManager* quotaManager = QuotaManager::Get();
  if (NS_WARN_IF(!quotaManager)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  // Must set this before dispatching otherwise we will race with the IO thread.
  mState = State_DatabaseWork;

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
      AllowNextSynchronizedOp(OriginOrPatternString::FromOrigin(mOrigin),
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

    case State_DatabaseWork:
      rv = DoDatabaseWork();
      break;

    case State_BeginVersionChange:
      rv = BeginVersionChange();
      break;

    case State_DispatchToTransactionThreadPool:
      rv = DispatchToTransactionThreadPool();
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

  if (NS_WARN_IF(NS_FAILED(rv)) && mState != State_SendingResults) {
    if (NS_SUCCEEDED(mResultCode)) {
      mResultCode = rv;
    }

    // Must set mState before dispatching otherwise we will race with the owning
    // thread.
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
  MOZ_ASSERT(mState == State_DatabaseWork);

  PROFILER_LABEL("IndexedDB", "OpenDatabaseHelper::DoDatabaseWork");

  if (NS_WARN_IF(QuotaManager::IsShuttingDown()) ||
      mActorDestroyed) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  QuotaManager* quotaManager = QuotaManager::Get();
  MOZ_ASSERT(quotaManager);

  nsCOMPtr<nsIFile> dbDirectory;

  nsresult rv =
    quotaManager->EnsureOriginIsInitialized(mPersistenceType, mGroup,
                                            mOrigin, mPrivilege != Chrome,
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
                                mGroup, mOrigin,
                                getter_AddRefs(connection));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = LoadDatabaseInformation(connection, mMetadata);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  for (uint32_t objectStoreCount = mMetadata->mObjectStores.Length(), i = 0;
       i < objectStoreCount;
       i++) {
    const nsAutoPtr<FullObjectStoreMetadata>& objectStoreMetadata =
      mMetadata->mObjectStores[i];

    for (uint32_t indexCount = objectStoreMetadata->mIndexes.Length(), j = 0;
         j < indexCount;
         j++) {
      const nsAutoPtr<FullIndexMetadata>& indexMetadata =
        objectStoreMetadata->mIndexes[j];
      mLastIndexId = std::max(indexMetadata->mCommonMetadata.id(),
                              mLastIndexId);
    }

    mLastObjectStoreId =
      std::max(objectStoreMetadata->mCommonMetadata.id(), mLastObjectStoreId);
  }

  // See if we need to do a versionchange transaction

  // Optional version semantics.
  if (!mRequestedVersion) {
    // If the requested version was not specified and the database was created,
    // treat it as if version 1 were requested.
    if (mMetadata->mCommonMetadata.version() == 0) {
      mRequestedVersion = 1;
    } else {
      // Otherwise, treat it as if the current version were requested.
      mRequestedVersion = mMetadata->mCommonMetadata.version();
    }
  }

  if (mMetadata->mCommonMetadata.version() > mRequestedVersion) {
    return NS_ERROR_DOM_INDEXEDDB_VERSION_ERR;
  }

  IndexedDatabaseManager* mgr = IndexedDatabaseManager::Get();
  MOZ_ASSERT(mgr);

  nsRefPtr<FileManager> fileManager =
    mgr->GetFileManager(mPersistenceType, mOrigin, mName);
  if (!fileManager) {
    fileManager = new FileManager(mPersistenceType, mGroup, mOrigin, mPrivilege,
                                  mMetadata->mCommonMetadata.name());

    rv = fileManager->Init(fmDirectory, connection);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    mgr->AddFileManager(fileManager);
  }

  mFileManager = fileManager.forget();

  // Must set mState before dispatching otherwise we will race with the owning
  // thread.
  mState = (mMetadata->mCommonMetadata.version() != mRequestedVersion) ?
           State_BeginVersionChange :
           State_SendingResults;

  rv = mOwningThread->Dispatch(this, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
#ifdef DEBUG
    mState = State_DatabaseWork;
#endif
    return rv;
  }

  return NS_OK;
}

nsresult
OpenDatabaseOp::BeginVersionChange()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_BeginVersionChange);
  MOZ_ASSERT(mMaybeBlockedDatabases.IsEmpty());
  MOZ_ASSERT(mMetadata->mCommonMetadata.version() != mRequestedVersion);

  if (IsActorDestroyed()) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  nsresult rv = EnsureDatabaseActor();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  DatabaseActorInfo* info;
  MOZ_ALWAYS_TRUE(gLiveDatabaseHashtable->Get(mDatabaseId, &info));

  MOZ_ASSERT(!info->mLiveDatabases.IsEmpty());
  MOZ_ASSERT(info->mLiveDatabases.Contains(mDatabase));
  MOZ_ASSERT(!info->mWaitingOpenOp);

  nsRefPtr<VersionChangeTransaction> transaction =
    new VersionChangeTransaction(this);
  if (!mDatabase->SendPBackgroundIDBVersionChangeTransactionConstructor(
                                           transaction,
                                           mMetadata->mCommonMetadata.version(),
                                           mRequestedVersion,
                                           mMetadata->mNextObjectStoreId,
                                           mMetadata->mNextIndexId)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  // Transfer ownership to IPDL.
  transaction->AddRef();

  mVersionChangeTransaction = transaction;

  // See if any other databases need to be closed.
  uint32_t count = info->mLiveDatabases.Length();

  if (count == 1) {
    // No other databases need to be notified, we can jump directly to the
    // transaction thread pool.
    mState = State_DispatchToTransactionThreadPool;
    return Run();
  }

  TransactionThreadPool* threadPool = TransactionThreadPool::Get();
  MOZ_ASSERT(threadPool);

  // Intentionally empty.
  nsTArray<nsString> objectStoreNames;

  // Add a placeholder for this transaction immediately.
  rv = threadPool->Dispatch(transaction->TransactionId(),
                            transaction->GetDatabase()->Id(), objectStoreNames,
                            IDBTransaction::VERSION_CHANGE,
                            gStartTransactionRunnable, false, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  mMaybeBlockedDatabases.SetCapacity(count - 1);

  for (uint32_t index = 0; index < count; index++) {
    Database*& actor = info->mLiveDatabases[index];
    if (actor != mDatabase) {
      mMaybeBlockedDatabases.AppendElement(actor);
    }
  }

  MOZ_ASSERT(mMaybeBlockedDatabases.Length() == count - 1);

  count = mMaybeBlockedDatabases.Length();

  for (uint32_t index = 0; index < count; /* incremented conditionally */) {
    if (mMaybeBlockedDatabases[index]->SendVersionChange(
                                           mMetadata->mCommonMetadata.version(),
                                           mRequestedVersion)) {
      index++;
    } else {
      // We don't want to wait forever if we were not able to send the
      // message.
      mMaybeBlockedDatabases.RemoveElementAt(index);
      count--;
    }
  }

  info->mWaitingOpenOp = this;

  mState = State_WaitingForOtherDatabasesToClose;
  return NS_OK;
}

void
OpenDatabaseOp::NoteDatabaseDone(Database* aDatabase)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_WaitingForOtherDatabasesToClose ||
             mState == State_BlockedWaitingForOtherDatabasesToClose);
  MOZ_ASSERT(!mMaybeBlockedDatabases.IsEmpty());

  if (!mMaybeBlockedDatabases.RemoveElement(aDatabase)) {
    MOZ_ASSERT(IsActorDestroyed());
  }

  if (mMaybeBlockedDatabases.IsEmpty()) {
    mState = State_DispatchToTransactionThreadPool;
    Run();
  }
}

void
OpenDatabaseOp::NoteDatabaseBlocked(Database* aDatabase)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_WaitingForOtherDatabasesToClose ||
             mState == State_BlockedWaitingForOtherDatabasesToClose);
  MOZ_ASSERT(!mMaybeBlockedDatabases.IsEmpty());
  MOZ_ASSERT(mMaybeBlockedDatabases.Contains(aDatabase));

  // Don't send the blocked notification twice.
  if (mState == State_WaitingForOtherDatabasesToClose) {
    if (!IsActorDestroyed()) {
      unused << SendBlocked(mMetadata->mCommonMetadata.version());
    }
    mState = State_BlockedWaitingForOtherDatabasesToClose;
  }
}

nsresult
OpenDatabaseOp::DispatchToTransactionThreadPool()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_DispatchToTransactionThreadPool);
  MOZ_ASSERT(mVersionChangeTransaction);
  MOZ_ASSERT(mMaybeBlockedDatabases.IsEmpty());

  if (IsActorDestroyed()) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  TransactionThreadPool* threadPool = TransactionThreadPool::Get();
  MOZ_ASSERT(threadPool);

  // Intentionally empty.
  nsTArray<nsString> objectStoreNames;

  // Must set mState before dispatching otherwise we will race with the
  // transaction thread.
  mState = State_DatabaseWorkVersionChange;

  nsRefPtr<VersionChangeOp> versionChangeOp = new VersionChangeOp(this);

  nsresult rv =
    threadPool->Dispatch(mVersionChangeTransaction->TransactionId(),
                         mVersionChangeTransaction->GetDatabase()->Id(),
                         objectStoreNames, mVersionChangeTransaction->GetMode(),
                         versionChangeOp, false, nullptr);
  if (NS_WARN_IF(NS_FAILED(rv))) {
#ifdef DEBUG
    mState = State_DispatchToTransactionThreadPool;
#endif
    return rv;
  }

  return NS_OK;
}

void
OpenDatabaseOp::SendResults()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_SendingResults);
  MOZ_ASSERT_IF(NS_SUCCEEDED(mResultCode), mMaybeBlockedDatabases.IsEmpty());

  mMaybeBlockedDatabases.Clear();

  // Only needed if we're being called from within NoteDatabaseDone() since this
  // OpenDatabaseOp is only held alive by the gLiveDatabaseHashtable.
  nsRefPtr<OpenDatabaseOp> kungFuDeathGrip;

  DatabaseActorInfo* info;
  if (gLiveDatabaseHashtable->Get(mDatabaseId, &info) &&
      info->mWaitingOpenOp) {
    MOZ_ASSERT(info->mWaitingOpenOp == this);
    kungFuDeathGrip.swap(info->mWaitingOpenOp);
  }

  if (!IsActorDestroyed()) {
    FactoryRequestResponse response;

    if (NS_SUCCEEDED(mResultCode)) {
      nsresult rv = EnsureDatabaseActor();
      if (NS_SUCCEEDED(rv)) {
        // We successfully opened a database so use its actor as the success
        // result for this request.
        OpenDatabaseRequestResponse openResponse;
        openResponse.databaseParent() = mDatabase;
        response = openResponse;
      } else {
        response = rv;
#ifdef DEBUG
        mResultCode = rv;
#endif
      }
    } else {
      response = ClampResultCode(mResultCode);
    }

    unused <<
      PBackgroundIDBFactoryRequestParent::Send__delete__(this, response);
  }

  // Must set mState before dispatching otherwise we will race with the main
  // thread.
  mState = State_UnblockingQuotaManager;

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(this)));
}

nsresult
OpenDatabaseOp::EnsureDatabaseActor()
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mState == State_BeginVersionChange ||
             mState == State_SendingResults);
  MOZ_ASSERT(NS_SUCCEEDED(mResultCode));
  MOZ_ASSERT(!mDatabaseFilePath.IsEmpty());
  MOZ_ASSERT(!IsActorDestroyed());

  if (mDatabase) {
    return NS_OK;
  }

  mMetadata->mNextObjectStoreId = mLastObjectStoreId + 1;
  mMetadata->mNextIndexId = mLastIndexId + 1;

  MOZ_ASSERT(mMetadata->mFilePath.IsEmpty());
  mMetadata->mFilePath = mDatabaseFilePath;

  DatabaseSpec spec;
  spec.metadata() = mMetadata->mCommonMetadata;

  const uint32_t objectStoreCount = mMetadata->mObjectStores.Length();
  spec.objectStores().SetCapacity(objectStoreCount);

  for (uint32_t i = 0; i < objectStoreCount; i++) {
    const nsAutoPtr<FullObjectStoreMetadata>& objectStoreMetadata =
      mMetadata->mObjectStores[i];

    ObjectStoreSpec* objectStoreSpec = spec.objectStores().AppendElement();
    objectStoreSpec->metadata() = objectStoreMetadata->mCommonMetadata;

    const uint32_t indexCount = objectStoreMetadata->mIndexes.Length();
    objectStoreSpec->indexes().SetCapacity(indexCount);

    for (uint32_t j = 0; j < indexCount; j++) {
      const nsAutoPtr<FullIndexMetadata>& indexMetadata =
        objectStoreMetadata->mIndexes[j];

      IndexMetadata* indexSpec = objectStoreSpec->indexes().AppendElement();
      *indexSpec = indexMetadata->mCommonMetadata;
    }
  }

  auto factory = static_cast<BackgroundFactoryParent*>(Manager());

  nsRefPtr<Database> database =
    new Database(factory, mMetadata, mFileManager);

  if (!factory->SendPBackgroundIDBDatabaseConstructor(database, spec, this)) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  // Transfer ownership to IPDL.
  database->AddRef();

  DatabaseActorInfo* info;
  if (gLiveDatabaseHashtable->Get(mDatabaseId, &info)) {
    AssertMetadataConsistency(info->mMetadata);
    info->mLiveDatabases.AppendElement(database);
  } else {
    info = new DatabaseActorInfo(mOwnedMetadata.forget(), database);
    gLiveDatabaseHashtable->Put(mDatabaseId, info);
  }

  mDatabase = database;

  return NS_OK;
}

#ifdef DEBUG

void
OpenDatabaseOp::AssertMetadataConsistency(const FullDatabaseMetadata* aMetadata)
{
  AssertIsOnBackgroundThread();

  const FullDatabaseMetadata* db1 = aMetadata;
  const FullDatabaseMetadata* db2 = mMetadata;

  MOZ_ASSERT(db1);
  MOZ_ASSERT(db2);
  MOZ_ASSERT(db1 != db2);

  MOZ_ASSERT(db1->mCommonMetadata.name() == db2->mCommonMetadata.name());
  MOZ_ASSERT(db1->mCommonMetadata.version() == db2->mCommonMetadata.version());
  MOZ_ASSERT(db1->mCommonMetadata.persistenceType() ==
             db2->mCommonMetadata.persistenceType());
  MOZ_ASSERT(db1->mDatabaseId == db2->mDatabaseId);
  MOZ_ASSERT(db1->mFilePath == db2->mFilePath);
  MOZ_ASSERT(db1->mNextObjectStoreId == db2->mNextObjectStoreId);
  MOZ_ASSERT(db1->mNextIndexId == db2->mNextIndexId);

  MOZ_ASSERT(db1->mObjectStores.Length() == db2->mObjectStores.Length());

  for (uint32_t i = 0; i < db1->mObjectStores.Length(); i++) {
    const nsAutoPtr<FullObjectStoreMetadata>& obj1 = db1->mObjectStores[i];
    const nsAutoPtr<FullObjectStoreMetadata>& obj2 = db2->mObjectStores[i];

    MOZ_ASSERT(obj1);
    MOZ_ASSERT(obj2);
    MOZ_ASSERT(obj1 != obj2);

    MOZ_ASSERT(obj1->mCommonMetadata.name() == obj2->mCommonMetadata.name());
    MOZ_ASSERT(obj1->mCommonMetadata.keyPath() ==
               obj2->mCommonMetadata.keyPath());
    MOZ_ASSERT(obj1->mCommonMetadata.autoIncrement() ==
               obj2->mCommonMetadata.autoIncrement());
    MOZ_ASSERT(obj1->mCommonMetadata.id() == obj2->mCommonMetadata.id());
    MOZ_ASSERT(obj1->mNextAutoIncrementId == obj2->mNextAutoIncrementId);
    MOZ_ASSERT(obj1->mComittedAutoIncrementId ==
               obj2->mComittedAutoIncrementId);

    MOZ_ASSERT(obj1->mIndexes.Length() == obj2->mIndexes.Length());

    for (uint32_t j = 0; j < obj1->mIndexes.Length(); j++) {
      const nsAutoPtr<FullIndexMetadata>& idx1 = obj1->mIndexes[j];
      const nsAutoPtr<FullIndexMetadata>& idx2 = obj2->mIndexes[j];

      MOZ_ASSERT(idx1);
      MOZ_ASSERT(idx2);
      MOZ_ASSERT(idx1 != idx2);

      MOZ_ASSERT(idx1->mCommonMetadata.name() == idx2->mCommonMetadata.name());
      MOZ_ASSERT(idx1->mCommonMetadata.keyPath() ==
                 idx2->mCommonMetadata.keyPath());
      MOZ_ASSERT(idx1->mCommonMetadata.unique() ==
                 idx2->mCommonMetadata.unique());
      MOZ_ASSERT(idx1->mCommonMetadata.multiEntry() ==
                 idx2->mCommonMetadata.multiEntry());
      MOZ_ASSERT(idx1->mCommonMetadata.id() == idx2->mCommonMetadata.id());
    }
  }
}

#endif // DEBUG

nsresult
OpenDatabaseOp::
VersionChangeOp::DoDatabaseWork(TransactionBase* aTransaction)
{
  MOZ_ASSERT(aTransaction);
  MOZ_ASSERT(mOpenDatabaseOp->mState == State_DatabaseWorkVersionChange);

  PROFILER_LABEL("IndexedDB", "VersionChangeOp::DoDatabaseWork");

  mozIStorageConnection* connection = aTransaction->Connection();

  nsCOMPtr<mozIStorageStatement> stmt;
  nsresult rv = connection->CreateStatement(NS_LITERAL_CSTRING(
    "UPDATE database "
    "SET version = :version"
  ), getter_AddRefs(stmt));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("version"),
                             mOpenDatabaseOp->mRequestedVersion);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
OpenDatabaseOp::
VersionChangeOp::SendSuccessResult()
{
  AssertIsOnOwningThread();

  // We don't progress to the next state until the version change transaction
  // commits.
  return NS_OK;
}

bool
OpenDatabaseOp::
VersionChangeOp::SendFailureResult(nsresult aResultCode)
{
  AssertIsOnOwningThread();
  MOZ_ASSERT(mOpenDatabaseOp);

  mOpenDatabaseOp->SetFailureCode(aResultCode);
  mOpenDatabaseOp->mState = State_SendingResults;

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(mOpenDatabaseOp->Run()));

  return false;
}

void
OpenDatabaseOp::
VersionChangeOp::Cleanup()
{
  AssertIsOnOwningThread();

  mOpenDatabaseOp = nullptr;

#ifdef DEBUG
  // A bit hacky but the VersionChangeOp is not generated in response to a
  // child request like most other database operations. Do this to make our
  // assertions happy.
  NoteActorDestroyed();
#endif

  CommonDatabaseOperationBase::Cleanup();
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

    case State_DatabaseWork:
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

    // Must set mState before dispatching otherwise we will race with the owning
    // thread.
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
  MOZ_ASSERT(mState == State_DatabaseWork);

  PROFILER_LABEL("IndexedDB", "DeleteDatabaseOp::DoDatabaseWork");

  if (mActorDestroyed) {
    return NS_ERROR_DOM_INDEXEDDB_UNKNOWN_ERR;
  }

  QuotaManager* quotaManager = QuotaManager::Get();
  MOZ_ASSERT(quotaManager);

  nsCOMPtr<nsIFile> directory;
  nsresult rv = quotaManager->GetDirectoryForOrigin(mPersistenceType,
                                                    mOrigin,
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
      quotaManager->DecreaseUsageForOrigin(mPersistenceType, mGroup, mOrigin,
                                           fileSize);
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
      quotaManager->DecreaseUsageForOrigin(mPersistenceType, mGroup, mOrigin,
                                           usage);
    }
  }

  IndexedDatabaseManager* mgr = IndexedDatabaseManager::Get();
  MOZ_ASSERT(mgr);

  mgr->InvalidateFileManager(mPersistenceType, mOrigin, mName);

  // Must set this before dispatching otherwise we will race with the owning
  // thread.
  mState = State_SendingResults;

  rv = mOwningThread->Dispatch(this, NS_DISPATCH_NORMAL);
  if (NS_WARN_IF(NS_FAILED(rv))) {
#ifdef DEBUG
    mState = State_DatabaseWork;
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

  if (!IsActorDestroyed()) {
    FactoryRequestResponse response;

    if (NS_SUCCEEDED(mResultCode)) {
      response = DeleteDatabaseRequestResponse();
    } else {
      response = ClampResultCode(mResultCode);
    }

    unused <<
      PBackgroundIDBFactoryRequestParent::Send__delete__(this, response);
  }

  // Must set mState before dispatching otherwise we will race with the main
  // thread.
  mState = State_UnblockingQuotaManager;

  MOZ_ALWAYS_TRUE(NS_SUCCEEDED(NS_DispatchToMainThread(this)));
}

#ifdef DEBUG
void
CommonDatabaseOperationBase::AssertIsOnTransactionThread() const
{
  MOZ_ASSERT(mTransaction);
  mTransaction->AssertIsOnTransactionThread();
}
#endif

void
CommonDatabaseOperationBase::DispatchToTransactionThreadPool()
{
  AssertIsOnOwningThread();

  TransactionThreadPool* threadPool = TransactionThreadPool::Get();
  MOZ_ASSERT(threadPool);

  threadPool->Dispatch(mTransaction->TransactionId(),
                       mTransaction->GetDatabase()->Id(), this, false, nullptr);
}

NS_IMETHODIMP
CommonDatabaseOperationBase::Run()
{
  MOZ_ASSERT(mOwningThread);

  if (!IsOnBackgroundThread()) {
    MOZ_ASSERT(NS_SUCCEEDED(mResultCode));

    if (NS_WARN_IF(mActorDestroyed)) {
      // The child must have crashed so there's no reason to attempt any
      // database operations.
      return NS_OK;
    }

    nsresult rv = mTransaction->EnsureConnection();
    if (NS_WARN_IF(NS_FAILED(rv))) {
      mResultCode = rv;
    } else {
      rv = DoDatabaseWork(mTransaction);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        mResultCode = rv;
      }
    }

    if (NS_WARN_IF(NS_FAILED(mOwningThread->Dispatch(this,
                                                     NS_DISPATCH_NORMAL)))) {
      // This should only happen if the child has crashed.
      MOZ_ASSERT(mActorDestroyed);
      return NS_ERROR_FAILURE;
    }

    return NS_OK;
  }

  if (NS_SUCCEEDED(mResultCode)) {
    // This may release the IPDL reference.
    mResultCode = SendSuccessResult();
  }

  if (NS_WARN_IF(NS_FAILED(mResultCode))) {
    // This should definitely release the IPDL reference.
    if (!SendFailureResult(mResultCode)) {
      // Abort the transaction.
      mTransaction->Abort(mResultCode);
    }
  }

  Cleanup();

  return mResultCode;
}

NS_IMPL_ISUPPORTS1(TransactionBase::UpdateRefcountFunction, mozIStorageFunction)

NS_IMETHODIMP
TransactionBase::
UpdateRefcountFunction::OnFunctionCall(mozIStorageValueArray* aValues,
                                       nsIVariant** _retval)
{
  MOZ_ASSERT(aValues);
  MOZ_ASSERT(_retval);

  uint32_t numEntries;
  nsresult rv = aValues->GetNumEntries(&numEntries);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  MOZ_ASSERT(numEntries == 2);

#ifdef DEBUG
  {
    int32_t type1 = mozIStorageValueArray::VALUE_TYPE_NULL;
    MOZ_ASSERT(NS_SUCCEEDED(aValues->GetTypeOfIndex(0, &type1)));

    int32_t type2 = mozIStorageValueArray::VALUE_TYPE_NULL;
    MOZ_ASSERT(NS_SUCCEEDED(aValues->GetTypeOfIndex(1, &type2)));

    MOZ_ASSERT(!(type1 == mozIStorageValueArray::VALUE_TYPE_NULL &&
                 type2 == mozIStorageValueArray::VALUE_TYPE_NULL));
  }
#endif

  rv = ProcessValue(aValues, 0, eDecrement);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = ProcessValue(aValues, 1, eIncrement);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
TransactionBase::
UpdateRefcountFunction::WillCommit(mozIStorageConnection* aConnection)
{
  MOZ_ASSERT(aConnection);

  DatabaseUpdateFunction function(aConnection, this);

  mFileInfoEntries.EnumerateRead(DatabaseUpdateCallback, &function);

  nsresult rv = function.ErrorCode();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = CreateJournals();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

void
TransactionBase::
UpdateRefcountFunction::DidCommit()
{
  mFileInfoEntries.EnumerateRead(FileInfoUpdateCallback, nullptr);

  if (NS_FAILED(RemoveJournals(mJournalsToRemoveAfterCommit))) {
    NS_WARNING("RemoveJournals failed!");
  }
}

void
TransactionBase::
UpdateRefcountFunction::DidAbort()
{
  if (NS_FAILED(RemoveJournals(mJournalsToRemoveAfterAbort))) {
    NS_WARNING("RemoveJournals failed!");
  }
}

nsresult
TransactionBase::
UpdateRefcountFunction::ProcessValue(mozIStorageValueArray* aValues,
                                     int32_t aIndex,
                                     UpdateType aUpdateType)
{
  MOZ_ASSERT(aValues);

  int32_t type;
  nsresult rv = aValues->GetTypeOfIndex(aIndex, &type);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (type == mozIStorageValueArray::VALUE_TYPE_NULL) {
    return NS_OK;
  }

  nsString ids;
  rv = aValues->GetString(aIndex, ids);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsTArray<int64_t> fileIds;
  rv = IDBObjectStore::ConvertFileIdsToArray(ids, fileIds);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  for (uint32_t i = 0; i < fileIds.Length(); i++) {
    int64_t id = fileIds.ElementAt(i);

    FileInfoEntry* entry;
    if (!mFileInfoEntries.Get(id, &entry)) {
      nsRefPtr<FileInfo> fileInfo = mFileManager->GetFileInfo(id);
      MOZ_ASSERT(fileInfo);

      entry = new FileInfoEntry(fileInfo);
      mFileInfoEntries.Put(id, entry);
    }

    switch (aUpdateType) {
      case eIncrement:
        entry->mDelta++;
        break;
      case eDecrement:
        entry->mDelta--;
        break;
      default:
        MOZ_CRASH("Unknown update type!");
    }
  }

  return NS_OK;
}

nsresult
TransactionBase::
UpdateRefcountFunction::CreateJournals()
{
  nsCOMPtr<nsIFile> journalDirectory = mFileManager->GetJournalDirectory();
  if (NS_WARN_IF(!journalDirectory)) {
    return NS_ERROR_FAILURE;
  }

  for (uint32_t i = 0; i < mJournalsToCreateBeforeCommit.Length(); i++) {
    int64_t id = mJournalsToCreateBeforeCommit[i];

    nsCOMPtr<nsIFile> file =
      mFileManager->GetFileForId(journalDirectory, id);
    if (NS_WARN_IF(!file)) {
      return NS_ERROR_FAILURE;
    }

    nsresult rv = file->Create(nsIFile::NORMAL_FILE_TYPE, 0644);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    mJournalsToRemoveAfterAbort.AppendElement(id);
  }

  return NS_OK;
}

nsresult
TransactionBase::
UpdateRefcountFunction::RemoveJournals(const nsTArray<int64_t>& aJournals)
{
  nsCOMPtr<nsIFile> journalDirectory = mFileManager->GetJournalDirectory();
  if (NS_WARN_IF(!journalDirectory)) {
    return NS_ERROR_FAILURE;
  }

  for (uint32_t index = 0; index < aJournals.Length(); index++) {
    nsCOMPtr<nsIFile> file =
      mFileManager->GetFileForId(journalDirectory, aJournals[index]);
    if (NS_WARN_IF(!file)) {
      return NS_ERROR_FAILURE;
    }

    if (NS_FAILED(file->Remove(false))) {
      NS_WARNING("Failed to removed journal!");
    }
  }

  return NS_OK;
}

PLDHashOperator
TransactionBase::
UpdateRefcountFunction::DatabaseUpdateCallback(const uint64_t& aKey,
                                               FileInfoEntry* aValue,
                                               void* aUserArg)
{
  MOZ_ASSERT(aValue);
  MOZ_ASSERT(aUserArg);

  if (!aValue->mDelta) {
    return PL_DHASH_NEXT;
  }

  auto function = static_cast<DatabaseUpdateFunction*>(aUserArg);

  if (!function->Update(aKey, aValue->mDelta)) {
    return PL_DHASH_STOP;
  }

  return PL_DHASH_NEXT;
}

PLDHashOperator
TransactionBase::
UpdateRefcountFunction::FileInfoUpdateCallback(const uint64_t& aKey,
                                               FileInfoEntry* aValue,
                                               void* aUserArg)
{
  MOZ_ASSERT(aValue);

  if (aValue->mDelta) {
    aValue->mFileInfo->UpdateDBRefs(aValue->mDelta);
  }

  return PL_DHASH_NEXT;
}

bool
TransactionBase::UpdateRefcountFunction::
DatabaseUpdateFunction::Update(int64_t aId,
                               int32_t aDelta)
{
  nsresult rv = UpdateInternal(aId, aDelta);
  if (NS_FAILED(rv)) {
    mErrorCode = rv;
    return false;
  }

  return true;
}

nsresult
TransactionBase::UpdateRefcountFunction::
DatabaseUpdateFunction::UpdateInternal(int64_t aId,
                                       int32_t aDelta)
{
  nsresult rv;

  if (!mUpdateStatement) {
    rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
      "UPDATE file "
      "SET refcount = refcount + :delta "
      "WHERE id = :id"
    ), getter_AddRefs(mUpdateStatement));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  mozStorageStatementScoper updateScoper(mUpdateStatement);

  rv = mUpdateStatement->BindInt32ByName(NS_LITERAL_CSTRING("delta"), aDelta);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mUpdateStatement->BindInt64ByName(NS_LITERAL_CSTRING("id"), aId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mUpdateStatement->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  int32_t rows;
  rv = mConnection->GetAffectedRows(&rows);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (rows > 0) {
    if (!mSelectStatement) {
      rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
        "SELECT id "
        "FROM file "
        "WHERE id = :id"
      ), getter_AddRefs(mSelectStatement));
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }

    mozStorageStatementScoper selectScoper(mSelectStatement);

    rv = mSelectStatement->BindInt64ByName(NS_LITERAL_CSTRING("id"), aId);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    bool hasResult;
    rv = mSelectStatement->ExecuteStep(&hasResult);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }

    if (!hasResult) {
      // Don't have to create the journal here, we can create all at once,
      // just before commit
      mFunction->mJournalsToCreateBeforeCommit.AppendElement(aId);
    }

    return NS_OK;
  }

  if (!mInsertStatement) {
    rv = mConnection->CreateStatement(NS_LITERAL_CSTRING(
      "INSERT INTO file (id, refcount) "
      "VALUES(:id, :delta)"
    ), getter_AddRefs(mInsertStatement));
    if (NS_WARN_IF(NS_FAILED(rv))) {
      return rv;
    }
  }

  mozStorageStatementScoper insertScoper(mInsertStatement);

  rv = mInsertStatement->BindInt64ByName(NS_LITERAL_CSTRING("id"), aId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mInsertStatement->BindInt32ByName(NS_LITERAL_CSTRING("delta"), aDelta);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = mInsertStatement->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  mFunction->mJournalsToRemoveAfterCommit.AppendElement(aId);
  return NS_OK;
}

nsresult
TransactionBase::
CommitOp::WriteAutoIncrementCounts()
{
  AssertIsOnTransactionThread();
  MOZ_ASSERT(mTransaction);

  const nsTArray<FullObjectStoreMetadata*>& metadataArray =
    mTransaction->mModifiedAutoIncrementObjectStoreMetadataArray;

  nsCOMPtr<mozIStorageStatement> stmt;
  nsresult rv;

  if (!metadataArray.IsEmpty()) {
    NS_NAMED_LITERAL_CSTRING(osid, "osid");
    NS_NAMED_LITERAL_CSTRING(ai, "ai");

    for (uint32_t count = metadataArray.Length(), index = 0;
         index < count;
         index++) {
      const FullObjectStoreMetadata* metadata = metadataArray[index];

      if (stmt) {
        MOZ_ALWAYS_TRUE(NS_SUCCEEDED(stmt->Reset()));
      } else {
        rv = mTransaction->mConnection->CreateStatement(
          NS_LITERAL_CSTRING("UPDATE object_store "
                             "SET auto_increment = :") + ai +
          NS_LITERAL_CSTRING(" WHERE id = :") + osid +
          NS_LITERAL_CSTRING(";"),
          getter_AddRefs(stmt));
        if (NS_WARN_IF(NS_FAILED(rv))) {
          return rv;
        }
      }

      rv = stmt->BindInt64ByName(osid, metadata->mCommonMetadata.id());
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      rv = stmt->BindInt64ByName(ai, metadata->mNextAutoIncrementId);
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }

      rv = stmt->Execute();
      if (NS_WARN_IF(NS_FAILED(rv))) {
        return rv;
      }
    }
  }

  return NS_OK;
}

void
TransactionBase::
CommitOp::CommitOrRollbackAutoIncrementCounts()
{
  AssertIsOnTransactionThread();
  MOZ_ASSERT(mTransaction);

  const nsTArray<FullObjectStoreMetadata*>& metadataArray =
    mTransaction->mModifiedAutoIncrementObjectStoreMetadataArray;

  if (!metadataArray.IsEmpty()) {
    bool committed = NS_SUCCEEDED(mResultCode);

    DatabaseActorInfo* info;
    MOZ_ALWAYS_TRUE(gLiveDatabaseHashtable->Get(mTransaction->mDatabase->Id(),
                                                &info));

    FullDatabaseMetadata* dbMetadata = info->mMetadata;

    for (uint32_t count = metadataArray.Length(), index = 0;
         index < count;
         index++) {
      FullObjectStoreMetadata* metadata = metadataArray[index];

      if (committed) {
        metadata->mComittedAutoIncrementId = metadata->mNextAutoIncrementId;
      } else {
        metadata->mNextAutoIncrementId = metadata->mComittedAutoIncrementId;
      }
    }
  }
}

NS_IMPL_ISUPPORTS_INHERITED0(TransactionBase::CommitOp, nsRunnable)

NS_IMETHODIMP
TransactionBase::
CommitOp::Run()
{
  MOZ_ASSERT(mTransaction);

  PROFILER_LABEL("IndexedDB", "CommitOp::Run");

  nsCOMPtr<mozIStorageConnection>& connection = mTransaction->mConnection;

  if (!connection) {
    return NS_OK;
  }

  AssertIsOnTransactionThread();

  if (NS_SUCCEEDED(mResultCode) && mTransaction->mUpdateFileRefcountFunction) {
    mResultCode = mTransaction->
      mUpdateFileRefcountFunction->WillCommit(connection);
  }

  if (NS_SUCCEEDED(mResultCode)) {
    mResultCode = WriteAutoIncrementCounts();
  }

  if (NS_SUCCEEDED(mResultCode)) {
    NS_NAMED_LITERAL_CSTRING(commit, "COMMIT TRANSACTION");
    mResultCode = connection->ExecuteSimpleSQL(commit);

    if (NS_SUCCEEDED(mResultCode)) {
      if (mTransaction->mUpdateFileRefcountFunction) {
        mTransaction->mUpdateFileRefcountFunction->DidCommit();
      }
    }
  }

  if (NS_FAILED(mResultCode)) {
    if (mTransaction->mUpdateFileRefcountFunction) {
      mTransaction->mUpdateFileRefcountFunction->DidAbort();
    }

    NS_NAMED_LITERAL_CSTRING(rollback, "ROLLBACK TRANSACTION");
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(connection->ExecuteSimpleSQL(rollback)));
  }

  CommitOrRollbackAutoIncrementCounts();

  if (mTransaction->mUpdateFileRefcountFunction) {
    NS_NAMED_LITERAL_CSTRING(functionName, "update_refcount");
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(connection->RemoveFunction(functionName)));
  }

  mTransaction->ReleaseTransactionThreadObjects();

  return NS_OK;
}

void
TransactionBase::
CommitOp::TransactionFinishedBeforeUnblock()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(mTransaction);

  PROFILER_LABEL("IndexedDB", "CommitOp::TransactionFinishedBeforeUnblock");

  mTransaction->UpdateMetadata(mResultCode);
}

void
TransactionBase::
CommitOp::TransactionFinishedAfterUnblock()
{
  AssertIsOnBackgroundThread();
  MOZ_ASSERT(mTransaction);

  PROFILER_LABEL("IndexedDB", "CommitOp::TransactionFinishedAfterUnblock");

  IDB_PROFILER_MARK("IndexedDB Transaction %llu: Complete (rv = %lu)",
                    "IDBTransaction[%llu] MT Complete",
                    mTransaction->TransactionId(), mResultCode);

  mTransaction->SendCompleteNotification(ClampResultCode(mResultCode));

  mTransaction->ReleaseBackgroundThreadObjects();
  mTransaction = nullptr;

#ifdef DEBUG
  // A bit hacky but the CommitOp is not really a normal database operation
  // that is tied to an actor. Do this to make our assertions happy.
  NoteActorDestroyed();
#endif
}

nsresult
VersionChangeOp::SendSuccessResult()
{
  AssertIsOnOwningThread();

  // Nothing to send here, the API assumes that this request always succeeds.
  return NS_OK;
}

bool
VersionChangeOp::SendFailureResult(nsresult aResultCode)
{
  AssertIsOnOwningThread();

  // The only option here is to cause the transaction to abort.
  return false;
}

void
VersionChangeOp::Cleanup()
{
  AssertIsOnOwningThread();

#ifdef DEBUG
  // A bit hacky but the VersionChangeOp is not generated in response to a child
  // request like most other database operations. Do this to make our assertions
  // happy.
  NoteActorDestroyed();
#endif

  CommonDatabaseOperationBase::Cleanup();
}

nsresult
CreateObjectStoreOp::DoDatabaseWork(TransactionBase* aTransaction)
{
  MOZ_ASSERT(aTransaction);

  PROFILER_LABEL("IndexedDB", "CreateObjectStoreOp::DoDatabaseWork");

  if (NS_WARN_IF(IndexedDatabaseManager::InLowDiskSpaceMode())) {
    return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
  }

  TransactionBase::CachedStatement stmt;
  nsresult rv = aTransaction->GetCachedStatement(
    "INSERT INTO object_store (id, auto_increment, name, key_path) "
    "VALUES (:id, :auto_increment, :name, :key_path)",
    &stmt);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("id"), mMetadata.id());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt32ByName(NS_LITERAL_CSTRING("auto_increment"),
                             mMetadata.autoIncrement() ? 1 : 0);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindStringByName(NS_LITERAL_CSTRING("name"), mMetadata.name());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  NS_NAMED_LITERAL_CSTRING(keyPath, "key_path");

  if (mMetadata.keyPath().IsValid()) {
    nsAutoString keyPathSerialization;
    mMetadata.keyPath().SerializeToString(keyPathSerialization);

    rv = stmt->BindStringByName(keyPath, keyPathSerialization);
  } else {
    rv = stmt->BindNullByName(keyPath);
  }

  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
DeleteObjectStoreOp::DoDatabaseWork(TransactionBase* aTransaction)
{
  MOZ_ASSERT(aTransaction);

  PROFILER_LABEL("IndexedDB", "DeleteObjectStoreOp::DoDatabaseWork");

  TransactionBase::CachedStatement stmt;
  nsresult rv = aTransaction->GetCachedStatement(
    "DELETE FROM object_store "
    "WHERE id = :id",
    &stmt);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("id"), mId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
CreateIndexOp::DoDatabaseWork(TransactionBase* aTransaction)
{
  MOZ_ASSERT(aTransaction);

  PROFILER_LABEL("IndexedDB", "CreateIndexOp::DoDatabaseWork");

  if (NS_WARN_IF(IndexedDatabaseManager::InLowDiskSpaceMode())) {
    return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
  }

  TransactionBase::CachedStatement stmt;
  nsresult rv = aTransaction->GetCachedStatement(
    "INSERT INTO object_store_index (id, name, key_path, unique_index, "
                                    "multientry, object_store_id) "
    "VALUES (:id, :name, :key_path, :unique, :multientry, :osid)",
    &stmt);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("id"), mMetadata.id());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindStringByName(NS_LITERAL_CSTRING("name"), mMetadata.name());
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  nsAutoString keyPathSerialization;
  mMetadata.keyPath().SerializeToString(keyPathSerialization);
  rv = stmt->BindStringByName(NS_LITERAL_CSTRING("key_path"),
                              keyPathSerialization);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt32ByName(NS_LITERAL_CSTRING("unique"),
                             mMetadata.unique() ? 1 : 0);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt32ByName(NS_LITERAL_CSTRING("multientry"),
                             mMetadata.multiEntry() ? 1 : 0);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"), mObjectStoreId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

#ifdef DEBUG
  {
    int64_t id;
    MOZ_ALWAYS_TRUE(NS_SUCCEEDED(
      aTransaction->Connection()->GetLastInsertRowID(&id)));
    MOZ_ASSERT(mMetadata.id());
  }
#endif

  rv = InsertDataFromObjectStore(aTransaction);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}

nsresult
CreateIndexOp::InsertDataFromObjectStore(TransactionBase* aTransaction)
{
  MOZ_ASSERT(aTransaction);
  MOZ_ASSERT(!IndexedDatabaseManager::InLowDiskSpaceMode());

  PROFILER_LABEL("IndexedDB", "CreateIndexOp::InsertDataFromObjectStore");

  TransactionBase::CachedStatement stmt;
  nsresult rv = aTransaction->GetCachedStatement(
    "SELECT id, data, file_ids, key_value "
    "FROM object_data "
    "WHERE object_store_id = :osid",
    &stmt);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("osid"), mObjectStoreId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  bool hasResult;
  rv = stmt->ExecuteStep(&hasResult);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  if (!hasResult) {
    // Bail early if we have no data to avoid creating the runtime below.
    return NS_OK;
  }

  MOZ_CRASH("Implement me!");
}

nsresult
DeleteIndexOp::DoDatabaseWork(TransactionBase* aTransaction)
{
  MOZ_ASSERT(aTransaction);

  PROFILER_LABEL("IndexedDB", "DeleteIndexOp::DoDatabaseWork");

  if (NS_WARN_IF(IndexedDatabaseManager::InLowDiskSpaceMode())) {
    return NS_ERROR_DOM_INDEXEDDB_QUOTA_ERR;
  }

  TransactionBase::CachedStatement stmt;
  nsresult rv = aTransaction->GetCachedStatement(
    "DELETE FROM object_store_index "
    "WHERE id = :id ",
    &stmt);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->BindInt64ByName(NS_LITERAL_CSTRING("id"), mIndexId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  rv = stmt->Execute();
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return rv;
  }

  return NS_OK;
}
