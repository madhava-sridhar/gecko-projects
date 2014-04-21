/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_indexeddatabase_h__
#define mozilla_dom_indexeddb_indexeddatabase_h__

#include "nsIProgrammingLanguage.h"

#include "js/StructuredClone.h"
#include "nsAutoPtr.h"
#include "nsCOMPtr.h"
#include "nsTArray.h"

#define BEGIN_INDEXEDDB_NAMESPACE \
  namespace mozilla { namespace dom { namespace indexedDB {

#define END_INDEXEDDB_NAMESPACE \
  } /* namespace indexedDB */ } /* namepsace dom */ } /* namespace mozilla */

#define USING_INDEXEDDB_NAMESPACE \
  using namespace mozilla::dom::indexedDB;

class nsIDOMBlob;
class nsIInputStream;

namespace mozilla {
namespace dom {
namespace indexedDB {

class FileInfo;
class IDBDatabase;
class IDBTransaction;
class SerializedStructuredCloneReadInfo;
class SerializedStructuredCloneWriteInfo;

struct StructuredCloneFile
{
  nsCOMPtr<nsIDOMBlob> mFile;
  nsRefPtr<FileInfo> mFileInfo;
  nsCOMPtr<nsIInputStream> mInputStream;

  // In IndexedDatabaseInlines.h
  inline
  StructuredCloneFile();

  // In IndexedDatabaseInlines.h
  inline
  ~StructuredCloneFile();

  // In IndexedDatabaseInlines.h
  inline bool
  operator==(const StructuredCloneFile& aOther) const;
};

struct StructuredCloneReadInfo
{
  nsTArray<uint8_t> mData;
  nsTArray<StructuredCloneFile> mFiles;
  IDBDatabase* mDatabase;

  // XXX Remove!
  JSAutoStructuredCloneBuffer mCloneBuffer;

  // In IndexedDatabaseInlines.h
  inline
  StructuredCloneReadInfo();

  // In IndexedDatabaseInlines.h
  inline StructuredCloneReadInfo&
  operator=(StructuredCloneReadInfo&& aOther);

  // In IndexedDatabaseInlines.h
  inline
  StructuredCloneReadInfo(SerializedStructuredCloneReadInfo&& aOther);
};

struct StructuredCloneWriteInfo
{
  // In IndexedDatabaseInlines.h
  inline StructuredCloneWriteInfo();
  inline StructuredCloneWriteInfo(StructuredCloneWriteInfo&& aCloneWriteInfo);

  inline bool
  operator==(const StructuredCloneWriteInfo& aOther) const;

  // In IndexedDatabaseInlines.h
  inline bool
  SetFromSerialized(const SerializedStructuredCloneWriteInfo& aOther);

  JSAutoStructuredCloneBuffer mCloneBuffer;
  nsTArray<StructuredCloneFile> mFiles;
  IDBTransaction* mTransaction;
  uint64_t mOffsetToKeyProp;
};

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_indexeddb_indexeddatabase_h__
