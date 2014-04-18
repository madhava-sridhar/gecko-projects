/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef IndexedDatabaseInlines_h
#define IndexedDatabaseInlines_h

#ifndef mozilla_dom_indexeddb_indexeddatabase_h__
#error Must include IndexedDatabase.h first
#endif

#include "FileInfo.h"
#include "mozilla/dom/indexedDB/PBackgroundIDBSharedTypes.h"
#include "nsIDOMFile.h"
#include "nsIInputStream.h"

namespace mozilla {
namespace dom {
namespace indexedDB {

inline
StructuredCloneFile::StructuredCloneFile()
{ }

inline
StructuredCloneFile::~StructuredCloneFile()
{ }

inline
bool
StructuredCloneFile::operator==(const StructuredCloneFile& aOther) const
{
  return this->mFile == aOther.mFile &&
          this->mFileInfo == aOther.mFileInfo &&
          this->mInputStream == aOther.mInputStream;
}

inline
StructuredCloneWriteInfo::StructuredCloneWriteInfo()
: mTransaction(nullptr),
  mOffsetToKeyProp(0)
{
}

inline
StructuredCloneWriteInfo::StructuredCloneWriteInfo(
                                    StructuredCloneWriteInfo&& aCloneWriteInfo)
: mCloneBuffer(Move(aCloneWriteInfo.mCloneBuffer))
, mTransaction(aCloneWriteInfo.mTransaction)
, mOffsetToKeyProp(aCloneWriteInfo.mOffsetToKeyProp)
{
  mFiles.SwapElements(aCloneWriteInfo.mFiles);
  aCloneWriteInfo.mTransaction = nullptr;
  aCloneWriteInfo.mOffsetToKeyProp = 0;
}

inline
bool
StructuredCloneWriteInfo::operator==(
                                   const StructuredCloneWriteInfo& aOther) const
{
  return this->mCloneBuffer.nbytes() == aOther.mCloneBuffer.nbytes() &&
         this->mCloneBuffer.data() == aOther.mCloneBuffer.data() &&
         this->mFiles == aOther.mFiles &&
         this->mTransaction == aOther.mTransaction &&
         this->mOffsetToKeyProp == aOther.mOffsetToKeyProp;
}

inline
bool
StructuredCloneWriteInfo::SetFromSerialized(
                               const SerializedStructuredCloneWriteInfo& aOther)
{
  if (aOther.data().IsEmpty()) {
    mCloneBuffer.clear();
  } else {
    uint64_t* aOtherBuffer =
      reinterpret_cast<uint64_t*>(
        const_cast<uint8_t*>(aOther.data().Elements()));
    if (!mCloneBuffer.copy(aOtherBuffer, aOther.data().Length())) {
      return false;
    }
  }

  mFiles.Clear();
  mOffsetToKeyProp = aOther.offsetToKeyProp();
  return true;
}

inline
StructuredCloneReadInfo::StructuredCloneReadInfo()
  : mDatabase(nullptr)
{
}

inline
StructuredCloneReadInfo::StructuredCloneReadInfo(
                             SerializedStructuredCloneReadInfo&& aCloneReadInfo)
  : mData(Move(aCloneReadInfo.data()))
  , mDatabase(nullptr)
{ }

inline StructuredCloneReadInfo&
StructuredCloneReadInfo::operator=(StructuredCloneReadInfo&& aCloneReadInfo)
{
  MOZ_ASSERT(&aCloneReadInfo != this);

  mData = Move(aCloneReadInfo.mData);
  mCloneBuffer = Move(aCloneReadInfo.mCloneBuffer);
  mFiles.Clear();
  mFiles.SwapElements(aCloneReadInfo.mFiles);
  mDatabase = aCloneReadInfo.mDatabase;
  aCloneReadInfo.mDatabase = nullptr;
  return *this;
}

} // namespace indexedDB
} // namespace dom
} // namespace mozilla

#endif // IndexedDatabaseInlines_h
