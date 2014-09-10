/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_CacheDBSchema_h
#define mozilla_dom_cache_CacheDBSchema_h

#include "mozilla/dom/CacheTypes.h"

class mozIStorageConnection;
template<class T> class nsTArray;

namespace mozilla {
namespace dom {

class PCacheQueryParams;
class PCacheRequest;
class PCacheResponse;
class PCacheResponseOrVoid;

class CacheDBSchema MOZ_FINAL
{
public:
  typedef int32_t EntryId;

  static nsresult Create(mozIStorageConnection* aConn);

  static nsresult Match(mozIStorageConnection* aConnection,
                        const PCacheRequest& aRequest,
                        const PCacheQueryParams& aParams,
                        PCacheResponseOrVoid* aResponseOrVoidOut);

  static nsresult MatchAll(mozIStorageConnection* aConnection,
                           const PCacheRequestOrVoid& aRequestOrVoid,
                           const PCacheQueryParams& aParams,
                           nsTArray<PCacheResponse>& aResponsesOut);

  static nsresult Put(mozIStorageConnection* aConnection,
                      const PCacheRequest& aRequest,
                      const PCacheResponse& aResponse);

  static nsresult Delete(mozIStorageConnection* aConnection,
                         const PCacheRequest& aRequest,
                         const PCacheQueryParams& aParams,
                         bool* aSuccessOut);

private:
  static nsresult QueryAll(mozIStorageConnection* aConnection,
                           nsTArray<EntryId>& aEntryIdListOut);
  static nsresult QueryCache(mozIStorageConnection* aConnection,
                             const PCacheRequest& aRequest,
                             const PCacheQueryParams& aParams,
                             nsTArray<EntryId>& aEntryIdListOut);
  static nsresult MatchByVaryHeader(mozIStorageConnection* aConnection,
                                    const PCacheRequest& aRequest,
                                    int32_t entryId, bool* aSuccessOut);
  static nsresult DeleteEntries(mozIStorageConnection* aConnection,
                                const nsTArray<EntryId>& aEntryIdList,
                                uint32_t aPos=0, int32_t aLen=-1);
  static nsresult InsertEntry(mozIStorageConnection* aConnection,
                              const PCacheRequest& aRequest,
                              const PCacheResponse& aResponse);
  static nsresult ReadResponse(mozIStorageConnection* aConnection,
                               EntryId aEntryId, PCacheResponse& aResponseOut);

  CacheDBSchema() MOZ_DELETE;
  ~CacheDBSchema() MOZ_DELETE;

  static const int32_t kLatestSchemaVersion = 1;
};

} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_CacheDBSchema_h
