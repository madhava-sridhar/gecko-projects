/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_cache_ReadStream_h
#define mozilla_dom_cache_ReadStream_h

#include "mozilla/dom/cache/CacheStreamControlListener.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "nsCOMPtr.h"
#include "nsID.h"
#include "nsIInputStream.h"
#include "nsISupportsImpl.h"

class nsIThread;
template<class T> class nsTArray;

namespace mozilla {
namespace dom {
namespace cache {

class PCacheReadStream;
class PCacheReadStreamOrVoid;
class PCacheStreamControlParent;

// IID for the dom::cache::ReadStream interface
#define NS_DOM_CACHE_IID \
{0x8e5da7c9, 0x0940, 0x4f1d, \
  {0x97, 0x25, 0x5c, 0x59, 0x38, 0xdd, 0xb9, 0x9f}}

class ReadStream : public nsIInputStream
                 , public CacheStreamControlListener
{
public:
  static already_AddRefed<ReadStream>
  Create(const PCacheReadStreamOrVoid& aReadStreamOrVoid);

  static already_AddRefed<ReadStream>
  Create(const PCacheReadStream& aReadStream);

  static already_AddRefed<ReadStream>
  Create(PCacheStreamControlParent* aControl, const nsID& aId,
         nsIInputStream* aStream);

  void Serialize(PCacheReadStreamOrVoid* aReadStreamOut);
  void Serialize(PCacheReadStream* aReadStreamOut);

  // CacheStreamControlListener methods
  virtual void CloseStream() MOZ_OVERRIDE;
  virtual bool MatchId(const nsID& aId) MOZ_OVERRIDE;

protected:
  ReadStream(const nsID& aId, nsIInputStream* aStream);
  virtual ~ReadStream();

  void NoteClosed();
  void Forget();

  virtual void NoteClosedOnWorkerThread()=0;
  virtual void ForgetOnWorkerThread()=0;
  virtual void SerializeControl(PCacheReadStream* aReadStreamOut)=0;

  virtual void
  SerializeFds(PCacheReadStream* aReadStreamOut,
               const nsTArray<mozilla::ipc::FileDescriptor>& fds)=0;

  const nsID mId;
  nsCOMPtr<nsIInputStream> mStream;
  nsCOMPtr<nsIThread> mThread;
  bool mClosed;

public:
  class NoteClosedRunnable;
  class ForgetRunnable;

  NS_DECLARE_STATIC_IID_ACCESSOR(NS_DOM_CACHE_IID);

  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIINPUTSTREAM
};

NS_DEFINE_STATIC_IID_ACCESSOR(ReadStream, NS_DOM_CACHE_IID);

} // namespace cache
} // namespace dom
} // namespace mozilla

#endif // mozilla_dom_cache_ReadStream_h
