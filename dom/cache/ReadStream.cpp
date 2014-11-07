/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/ReadStream.h"

#include "mozilla/unused.h"
#include "mozilla/dom/cache/CacheStreamControlChild.h"
#include "mozilla/dom/cache/CacheStreamControlParent.h"
#include "mozilla/dom/cache/PCacheStreamControlChild.h"
#include "mozilla/dom/cache/PCacheStreamControlParent.h"
#include "mozilla/dom/cache/PCacheTypes.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/FileDescriptorSetChild.h"
#include "mozilla/ipc/FileDescriptorSetParent.h"
#include "mozilla/ipc/InputStreamParams.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "mozilla/ipc/PBackgroundParent.h"
#include "mozilla/ipc/PFileDescriptorSetChild.h"
#include "mozilla/ipc/PFileDescriptorSetParent.h"
#include "nsIAsyncInputStream.h"
#include "nsTArray.h"

namespace {

using mozilla::unused;
using mozilla::void_t;
using mozilla::dom::cache::CacheStreamControlChild;
using mozilla::dom::cache::CacheStreamControlParent;
using mozilla::dom::cache::PCacheReadStream;
using mozilla::dom::cache::PCacheStreamControlChild;
using mozilla::dom::cache::PCacheStreamControlParent;
using mozilla::dom::cache::ReadStream;
using mozilla::ipc::FileDescriptor;
using mozilla::ipc::PFileDescriptorSetChild;
using mozilla::ipc::PFileDescriptorSetParent;

class ReadStreamChild MOZ_FINAL : public ReadStream
{
public:
  ReadStreamChild(PCacheStreamControlChild* aControl, const nsID& aId,
                  nsIInputStream* aStream)
    : ReadStream(aId, aStream)
    , mControl(static_cast<CacheStreamControlChild*>(aControl))
  {
    MOZ_ASSERT(mControl);
    mControl->AddListener(this);
  }

  virtual ~ReadStreamChild()
  {
    NoteClosed();
  }

  virtual void NoteClosed() MOZ_OVERRIDE
  {
    if (mClosed) {
      return;
    }

    mClosed = true;
    mControl->RemoveListener(this);
    mControl->NoteClosed(mId);
  }

  virtual void Forget() MOZ_OVERRIDE
  {
    if (mClosed) {
      return;
    }

    mClosed = true;
    mControl->RemoveListener(this);
  }

  virtual void SerializeControl(PCacheReadStream* aReadStreamOut) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aReadStreamOut);
    aReadStreamOut->controlParent() = nullptr;
    aReadStreamOut->controlChild() = mControl;
  }

  virtual void
  SerializeFds(PCacheReadStream* aReadStreamOut,
               const nsTArray<FileDescriptor>& fds) MOZ_OVERRIDE
  {
    PFileDescriptorSetChild* fdSet = nullptr;
    if (!fds.IsEmpty()) {
      fdSet = mControl->Manager()->SendPFileDescriptorSetConstructor(fds[0]);
      for (uint32_t i = 1; i < fds.Length(); ++i) {
        unused << fdSet->SendAddFileDescriptor(fds[i]);
      }
    }

    if (fdSet) {
      aReadStreamOut->fds() = fdSet;
    } else {
      aReadStreamOut->fds() = void_t();
    }
  }

private:
  CacheStreamControlChild* mControl;
};

class ReadStreamParent MOZ_FINAL : public ReadStream
{
public:
  ReadStreamParent(PCacheStreamControlParent* aControl, const nsID& aId,
                  nsIInputStream* aStream)
    : ReadStream(aId, aStream)
    , mControl(static_cast<CacheStreamControlParent*>(aControl))
  {
    MOZ_ASSERT(mControl);
    mControl->AddListener(this);
  }

  virtual ~ReadStreamParent()
  {
    NoteClosed();
  }

