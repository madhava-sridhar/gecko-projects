/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_ReadStream_h
#define mozilla_dom_cache_ReadStream_h

#include "mozilla/dom/cache/CacheStreamControlChild.h"
#include "nsCOMPtr.h"
#include "nsID.h"
#include "nsIInputStream.h"
#include "nsIIPCSerializableInputStream.h"
#include "nsISupportsImpl.h"

template<class T> class nsTArray;

namespace mozilla {
namespace dom {
namespace cache {

class PCacheReadStream;
class PCacheReadStreamOrVoid;

class ReadStream : public nsIInputStream
                 , public nsIIPCSerializableInputStream
                 , public CacheStreamControlChild::Listener
{
public:
  static already_AddRefed<ReadStream>
  Create(PCacheStreamControlChild* aControl,
         const PCacheReadStreamOrVoid& aReadStreamOrVoid);

  static already_AddRefed<ReadStream>
  Create(PCacheStreamControlChild* aControl,
         const PCacheReadStream& aReadStream);

  // CacheStreamControlChild::Listener methods
  virtual void CloseStream() MOZ_OVERRIDE;
  virtual bool MatchId(const nsID& aId) MOZ_OVERRIDE;

private:
  ReadStream(PCacheStreamControlChild* aControl, const nsID& aId,
             nsIInputStream* aStream);
  virtual ~ReadStream();

  void NoteClosed();

  CacheStreamControlChild* mControl;
  const nsID mId;
  nsCOMPtr<nsIInputStream> mStream;
  nsCOMPtr<nsIIPCSerializableInputStream> mSerializable;
  bool mClosed;

public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIINPUTSTREAM
  NS_DECL_NSIIPCSERIALIZABLEINPUTSTREAM
};

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_ReadStream_h
