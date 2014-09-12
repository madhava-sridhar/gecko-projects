/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MessagePort.h"
#include "MessagePortData.h"

#include "MessageEvent.h"
#include "MessagePortChild.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/MessageChannel.h"
#include "mozilla/dom/MessagePortBinding.h"
#include "mozilla/dom/MessagePortList.h"
#include "mozilla/dom/StructuredCloneTags.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/WorkerScope.h"
#include "mozilla/ipc/BackgroundChild.h"
#include "mozilla/ipc/PBackgroundChild.h"
#include "nsContentUtils.h"
#include "nsGlobalWindow.h"
#include "nsPresContext.h"
#include "ScriptSettings.h"

#include "nsIDocument.h"
#include "nsIDOMFile.h"
#include "nsIDOMFileList.h"
#include "nsIPresShell.h"
#include "nsISupportsPrimitives.h"
#include "nsServiceManagerUtils.h"

using namespace mozilla::dom::workers;

namespace mozilla {
namespace dom {

/* TODO
1. postMessage from main-thread to workers
2. Blobs
*/

class DispatchEventRunnable : public nsCancelableRunnable
{
friend class MessagePort;

public:
  explicit DispatchEventRunnable(MessagePort* aPort)
    : mPort(aPort)
  { }

  NS_IMETHOD
  Run()
  {
    nsRefPtr<DispatchEventRunnable> mKungFuDeathGrip(this);

    mPort->mDispatchRunnable = nullptr;
    mPort->Dispatch();

    return NS_OK;
  }

private:
  nsRefPtr<MessagePort> mPort;
};

class PostMessageRunnable MOZ_FINAL : public nsCancelableRunnable
{
public:
  PostMessageRunnable(MessagePort* aPort, MessagePortData* aData)
    : mPort(aPort)
    , mData(aData)
  {
    MOZ_ASSERT(aPort);
    MOZ_ASSERT(aData);
  }

  NS_IMETHODIMP Run()
  {
    AutoJSAPI jsapi;
    nsCOMPtr<nsIGlobalObject> globalObject;

    if (NS_IsMainThread()) {
      globalObject = do_QueryInterface(mPort->GetParentObject());
    } else {
      WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
      MOZ_ASSERT(workerPrivate);
      globalObject = workerPrivate->GlobalScope();
    }

    if (!globalObject || !jsapi.Init(globalObject)) {
      NS_WARNING("Failed to initialize AutoJSAPI object.");
      return NS_ERROR_FAILURE;
    }

    JSContext* cx = jsapi.cx();

    nsTArray<nsRefPtr<MessagePort>> ports;
    nsCOMPtr<nsPIDOMWindow> window =
      do_QueryInterface(mPort->GetParentObject());

    JS::Rooted<JS::Value> value(cx, JS::NullValue());
    if (mData->mBuffer.nbytes() &&
        !ReadStructuredCloneWithTransfer(cx, mData->mBuffer.data(),
                                         mData->mBuffer.nbytes(),
                                         mData->mClosure, &value, window,
                                         ports)) {
      JS_ClearPendingException(cx);
      return NS_ERROR_FAILURE;
    }

    // Create the event
    nsCOMPtr<mozilla::dom::EventTarget> eventTarget =
      do_QueryInterface(mPort->GetOwner());
    nsRefPtr<MessageEvent> event =
      new MessageEvent(eventTarget, nullptr, nullptr);

    event->InitMessageEvent(NS_LITERAL_STRING("message"),
                            false /* non-bubbling */,
                            false /* cancelable */, value, EmptyString(),
                            EmptyString(), nullptr);
    event->SetTrusted(true);
    event->SetSource(mPort);

    nsTArray<nsRefPtr<MessagePortBase>> array;
    for (uint32_t i = 0; i < ports.Length(); ++i) {
      array.AppendElement(ports[i]);
    }

    nsRefPtr<MessagePortList> portList =
      new MessagePortList(static_cast<dom::Event*>(event.get()), array);
    event->SetPorts(portList);

    bool status;
    mPort->DispatchEvent(static_cast<dom::Event*>(event.get()), &status);
    return status ? NS_OK : NS_ERROR_FAILURE;
  }

private:
  nsRefPtr<MessagePort> mPort;
  nsRefPtr<MessagePortData> mData;
};

MessagePortBase::MessagePortBase(nsPIDOMWindow* aWindow)
  : DOMEventTargetHelper(aWindow)
{
  // SetIsDOMBinding() is called by DOMEventTargetHelper's ctor.
}

MessagePortBase::MessagePortBase()
{
  SetIsDOMBinding();
}

NS_IMPL_CYCLE_COLLECTION_CLASS(MessagePort)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN_INHERITED(MessagePort,
                                                DOMEventTargetHelper)
  if (tmp->mDispatchRunnable) {
    NS_IMPL_CYCLE_COLLECTION_UNLINK(mDispatchRunnable->mPort);
  }