  virtual void NoteClosed() MOZ_OVERRIDE
  {
    if (mClosed) {
      return;
    }

    mClosed = true;
    mControl->RemoveListener(this);
    // This can cause mControl to be destructed
    mControl->RecvNoteClosed(mId);
    mControl = nullptr;
  }

  virtual void Forget() MOZ_OVERRIDE
  {
    if (mClosed) {
      return;
    }

    mClosed = true;
    // This can cause mControl to be destructed
    mControl->RemoveListener(this);
    mControl = nullptr;
  }

  virtual void SerializeControl(PCacheReadStream* aReadStreamOut) MOZ_OVERRIDE
  {
    MOZ_ASSERT(aReadStreamOut);
    MOZ_ASSERT(!mClosed);
    MOZ_ASSERT(mControl);
    aReadStreamOut->controlChild() = nullptr;
    aReadStreamOut->controlParent() = mControl;
  }

  virtual void
  SerializeFds(PCacheReadStream* aReadStreamOut,
               const nsTArray<FileDescriptor>& fds) MOZ_OVERRIDE
  {
    MOZ_ASSERT(!mClosed);
    MOZ_ASSERT(mControl);
    PFileDescriptorSetParent* fdSet = nullptr;
    if (!fds.IsEmpty()) {
      fdSet = mControl->Manager()->SendPFileDescriptorSetConstructor(fds[0]);
      for (uint32_t i = 1; i < fds.Length(); ++i) {
        unused << fdSet->SendAddFileDescriptor(fds[i]);
      }
    }

    if (fdSet) {
      aReadStreamOut->fds() = fdSet;
    } else {
      aReadStreamOut->fds() = void_t();
    }
  }

private:
  CacheStreamControlParent* mControl;
};

} // anonymous namespace

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::ipc::FileDescriptor;
using mozilla::ipc::FileDescriptorSetChild;
using mozilla::ipc::FileDescriptorSetParent;
using mozilla::ipc::InputStreamParams;
using mozilla::ipc::OptionalFileDescriptorSet;
using mozilla::ipc::PFileDescriptorSetChild;

NS_IMPL_ISUPPORTS(mozilla::dom::cache::ReadStream, nsIInputStream,
                                                   ReadStream);

// static
already_AddRefed<ReadStream>
ReadStream::Create(const PCacheReadStreamOrVoid& aReadStreamOrVoid)
{
  if (aReadStreamOrVoid.type() == PCacheReadStreamOrVoid::Tvoid_t) {
    return nullptr;
  }

  return Create(aReadStreamOrVoid.get_PCacheReadStream());
}

// static
already_AddRefed<ReadStream>
ReadStream::Create(const PCacheReadStream& aReadStream)
{
  if (!aReadStream.controlChild() && !aReadStream.controlParent()) {
    return nullptr;
  }

  nsTArray<FileDescriptor> fds;
  if (aReadStream.fds().type() ==
      OptionalFileDescriptorSet::TPFileDescriptorSetChild) {

    FileDescriptorSetChild* fdSetActor =
      static_cast<FileDescriptorSetChild*>(aReadStream.fds().get_PFileDescriptorSetChild());
    MOZ_ASSERT(fdSetActor);

    fdSetActor->ForgetFileDescriptors(fds);
    MOZ_ASSERT(!fds.IsEmpty());

    unused << fdSetActor->Send__delete__(fdSetActor);
  } else if (aReadStream.fds().type() ==
      OptionalFileDescriptorSet::TPFileDescriptorSetParent) {

    FileDescriptorSetParent* fdSetActor =
      static_cast<FileDescriptorSetParent*>(aReadStream.fds().get_PFileDescriptorSetParent());
    MOZ_ASSERT(fdSetActor);

    fdSetActor->ForgetFileDescriptors(fds);
    MOZ_ASSERT(!fds.IsEmpty());

    unused << fdSetActor->Send__delete__(fdSetActor);
  }

  nsCOMPtr<nsIInputStream> stream =
    DeserializeInputStream(aReadStream.params(), fds);

  if (!stream) {
    return nullptr;
  }

  // Currently we expect all cache read streams to be blocking file streams.
#ifdef DEBUG
  nsCOMPtr<nsIAsyncInputStream> asyncStream = do_QueryInterface(stream);
  MOZ_ASSERT(!asyncStream);
#endif

  nsRefPtr<ReadStream> ref;

  if (aReadStream.controlChild()) {
    ref = new ReadStreamChild(aReadStream.controlChild(), aReadStream.id(),
                              stream);
  } else {
    ref = new ReadStreamParent(aReadStream.controlParent(), aReadStream.id(),
                               stream);
  }

  return ref.forget();
}

