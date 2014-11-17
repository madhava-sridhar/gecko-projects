/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ArrayBufferBuilder_h
#define mozilla_ArrayBufferBuilder_h

#include "mozilla/Types.h"
#include "nsError.h"

struct JSContext;
class JSObject;
class nsCString;
class nsIFile;

namespace mozilla {
// A helper for building up an ArrayBuffer object's data
// before creating the ArrayBuffer itself.  Will do doubling
// based reallocation, up to an optional maximum growth given.
//
// When all the data has been appended, call getArrayBuffer,
// passing in the JSContext* for which the ArrayBuffer object
// is to be created.  This also implicitly resets the builder,
// or it can be reset explicitly at any point by calling reset().
class ArrayBufferBuilder
{
  uint8_t* mDataPtr;
  uint32_t mCapacity;
  uint32_t mLength;
  void* mMapPtr;
public:
  ArrayBufferBuilder();
  ~ArrayBufferBuilder();

  void reset();

  // Will truncate if aNewCap is < length().
  bool setCapacity(uint32_t aNewCap);

  // Append aDataLen bytes from data to the current buffer.  If we
  // need to grow the buffer, grow by doubling the size up to a
  // maximum of aMaxGrowth (if given).  If aDataLen is greater than
  // what the new capacity would end up as, then grow by aDataLen.
  //
  // The data parameter must not overlap with anything beyond the
  // builder's current valid contents [0..length)
  bool append(const uint8_t* aNewData, uint32_t aDataLen,
              uint32_t aMaxGrowth = 0);

  uint32_t length()   { return mLength; }
  uint32_t capacity() { return mCapacity; }

  JSObject* getArrayBuffer(JSContext* aCx);

  // Memory mapping to starting position of file(aFile) in the zip
  // package(aJarFile).
  //
  // The file in the zip package has to be uncompressed and the starting
  // position of the file must be aligned according to array buffer settings
  // in JS engine.
  nsresult mapToFileInPackage(const nsCString& aFile, nsIFile* aJarFile);

protected:
  static bool areOverlappingRegions(const uint8_t* aStart1, uint32_t aLength1,
                                    const uint8_t* aStart2, uint32_t aLength2);
};
} // namespace mozilla

#endif // mozilla_ArrayBufferBuilder_h