  NS_IMPL_CYCLE_COLLECTION_UNLINK(mUnshippedEntangledPort);
NS_IMPL_CYCLE_COLLECTION_UNLINK_END

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(MessagePort,
                                                  DOMEventTargetHelper)
  if (tmp->mDispatchRunnable) {
    NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDispatchRunnable->mPort);
  }

  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mUnshippedEntangledPort);
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(MessagePort)
  NS_INTERFACE_MAP_ENTRY(nsIIPCBackgroundChildCreateCallback)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
NS_INTERFACE_MAP_END_INHERITING(DOMEventTargetHelper)

NS_IMPL_ADDREF_INHERITED(MessagePort, DOMEventTargetHelper)
NS_IMPL_RELEASE_INHERITED(MessagePort, DOMEventTargetHelper)

namespace {

class MessagePortFeature : public workers::WorkerFeature
{
  MessagePort* mPort;

public:
  MessagePortFeature(MessagePort* aPort)
    : mPort(aPort)
  {
    MOZ_ASSERT(aPort);
  }

  virtual bool Notify(JSContext* aCx, workers::Status aStatus) MOZ_OVERRIDE
  {
    if (aStatus >= Canceling) {
      WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
      MOZ_ASSERT(workerPrivate);

      workerPrivate->RemoveFeature(workerPrivate->GetJSContext(), this);
      mPort->Close();
    }

    return true;
  }
};

} // anonymous namespace

MessagePort::MessagePort(nsPIDOMWindow* aWindow, const nsID& aUUID,
                         const nsID& aDestinationUUID)
  : MessagePortBase(aWindow)
{
  Initialize(aUUID, aDestinationUUID, 1 /* 0 is an invalid sequence ID */,
             false /* Neutered */, eStateUnshippedEntangled);
}

MessagePort::MessagePort(nsPIDOMWindow* aWindow,
                         const MessagePortIdentifier& aIdentifier)
  : MessagePortBase(aWindow)
{
  Initialize(aIdentifier.mUUID, aIdentifier.mDestinationUUID,
             aIdentifier.mSequenceID, aIdentifier.mNeutered,
             eStateEntangling);
}

void
MessagePort::UnshippedEntangle(MessagePort* aEntangledPort)
{
  MOZ_ASSERT(aEntangledPort);
  MOZ_ASSERT(!mUnshippedEntangledPort);

  mUnshippedEntangledPort = aEntangledPort;
}

void
MessagePort::Initialize(const nsID& aUUID,
                        const nsID& aDestinationUUID,
                        uint32_t aSequenceID, bool mNeutered,
                        State aState)
{
  mUUID = aUUID;
  mDestinationUUID = aDestinationUUID;
  mSequenceID = aSequenceID;
  mMessageQueueEnabled = false;
  mState = aState;
  mNextStep = eNextStepNone;
  mIsKeptAlive = false;
  mInnerID = 0;

  if (mNeutered) {
    mState = eStateDisentangled;
  } else if (mState == eStateEntangling) {
    ConnectToPBackground();
  } else {
    MOZ_ASSERT(mState == eStateUnshippedEntangled);
  }

  // The port has to keep itself alive until it's entangled.
  UpdateMustKeepAlive();

  if (!NS_IsMainThread()) {
    WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
    MOZ_ASSERT(workerPrivate);

    MessagePortFeature* feature = new MessagePortFeature(this);
    JSContext* cx = workerPrivate->GetJSContext();
    if (NS_WARN_IF(!workerPrivate->AddFeature(cx, feature))) {
      NS_WARNING("Failed to register the BroadcastChannel worker feature.");
    }
  }

  // Register as observer for inner-window-destroyed.
  else {
    MOZ_ASSERT(GetOwner());
    MOZ_ASSERT(GetOwner()->IsInnerWindow());
    mInnerID = GetOwner()->WindowID();

    nsCOMPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
    if (obs) {
      obs->AddObserver(this, "inner-window-destroyed", false);
    }
  }
}