// static
already_AddRefed<ReadStream>
ReadStream::Create(PCacheStreamControlParent* aControl, const nsID& aId,
                   nsIInputStream* aStream)
{
  nsRefPtr<ReadStream> ref = new ReadStreamParent(aControl, aId, aStream);
  return ref.forget();
}

void
ReadStream::Serialize(PCacheReadStreamOrVoid* aReadStreamOut)
{
  MOZ_ASSERT(aReadStreamOut);
  PCacheReadStream stream;
  Serialize(&stream);
  *aReadStreamOut = stream;
}

void
ReadStream::Serialize(PCacheReadStream* aReadStreamOut)
{
  MOZ_ASSERT(aReadStreamOut);
  MOZ_ASSERT(!mClosed);

  aReadStreamOut->id() = mId;
  SerializeControl(aReadStreamOut);

  nsTArray<FileDescriptor> fds;
  SerializeInputStream(mStream, aReadStreamOut->params(), fds);

  SerializeFds(aReadStreamOut, fds);

  // We're passing ownership across the IPC barrier with the control, so
  // do not signal that the stream is closed here.
  Forget();
}

void
ReadStream::CloseStream()
{
  Close();
}

bool
ReadStream::MatchId(const nsID& aId)
{
  return mId.Equals(aId);
}

ReadStream::ReadStream(const nsID& aId, nsIInputStream* aStream)
  : mId(aId)
  , mStream(aStream)
  , mClosed(false)
{
  MOZ_ASSERT(mStream);
}

ReadStream::~ReadStream()
{
}

NS_IMETHODIMP
ReadStream::Close()
{
  NoteClosed();
  return mStream->Close();
}

NS_IMETHODIMP
ReadStream::Available(uint64_t* aNumAvailableOut)
{
  nsresult rv = mStream->Available(aNumAvailableOut);

  if (NS_FAILED(rv)) {
    NoteClosed();
  }

  return rv;
}

NS_IMETHODIMP
ReadStream::Read(char* aBuf, uint32_t aCount, uint32_t* aNumReadOut)
{
  MOZ_ASSERT(aNumReadOut);

  nsresult rv = mStream->Read(aBuf, aCount, aNumReadOut);

  // Don't auto-close when end of stream is hit.  We want to close
  // this stream on a particular thread in the parent case.

  if (NS_FAILED(rv) && rv != NS_BASE_STREAM_WOULD_BLOCK) {
    NoteClosed();
  }

  return rv;
}

NS_IMETHODIMP
ReadStream::ReadSegments(nsWriteSegmentFun aWriter, void* aClosure,
                         uint32_t aCount, uint32_t* aNumReadOut)
{
  MOZ_ASSERT(aNumReadOut);

  nsresult rv = mStream->ReadSegments(aWriter, aClosure, aCount, aNumReadOut);

  // Don't auto-close when end of stream is hit.  We want to close
  // this stream on a particular thread in the parent case.

  if (NS_FAILED(rv) && rv != NS_BASE_STREAM_WOULD_BLOCK &&
                       rv != NS_ERROR_NOT_IMPLEMENTED) {
    NoteClosed();
  }

  return rv;
}

NS_IMETHODIMP
ReadStream::IsNonBlocking(bool* aNonBlockingOut)
{
  return mStream->IsNonBlocking(aNonBlockingOut);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
