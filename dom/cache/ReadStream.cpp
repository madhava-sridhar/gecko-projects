/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/ReadStream.h"

#include "mozilla/unused.h"
#include "mozilla/dom/cache/PCacheTypes.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/FileDescriptorSetChild.h"
#include "mozilla/ipc/InputStreamParams.h"
#include "mozilla/ipc/InputStreamUtils.h"
#include "nsIAsyncInputStream.h"
#include "nsTArray.h"

namespace mozilla {
namespace dom {
namespace cache {

using mozilla::unused;
using mozilla::ipc::FileDescriptor;
using mozilla::ipc::FileDescriptorSetChild;
using mozilla::ipc::InputStreamParams;
using mozilla::ipc::OptionalFileDescriptorSet;

NS_IMPL_ISUPPORTS(mozilla::dom::cache::ReadStream, nsIInputStream,
                                                   nsIIPCSerializableInputStream);

// static
already_AddRefed<ReadStream>
ReadStream::Create(PCacheStreamControlChild* aControl,
                   const PCacheReadStreamOrVoid& aReadStreamOrVoid)
{
  if (!aControl || aReadStreamOrVoid.type() == PCacheReadStreamOrVoid::Tvoid_t) {
    return nullptr;
  }

  return Create(aControl, aReadStreamOrVoid.get_PCacheReadStream());
}

// static
already_AddRefed<ReadStream>
ReadStream::Create(PCacheStreamControlChild* aControl,
                   const PCacheReadStream& aReadStream)
{
  MOZ_ASSERT(aControl);

  nsTArray<FileDescriptor> fds;
  if (aReadStream.fds().type() ==
      OptionalFileDescriptorSet::TPFileDescriptorSetChild) {

    FileDescriptorSetChild* fdSetActor =
      static_cast<FileDescriptorSetChild*>(aReadStream.fds().get_PFileDescriptorSetChild());
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

  nsRefPtr<ReadStream> ref = new ReadStream(aControl, aReadStream.id(), stream);
  return ref.forget();
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

ReadStream::ReadStream(PCacheStreamControlChild* aControl, const nsID& aId,
                       nsIInputStream* aStream)
  : mControl(static_cast<CacheStreamControlChild*>(aControl))
  , mId(aId)
  , mStream(aStream)
  , mClosed(false)
{
  MOZ_ASSERT(mControl);
  MOZ_ASSERT(mStream);

  mSerializable = do_QueryInterface(mStream);
  MOZ_ASSERT(mSerializable);

  mControl->AddListener(this);
}

ReadStream::~ReadStream()
{
  NoteClosed();
}

void
ReadStream::NoteClosed()
{
  if (mClosed) {
    return;
  }

  mClosed = true;
  mControl->NoteClosed(mId);
  mControl->RemoveListener(this);
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

  if ((NS_FAILED(rv) && rv != NS_BASE_STREAM_WOULD_BLOCK) ||
      *aNumReadOut == 0) {
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

  if ((NS_FAILED(rv) && rv != NS_BASE_STREAM_WOULD_BLOCK &&
                        rv != NS_ERROR_NOT_IMPLEMENTED) || *aNumReadOut == 0) {
    NoteClosed();
  }

  return rv;
}

NS_IMETHODIMP
ReadStream::IsNonBlocking(bool* aNonBlockingOut)
{
  return mStream->IsNonBlocking(aNonBlockingOut);
}

void
ReadStream::Serialize(InputStreamParams& aParams, FileDescriptorArray& aFds)
{
  // TODO: will lose track of when stream underlying fd closes... must we accept this?
  mSerializable->Serialize(aParams, aFds);
}

bool
ReadStream::Deserialize(const InputStreamParams& aParams,
                        const FileDescriptorArray& aFds)
{
  return mSerializable->Deserialize(aParams, aFds);
}

} // namespace cache
} // namespace dom
} // namespace mozilla