MessagePort::~MessagePort()
{
  Close();
}

JSObject*
MessagePort::WrapObject(JSContext* aCx)
{
  return MessagePortBinding::Wrap(aCx, this);
}

void
MessagePort::PostMessageMoz(JSContext* aCx, JS::Handle<JS::Value> aMessage,
                            const Optional<Sequence<JS::Value>>& aTransferable,
                            ErrorResult& aRv)
{
  nsRefPtr<MessagePortData> data = new MessagePortData();

  // We *must* clone the data here, or the JS::Value could be modified
  // by script

  JS::Rooted<JS::Value> transferable(aCx, JS::UndefinedValue());
  if (aTransferable.WasPassed()) {
    const Sequence<JS::Value>& realTransferable = aTransferable.Value();

    // The input sequence only comes from the generated bindings code, which
    // ensures it is rooted.
    JS::HandleValueArray elements =
      JS::HandleValueArray::fromMarkedLocation(realTransferable.Length(),
                                               realTransferable.Elements());

    JSObject* array =
      JS_NewArrayObject(aCx, elements);
    if (!array) {
      aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }
    transferable.setObject(*array);
  }

  if (!WriteStructuredCloneWithTransfer(aCx, aMessage, transferable,
                                        data->mBuffer, data->mClosure)) {
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
    return;
  }

  // Blobs are not supported yet!
  if (!data->mClosure.mBlobs.IsEmpty()) {
    aRv.Throw(NS_ERROR_DOM_DATA_CLONE_ERR);
    return;
  }

  // This message has to be ignored.
  if (mState > eStateEntangled) {
    return;
  }

  // If we are unshipped we are connected to the other port on the same thread.
  if (mState == eStateUnshippedEntangled) {
    MOZ_ASSERT(mUnshippedEntangledPort);
    mUnshippedEntangledPort->mMessageQueue.AppendElement(data);
    mUnshippedEntangledPort->Dispatch();
    return;
  }

  // Not entangled yet, but already closed.
  if (mNextStep != eNextStepNone) {
    return;
  }

  // Not entangled yet.
  if (mState != eStateEntangled) {
    mMessagePendingQueue.AppendElement(data);
    return;
  }

  MOZ_ASSERT(mActor);
  MOZ_ASSERT(mMessagePendingQueue.IsEmpty());

  nsTArray<nsRefPtr<MessagePortData>> array;
  array.AppendElement(data);

  nsTArray<MessagePortMessage> messages;
  MessagePortData::FromDataToMessages(array, messages);
  mActor->SendPostMessages(messages);
}

void
MessagePort::Start()
{
  if (mMessageQueueEnabled) {
    return;
  }

  mMessageQueueEnabled = true;
  Dispatch();
}

void
MessagePort::Dispatch()
{
  if (!mMessageQueueEnabled || mMessageQueue.IsEmpty() || mDispatchRunnable ||
      mState > eStateEntangled || mNextStep != eNextStepNone) {
    return;
  }

  nsRefPtr<MessagePortData> data = mMessageQueue.ElementAt(0);
  mMessageQueue.RemoveElementAt(0);

  nsRefPtr<PostMessageRunnable> runnable = new PostMessageRunnable(this, data);

  if (NS_FAILED(NS_DispatchToCurrentThread(runnable))) {
    NS_WARNING("Failed to dispatch to the current thread!");
  }

  mDispatchRunnable = new DispatchEventRunnable(this);

  if (NS_FAILED(NS_DispatchToCurrentThread(mDispatchRunnable))) {
    NS_WARNING("Failed to dispatch to the current thread!");
  }
}

