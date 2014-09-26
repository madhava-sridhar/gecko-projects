/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_FileUtils_h
#define mozilla_dom_cache_FileUtils_h

#include "mozilla/Attributes.h"
#include "mozilla/dom/cache/Types.h"
#include "nsStreamUtils.h"

struct nsID;
class nsIFile;
template<class T> class nsTArray;

namespace mozilla {
namespace dom {
namespace cache {

class FileUtils MOZ_FINAL
{
public:
  enum BodyFileType
  {
    BODY_FILE_FINAL,
    BODY_FILE_TMP
  };

  static nsresult BodyCreateDir(nsIFile* aBaseDir);
  static nsresult BodyGetCacheDir(nsIFile* aBaseDir, CacheId aCacheId,
                                  nsIFile** aCacheDirOut);
  static nsresult BodyDeleteCacheDir(nsIFile* aBaseDir, CacheId aCacheId);

  static nsresult
  BodyIdToFile(nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId,
               BodyFileType aType, nsIFile** aBodyFileOut);

  static nsresult
  BodyStartWriteStream(const nsACString& aOrigin, const nsACString& aBaseDomain,
                       nsIFile* aBaseDir, CacheId aCacheId,
                       nsIInputStream* aSource, void* aClosure,
                       nsAsyncCopyCallbackFun aCallback, nsID* aIdOut,
                       nsISupports** aCopyContextOut);

  static void
  BodyCancelWrite(nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId,
                  nsISupports* aCopyContext);

  static nsresult
  BodyFinalizeWrite(nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId);

  static nsresult
  BodyStartReadStream(const nsACString& aOrigin, const nsACString& aBaseDomain,
                      nsIFile* aBaseDir, CacheId aCacheId, const nsID& aId,
                      nsIOutputStream* aDest, void* aClosure,
                      nsAsyncCopyCallbackFun aCallback,
                      nsISupports** aCopyContextOut);

  static void BodyCancelRead(nsISupports* aCopyContext);

  static nsresult
  BodyDeleteFiles(nsIFile* aBaseDir, CacheId aCacheId,
                  const nsTArray<nsID>& aIdList);

private:
  FileUtils() MOZ_DELETE;
  ~FileUtils() MOZ_DELETE;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_FileUtils_h
