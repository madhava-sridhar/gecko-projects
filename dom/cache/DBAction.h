/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_DBAction_h
#define mozilla_dom_cache_DBAction_h

#include "mozilla/dom/cache/Action.h"
#include "mozilla/dom/cache/CacheInitData.h"
#include "nsString.h"

class mozIStorageConnection;
class nsIFile;

namespace mozilla {
namespace dom {
namespace cache {

class DBAction : public Action
{
protected:
  enum Mode
  {
    Existing,
    Create
  };

  DBAction(Mode aMode, const CacheInitData& aInitData);

  // Just as the resolver must be ref'd until cancel or resolve, you may also
  // ref the DB connection.  The connection can only be referenced from the
  // target thread and must be released upon cancel or resolve.
  virtual void RunWithDBOnTarget(Resolver* aResolver, nsIFile* aDBDir,
                                 mozIStorageConnection* aConn)=0;

  virtual
  void RunOnTarget(Resolver* aResolver, nsIFile* aQuotaDir) MOZ_OVERRIDE;

  virtual ~DBAction() { }

private:
  nsresult OpenConnection(nsIFile* aQuotaDir, mozIStorageConnection** aConnOut);

  const Mode mMode;
  const CacheInitData mInitData;
};

class SyncDBAction : public DBAction
{
protected:
  SyncDBAction(Mode aMode, const CacheInitData& aInitData);

  virtual ~SyncDBAction() { }

  virtual nsresult RunSyncWithDBOnTarget(nsIFile* aDBDir,
                                         mozIStorageConnection* aConn)=0;

  virtual void RunWithDBOnTarget(Resolver* aResolver, nsIFile* aDBDir,
                                 mozIStorageConnection* aConn) MOZ_OVERRIDE;
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_DBAction_h