void
MessagePort::Close()
{
  // Not entangled yet, but already closed.
  if (mNextStep != eNextStepNone) {
    return;
  }

  if (mState == eStateUnshippedEntangled) {
    MOZ_ASSERT(mUnshippedEntangledPort);

    // This avoids loops.
    nsRefPtr<MessagePort> port = mUnshippedEntangledPort;
    mUnshippedEntangledPort = nullptr;

    mState = eStateDisentangled;
    port->Close();

    UpdateMustKeepAlive();
    return;
  }

  // Not entangled yet, we have to wait.
  if (mState < eStateEntangled) {
    mNextStep = eNextStepClose;
    return;
  }

  if (mState > eStateEntangled) {
    return;
  }

  // We don't care about stopping the sending of messages because from now all
  // the incoming messages will be ingnored.
  mState = eStateDisentangled;

  MOZ_ASSERT(mActor);

  mActor->SendClose();
  mActor->SetPort(nullptr);
  mActor = nullptr;

  UpdateMustKeepAlive();
}

EventHandlerNonNull*
MessagePort::GetOnmessage()
{
  if (NS_IsMainThread()) {
    return GetEventHandler(nsGkAtoms::onmessage, EmptyString());
  }
  return GetEventHandler(nullptr, NS_LITERAL_STRING("message"));
}

void
MessagePort::SetOnmessage(EventHandlerNonNull* aCallback)
{
  if (NS_IsMainThread()) {
    SetEventHandler(nsGkAtoms::onmessage, EmptyString(), aCallback);
  } else {
    SetEventHandler(nullptr, NS_LITERAL_STRING("message"), aCallback);
  }

  // When using onmessage, the call to start() is implied.
  Start();
}

// This method is called when the PMessagePortChild actor is entangled to
// another actor. It receives a list of messages to be dispatch. It can be that
// we were waiting for this entangling step in order to disentangle the port or
// to close it.
void
MessagePort::Entangled(const nsTArray<MessagePortMessage>& aMessages)
{
  MOZ_ASSERT(mState == eStateEntangling);

  mState = eStateEntangled;

  // If we have pending messages, these have to be sent.
  if (!mMessagePendingQueue.IsEmpty()) {
    nsTArray<MessagePortMessage> messages;
    MessagePortData::FromDataToMessages(mMessagePendingQueue, messages);
    mActor->SendPostMessages(messages);
    mMessagePendingQueue.Clear();
  }

  if (mNextStep == eNextStepClose) {
    Close();
    return;
  }

  nsTArray<nsRefPtr<MessagePortData>> data;
  MessagePortData::FromMessagesToData(aMessages, data);
  mMessageQueue.AppendElements(data);

  // We were waiting for the entangling callback in order to disentangle this
  // port immediately after.
  if (mNextStep == eNextStepDisentangle) {
    StartDisentangling();
    return;
  }

  MOZ_ASSERT(mNextStep == eNextStepNone);
  Dispatch();
}

void
MessagePort::StartDisentangling()
{
  MOZ_ASSERT(mActor);
  MOZ_ASSERT(mState == eStateEntangled);

  mState = eStateDisentangling;
  mNextStep = eNextStepNone;

  // Sending this message we communicate to the parent actor that we don't want
  // to receive any new messages. It is possible that a message has been
  // already sent but not received yet. So we have to collect all of them and
  // we send them in the SendDispatch() request.
  mActor->SendStopSendingData();
}

void
MessagePort::MessagesReceived(const nsTArray<MessagePortMessage>& aMessages)
{
  MOZ_ASSERT(mState == eStateEntangled || mState == eStateDisentangling);
  MOZ_ASSERT(mNextStep == eNextStepNone);
  MOZ_ASSERT(mMessagePendingQueue.IsEmpty());

  nsTArray<nsRefPtr<MessagePortData>> data;
  MessagePortData::FromMessagesToData(aMessages, data);
  mMessageQueue.AppendElements(data);

  if (mState == eStateEntangled) {
    Dispatch();
  }
}

void
MessagePort::StopSendingDataConfirmed()
{
  MOZ_ASSERT(mState == eStateDisentangling);
  MOZ_ASSERT(mActor);

  Disentangle();
}

void
MessagePort::Disentangle()
{
  MOZ_ASSERT(mState == eStateDisentangling);
  MOZ_ASSERT(mActor);
 
  mState = eStateDisentangled;

  nsTArray<MessagePortMessage> messages;
  MessagePortData::FromDataToMessages(mMessageQueue, messages);
  mActor->SendDisentangle(messages);
  mMessageQueue.Clear();

  mActor->SetPort(nullptr);
  mActor = nullptr;

  UpdateMustKeepAlive();
}

bool
MessagePort::CloneAndDisentangle(MessagePortIdentifier& aIdentifier)
{
  aIdentifier.mNeutered = true;

  if (mState > eStateEntangled) {
    return true;
  }

  // We already have a 'next step'. We have to consider this port as already
  // cloned/closed/disentangled.
  if (mNextStep != eNextStepNone) {
    return true;
  }

  aIdentifier.mUUID = mUUID;
  aIdentifier.mDestinationUUID = mDestinationUUID;
  aIdentifier.mSequenceID = mSequenceID + 1;
  aIdentifier.mNeutered = false;

  // We have to entangle first.
  if (mState == eStateUnshippedEntangled) {
    MOZ_ASSERT(mUnshippedEntangledPort);
    MOZ_ASSERT(mMessagePendingQueue.IsEmpty());

    // Disconnect the entangled port and connect it to PBackground.
    mUnshippedEntangledPort->ConnectToPBackground();
    mUnshippedEntangledPort = nullptr;

    // In this case, we don't need to be connected to the PBackground service.
    if (mMessageQueue.IsEmpty()) {
      aIdentifier.mSequenceID = mSequenceID;

      // Disentangle can delete this object.
      nsRefPtr<MessagePort> kungFuDeathGrip(this);
      mState = eStateDisentangled;
      UpdateMustKeepAlive();
      return true;
    }

    // Register this component to PBackground.
    ConnectToPBackground();

    mNextStep = eNextStepDisentangle;
    return true;
  }

  // Not entangled yet, we have to wait.
  if (mState < eStateEntangled) {
    mNextStep = eNextStepDisentangle;
    return true;
  }

  StartDisentangling();
  return true;
}

void
MessagePort::Closed()
{
  if (mState == eStateDisentangled) {
    return;
  }

  mState = eStateDisentangled;

  if (mActor) {
    mActor->SetPort(nullptr);
    mActor = nullptr;
  }

  UpdateMustKeepAlive();
}

void
MessagePort::ConnectToPBackground()
{
  mozilla::ipc::BackgroundChild::GetOrCreateForCurrentThread(this);
  mState = eStateEntangling;
}

void
MessagePort::ActorFailed()
{
  MOZ_CRASH("Failed to create a PBackgroundChild actor!");
}

void
MessagePort::ActorCreated(mozilla::ipc::PBackgroundChild* aActor)
{
  MOZ_ASSERT(aActor);
  MOZ_ASSERT(mState == eStateEntangling);

  PMessagePortChild* actor = aActor->SendPMessagePortConstructor();

  mActor = static_cast<MessagePortChild*>(actor);
  MOZ_ASSERT(mActor);

  mActor->SetPort(this);

  mActor->SendEntangle(mUUID, mDestinationUUID, mSequenceID);
}

void
MessagePort::UpdateMustKeepAlive()
{
  if (mState == eStateDisentangled && mIsKeptAlive) {
    mIsKeptAlive = false;
    Release();
    return;
  }

  if (mState < eStateDisentangled && !mIsKeptAlive) {
    mIsKeptAlive = true;
    AddRef();
  }
}

NS_IMETHODIMP
MessagePort::Observe(nsISupports* aSubject, const char* aTopic,
                     const char16_t* aData)
{
  MOZ_ASSERT(NS_IsMainThread());

  if (strcmp(aTopic, "inner-window-destroyed")) {
    return NS_OK;
  }

  // If the window id destroyed we have to release the reference that we are
  // keeping.
  if (!mIsKeptAlive) {
    return NS_OK;
  }

  nsCOMPtr<nsISupportsPRUint64> wrapper = do_QueryInterface(aSubject);
  NS_ENSURE_TRUE(wrapper, NS_ERROR_FAILURE);

  uint64_t innerID;
  nsresult rv = wrapper->GetData(&innerID);
  NS_ENSURE_SUCCESS(rv, rv);

  if (innerID == mInnerID) {
    nsCOMPtr<nsIObserverService> obs =
      do_GetService("@mozilla.org/observer-service;1");
    if (obs) {
      obs->RemoveObserver(this, "inner-window-destroyed");
    }

    Close();
  }

  return NS_OK;
}

} // namespace dom
} // namespace mozilla
