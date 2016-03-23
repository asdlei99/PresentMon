//--------------------------------------------------------------------------------------
// Copyright 2015 Intel Corporation
// All Rights Reserved
//
// Permission is granted to use, copy, distribute and prepare derivative works of this
// software for any purpose and without fee, provided, that the above copyright notice
// and this statement appear in all copies.  Intel makes no representations about the
// suitability of this software for any purpose.  THIS SOFTWARE IS PROVIDED "AS IS."
// INTEL SPECIFICALLY DISCLAIMS ALL WARRANTIES, EXPRESS OR IMPLIED, AND ALL LIABILITY,
// INCLUDING CONSEQUENTIAL AND OTHER INDIRECT DAMAGES, FOR THE USE OF THIS SOFTWARE,
// INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PROPRIETARY RIGHTS, AND INCLUDING THE
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.  Intel does not
// assume any responsibility for any errors which may appear in this software nor any
// responsibility to update it.
//--------------------------------------------------------------------------------------

#include "PresentMon.hpp"
#include "TraceSession.hpp"

#include <cstdio>
#include <cassert>

#include <string>
#include <thread>
#include <tdh.h>
#include <dxgi.h>
#include <set>
#include <d3d9.h>
#include <algorithm>

struct __declspec(uuid("{CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}")) DXGI_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{802ec45a-1e99-4b83-9920-87c98277ba9d}")) DXGKRNL_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{8c416c79-d49b-4f01-a467-e56d3aa8234c}")) WIN32K_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{9e9bba3c-2e38-40cb-99f4-9e8281425164}")) DWM_PROVIDER_GUID_HOLDER;
struct __declspec(uuid("{783ACA0A-790E-4d7f-8451-AA850511C6B9}")) D3D9_PROVIDER_GUID_HOLDER;
static const auto DXGI_PROVIDER_GUID = __uuidof(DXGI_PROVIDER_GUID_HOLDER);
static const auto DXGKRNL_PROVIDER_GUID = __uuidof(DXGKRNL_PROVIDER_GUID_HOLDER);
static const auto WIN32K_PROVIDER_GUID = __uuidof(WIN32K_PROVIDER_GUID_HOLDER);
static const auto DWM_PROVIDER_GUID = __uuidof(DWM_PROVIDER_GUID_HOLDER);
static const auto D3D9_PROVIDER_GUID = __uuidof(D3D9_PROVIDER_GUID_HOLDER);

class TraceEventInfo
{
public:
    TraceEventInfo(PEVENT_RECORD pEvent)
    : pEvent(pEvent) {
        unsigned long bufferSize = 0;
        auto result = TdhGetEventInformation(pEvent, 0, nullptr, nullptr, &bufferSize);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            pInfo = reinterpret_cast<TRACE_EVENT_INFO*>(operator new(bufferSize));
            result = TdhGetEventInformation(pEvent, 0, nullptr, pInfo, &bufferSize);
        }
        if (result != ERROR_SUCCESS) {
            throw std::exception("Unexpected error from TdhGetEventInformation.", result);
        }
    }
    TraceEventInfo(const TraceEventInfo&) = delete;
    TraceEventInfo& operator=(const TraceEventInfo&) = delete;
    TraceEventInfo(TraceEventInfo&& o) {
        *this = std::move(o);
    }
    TraceEventInfo& operator=(TraceEventInfo&& o) {
        if (pInfo) {
            operator delete(pInfo);
        }
        pInfo = o.pInfo;
        pEvent = o.pEvent;
        o.pInfo = nullptr;
        return *this;
    }

    ~TraceEventInfo() {
        operator delete(pInfo);
        pInfo = nullptr;
    }

    void GetData(PCWSTR name, byte* outData, uint32_t dataSize) {
        PROPERTY_DATA_DESCRIPTOR descriptor;
        descriptor.ArrayIndex = 0;
        descriptor.PropertyName = reinterpret_cast<unsigned long long>(name);
        auto result = TdhGetProperty(pEvent, 0, nullptr, 1, &descriptor, dataSize, outData);
        if (result != ERROR_SUCCESS) {
            throw std::exception("Unexpected error from TdhGetProperty.", result);
        }
    }

    template <typename T>
    T GetData(PCWSTR name) {
        T local;
        GetData(name, reinterpret_cast<byte*>(&local), sizeof(local));
        return local;
    }

    uint64_t GetPtr(PCWSTR name) {
        if (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) {
            return GetData<uint32_t>(name);
        } else if (pEvent->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) {
            return GetData<uint64_t>(name);
        }
        return 0;
    }

private:
    TRACE_EVENT_INFO* pInfo;
    EVENT_RECORD* pEvent;
};

struct DxgiConsumer : ITraceConsumer
{
    CRITICAL_SECTION mMutex;
    // A set of presents that are "completed":
    // They progressed as far as they can through the pipeline before being either discarded or hitting the screen.
    // These will be handed off to the consumer thread.
    std::vector<std::shared_ptr<PresentEvent>> mCompletedPresents;

    // A high-level description of the sequence of events for each present type, ignoring runtime end:
    // Fullscreen:
    //   Runtime PresentStart -> Flip (by thread/process, for classification) -> QueueSubmit (by thread, for submit sequence) ->
    //    MMIOFlip (by submit sequence, for ready time and immediate flags) [-> VSyncDPC (by submit sequence, for screen time)]
    // Composed_Flip,
    //   Runtime PresentStart -> TokenCompositionSurfaceObject (by thread/process, for classification and token key) ->
    //    PresentHistoryDetailed (by thread, for token ptr) -> QueueSubmit (by thread, for submit sequence) ->
    //    PropagatePresentHistory (by token ptr, for ready time) and TokenStateChanged (by token key, for discard status and screen time)
    // DirectFlip,
    //   N/A, not currently uniquely detectable (follows the same path as composed_flip)
    // IndependentFlip,
    //   Follows composed flip, TokenStateChanged indicates IndependentFlip -> MMIOFlip (by submit sequence, for immediate flags) ->
    //   VSyncDPC (by submit sequence, for screen time)
    // ImmediateIndependentFlip,
    //   Identical to above, except MMIOFlip indicates immediate and screen time
    // IndependentFlipMPO,
    //   Identical to IndependentFlip, but MMIOFlipMPO is received instead
    // Windowed_Blit,
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> PresentHistoryDetailed (by thread, for token ptr and classification) ->
    //    DxgKrnl Present (by thread, for hWnd) -> PropagatePresentHistory (by token ptr, for ready time) ->
    //    DWM UpdateWindow (by hWnd, marks hWnd active for composition) -> DWM Present (consumes most recent present per hWnd, marks DWM thread ID) ->
    //    A fullscreen present is issued by DWM, and when it completes, this present is on screen
    // Fullscreen_Blit,
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> QueueSubmit (by thread, for submit sequence) ->
    //    QueueComplete (by submit sequence, indicates ready and screen time)
    //    Distinction between FS and windowed blt is done by LACK of other events
    // Legacy_Windowed_Blit (a.k.a. Vista Blit),
    //   Runtime PresentStart -> Blt (by thread/process, for classification) -> SubmitPresentHistory (by thread, for token ptr, legacy blit token, and classification) ->
    //    PropagatePresentHsitory (by token ptr, for ready time) -> DWM FlipChain (by legacy blit token, for hWnd and marks hWnd active for composition) ->
    //    Follows the Windowed_Blit path for tracking to screen
    // Composition_Buffer,
    //   SubmitPresentHistory (use model field for classification, get token ptr) -> PropagatePresentHistory (by token ptr) ->
    //    Assume DWM will compose this buffer on next present (missing InFrame event), follow windowed blit paths to screen time

    // Presents in the process of being submitted
    // The first map contains a single present that is currently in-between a set of expected events on the same thread:
    //   (e.g. DXGI_Present_Start/DXGI_Present_Stop, or Flip/QueueSubmit)
    // The second map contains a queue of presents currently pending for a process
    //   These presents have been "batched" and will be submitted by a driver worker thread
    //   The assumption is that they will be submitted to kernel in the same order they were submitted to DXGI,
    //   but this might not hold true especially if there are multiple D3D devices in play
    // Used for mapping from runtime events to future events, and thread map used extensively for correlating kernel events
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPresentByThreadId;
    std::map<uint32_t, std::deque<std::shared_ptr<PresentEvent>>> mPresentsByProcess;

    // Maps from queue packet submit sequence
    // Used for Flip -> MMIOFlip -> VSyncDPC for FS, for PresentHistoryToken -> MMIOFlip -> VSyncDPC for iFlip,
    // and for Blit Submission -> Blit completion for FS Blit
    std::map<uint32_t, std::shared_ptr<PresentEvent>> mPresentsBySubmitSequence;

    // Win32K present history tokens are uniquely identified by (composition surface pointer, present count, bind id)
    // Using a tuple instead of named struct simply to have auto-generated comparison operators
    // These tokens are used for "flip model" presents (windowed flip, dFlip, iFlip) only
    typedef std::tuple<uint64_t, uint64_t, uint32_t> Win32KPresentHistoryTokenKey;
    std::map<Win32KPresentHistoryTokenKey, std::shared_ptr<PresentEvent>> mWin32KPresentHistoryTokens;

    // DxgKrnl present history tokens are uniquely identified by a single pointer
    // These are used for all types of windowed presents to track a "ready" time
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mDxgKrnlPresentHistoryTokens;

    // Present by window, used for determining superceding presents
    // For windowed blit presents, when DWM issues a present event, we choose the most recent event as the one that will make it to screen
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentByWindow;

    // Presents that will be completed by DWM's next present
    std::deque<std::shared_ptr<PresentEvent>> mPresentsWaitingForDWM;
    // Used to understand that a flip event is coming from the DWM
    uint32_t DwmPresentThreadId = 0;

    // Windows that will be composed the next time DWM presents
    // Generated by DWM events indicating it's received tokens targeting a given hWnd
    std::set<uint32_t> mWindowsBeingComposed;

    // Yet another unique way of tracking present history tokens, this time from DxgKrnl -> DWM, only for legacy blit
    std::map<uint64_t, std::shared_ptr<PresentEvent>> mPresentsByLegacyBlitToken;

    bool DequeuePresents(std::vector<std::shared_ptr<PresentEvent>>& outPresents)
    {
        if (mCompletedPresents.size())
        {
            EnterCriticalSection(&mMutex);
            outPresents.swap(mCompletedPresents);
            LeaveCriticalSection(&mMutex);
            return !outPresents.empty();
        }
        return false;
    }

    DxgiConsumer() {
        InitializeCriticalSection(&mMutex);
    }
    ~DxgiConsumer() {
        DeleteCriticalSection(&mMutex);
    }

    virtual void OnEventRecord(_In_ PEVENT_RECORD pEventRecord);
    virtual bool ContinueProcessing() { return !g_Quit; }

private:
    void CompletePresent(std::shared_ptr<PresentEvent> p)
    {
#if _DEBUG
        p->Completed = true;
#endif

        // Complete all other presents that were riding along with this one (i.e. this one came from DWM)
        for (auto& p2 : p->DependentPresents) {
            p2->ScreenTime = p->ScreenTime;
            CompletePresent(p2);
        }
        p->DependentPresents.clear();

        // Remove it from any tracking maps that it may have been inserted into
        if (p->QueueSubmitSequence != 0) {
            mPresentsBySubmitSequence.erase(p->QueueSubmitSequence);
        }
        if (p->Hwnd != 0) {
            auto hWndIter = mPresentByWindow.find(p->Hwnd);
            if (hWndIter->second == p) {
                mPresentByWindow.erase(hWndIter);
            }
        }

        EnterCriticalSection(&mMutex);
        mCompletedPresents.push_back(p);
        LeaveCriticalSection(&mMutex);
    }

    decltype(mPresentByThreadId.begin()) FindOrCreatePresent(_In_ PEVENT_RECORD pEventRecord)
    {
        // Easy: we're on a thread that had some step in the present process
        auto eventIter = mPresentByThreadId.find(pEventRecord->EventHeader.ThreadId);
        if (eventIter != mPresentByThreadId.end()) {
            return eventIter;
        }

        // No such luck, check for batched presents
        auto& processDeque = mPresentsByProcess[pEventRecord->EventHeader.ProcessId];
        uint64_t EventTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
        if (processDeque.empty()) {
            // This likely didn't originate from a runtime whose events we're tracking (DXGI/D3D9)
            // Could be composition buffers, or maybe another runtime (e.g. GL)
            auto newEvent = std::make_shared<PresentEvent>();
            newEvent->QpcTime = EventTime;
            newEvent->ProcessId = pEventRecord->EventHeader.ProcessId;
            eventIter = mPresentByThreadId.emplace(pEventRecord->EventHeader.ThreadId, newEvent).first;
        } else {
            // Assume batched presents are popped off the front of the driver queue by process in order, do the same here
            assert(processDeque.front()->QpcTime < EventTime);
            eventIter = mPresentByThreadId.emplace(pEventRecord->EventHeader.ThreadId, processDeque.front()).first;
            processDeque.pop_front();
        }

        return eventIter;
    }

    void RuntimePresentStop(_In_ PEVENT_RECORD pEventRecord, bool AllowPresentBatching = true)
    {
        auto& hdr = pEventRecord->EventHeader;
        uint64_t EndTime = *(uint64_t*)&hdr.TimeStamp;
        auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
        if (eventIter == mPresentByThreadId.end()) {
            return;
        }
        auto &event = *eventIter->second;

        assert(event.QpcTime < EndTime);
        event.TimeTaken = EndTime - event.QpcTime;

        // PresentMode unknown means we didn't get any other events between the start and stop events which would help us classify
        // That either means the driver is batching it, or the present was dropped (e.g. DoNotWait would've waited)
        if (event.PresentMode == PresentMode::Unknown) {
            if (AllowPresentBatching) {
                auto& eventDeque = mPresentsByProcess[hdr.ProcessId];
                eventDeque.push_back(eventIter->second);

#if _DEBUG
                auto& container = mPresentsByProcess[hdr.ProcessId];
                for (UINT i = 0; i < container.size() - 1; ++i)
                {
                    assert(container[i]->QpcTime <= container[i + 1]->QpcTime);
                }
#endif

                // We'll give the driver some time to submit this present
                // In a batching scenario, the deque should grow to a steady state, and then we'll start popping off the front during FindOrCreatePresent above
                const uint32_t cMaxPendingPresents = 20;
                if (eventDeque.size() > cMaxPendingPresents)
                {
                    CompletePresent(eventDeque.front());
                    eventDeque.pop_front();
                }
            } else {
                CompletePresent(eventIter->second);
            }
        }
    }

    void OnDXGIEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnDXGKrnlEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnWin32kEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnDWMEvent(_In_ PEVENT_RECORD pEventRecord);
    void OnD3D9Event(_In_ PEVENT_RECORD pEventRecord);
};

void DxgiConsumer::OnDXGIEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DXGIPresent_Start = 42,
        DXGIPresent_Stop,
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    switch (hdr.EventDescriptor.Id)
    {
        case DXGIPresent_Start:
        {
            PresentEvent event;
            event.ProcessId = hdr.ProcessId;
            event.QpcTime = EventTime;
        
            TraceEventInfo eventInfo(pEventRecord);
            event.SwapChainAddress = eventInfo.GetPtr(L"pIDXGISwapChain");
            event.SyncInterval = eventInfo.GetData<uint32_t>(L"SyncInterval");
            event.PresentFlags = eventInfo.GetData<uint32_t>(L"Flags");
            event.Runtime = Runtime::DXGI;
        
            // Ignore PRESENT_TEST: it's just to check if you're still fullscreen, doesn't actually do anything
            if ((event.PresentFlags & DXGI_PRESENT_TEST) == 0) {
                mPresentByThreadId[hdr.ThreadId] = std::make_shared<PresentEvent>(event);
            }

#if _DEBUG
            event.Completed = true;
#endif
            break;
        }
        case DXGIPresent_Stop:
        {
            RuntimePresentStop(pEventRecord);
            break;
        }
    }
}

void DxgiConsumer::OnDXGKrnlEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DxgKrnl_Flip = 168,
        DxgKrnl_FlipMPO = 252,
        DxgKrnl_QueueSubmit = 178,
        DxgKrnl_QueueComplete = 180,
        DxgKrnl_MMIOFlip = 116,
        DxgKrnl_MMIOFlipMPO = 259,
        DxgKrnl_VSyncDPC = 17,
        DxgKrnl_Present = 184,
        DxgKrnl_PresentHistoryDetailed = 215,
        DxgKrnl_SubmitPresentHistory = 171,
        DxgKrnl_PropagatePresentHistory = 172,
        DxgKrnl_Blit = 166,
    };

    auto& hdr = pEventRecord->EventHeader;

    // Skip constructing the TraceEventInfo if this isn't an event we recognize (helps avoid dropping events by being too slow)
    switch (hdr.EventDescriptor.Id) 
    {
    case DxgKrnl_Flip:
    case DxgKrnl_FlipMPO:
    case DxgKrnl_QueueSubmit:
    case DxgKrnl_QueueComplete:
    case DxgKrnl_MMIOFlip:
    case DxgKrnl_MMIOFlipMPO:
    case DxgKrnl_VSyncDPC:
    case DxgKrnl_Present:
    case DxgKrnl_PresentHistoryDetailed:
    case DxgKrnl_SubmitPresentHistory:
    case DxgKrnl_PropagatePresentHistory:
    case DxgKrnl_Blit:
        break;
    default:
        return;
    }

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    TraceEventInfo eventInfo(pEventRecord);
    switch (hdr.EventDescriptor.Id)
    {
        case DxgKrnl_Flip:
        case DxgKrnl_FlipMPO:
        {
            // A flip event is emitted during fullscreen present submission.
            // Afterwards, expect an MMIOFlip packet on the same thread, used
            // to trace the flip to screen.
            auto eventIter = FindOrCreatePresent(pEventRecord);

            if (eventIter->second->PresentMode != PresentMode::Unknown) {
                // For MPO, N events may be issued, but we only care about the first
                return;
            }
            
            eventIter->second->PresentMode = PresentMode::Fullscreen;
            if (eventIter->second->Runtime != Runtime::DXGI) {
                // Only DXGI gives us the sync interval in the runtime present start event
                eventIter->second->SyncInterval = eventInfo.GetData<uint32_t>(L"FlipInterval");
            }

            // If this is the DWM thread, piggyback these pending presents on our fullscreen present
            if (hdr.ThreadId == DwmPresentThreadId) {
                std::swap(eventIter->second->DependentPresents, mPresentsWaitingForDWM);
                DwmPresentThreadId = 0;
            }
            break;
        }
        case DxgKrnl_QueueSubmit:
        {
            // A QueueSubmit can be many types, but these are interesting for present.
            // This event is emitted after a flip/blt/PHT event, and may be the only way
            // to trace completion of the present.
            enum class DxgKrnl_QueueSubmit_Type {
                MMIOFlip = 3,
                Software = 7,
            };
            auto Type = eventInfo.GetData<DxgKrnl_QueueSubmit_Type>(L"PacketType");
            auto SubmitSequence = eventInfo.GetData<uint32_t>(L"SubmitSequence");
            bool Present = eventInfo.GetData<BOOL>(L"bPresent") != 0;

            if (Type == DxgKrnl_QueueSubmit_Type::MMIOFlip ||
                Type == DxgKrnl_QueueSubmit_Type::Software ||
                Present) {
                auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
                if (eventIter == mPresentByThreadId.end()) {
                    return;
                }

                if (eventIter->second->QueueSubmitSequence == 0) {
                    eventIter->second->QueueSubmitSequence = SubmitSequence;
                    mPresentsBySubmitSequence.emplace(SubmitSequence, eventIter->second);
                }
            }

            break;
        }
        case DxgKrnl_QueueComplete:
        {
            auto SubmitSequence = eventInfo.GetData<uint32_t>(L"SubmitSequence");
            auto eventIter = mPresentsBySubmitSequence.find(SubmitSequence);
            if (eventIter == mPresentsBySubmitSequence.end()) {
                return;
            }

            auto pEvent = eventIter->second;
            //if (pEvent->PresentMode != PresentMode::Fullscreen &&
            //    pEvent->PresentMode != PresentMode::IndependentFlip) {
            //    mPresentsBySubmitSequence.erase(eventIter);
            //    pEvent->QueueSubmitSequence = 0;
            //}

            if (pEvent->PresentMode == PresentMode::Fullscreen_Blit) {
                pEvent->ScreenTime = pEvent->ReadyTime = EventTime;
                pEvent->FinalState = PresentResult::Presented;
                CompletePresent(pEvent);
            }
            break;
        }
        case DxgKrnl_MMIOFlip:
        {
            // An MMIOFlip event is emitted when an MMIOFlip packet is dequeued.
            // This corresponds to all GPU work prior to the flip being completed
            // (i.e. present "ready")
            // It also is emitted when an independent flip PHT is dequed,
            // and will tell us whether the present is immediate or vsync.
            enum DxgKrnl_MMIOFlip_Flags {
                FlipImmediate = 0x2,
                FlipOnNextVSync = 0x4
            };

            auto FlipSubmitSequence = eventInfo.GetData<uint32_t>(L"FlipSubmitSequence");
            auto Flags = eventInfo.GetData<DxgKrnl_MMIOFlip_Flags>(L"Flags");

            auto eventIter = mPresentsBySubmitSequence.find(FlipSubmitSequence);
            if (eventIter == mPresentsBySubmitSequence.end()) {
                return;
            }

            eventIter->second->ReadyTime = EventTime;

            if (Flags & DxgKrnl_MMIOFlip_Flags::FlipImmediate) {
                eventIter->second->FinalState = PresentResult::Presented;
                eventIter->second->ScreenTime = *(uint64_t*)&pEventRecord->EventHeader.TimeStamp;
                if (eventIter->second->PresentMode == PresentMode::Fullscreen) {
                    CompletePresent(eventIter->second);
                } else {
                    // We'll let the Win32K token discard event trigger the Complete for this one
                    eventIter->second->PresentMode = PresentMode::ImmediateIndependentFlip;
                }
            }

            break;
        }
        case DxgKrnl_MMIOFlipMPO:
        {
            // See above for more info about this packet.
            // Note: this packet currently does not support immediate flips
            auto FlipFenceId = eventInfo.GetData<uint64_t>(L"FlipSubmitSequence");
            uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);

            auto eventIter = mPresentsBySubmitSequence.find(FlipSubmitSequence);
            if (eventIter == mPresentsBySubmitSequence.end()) {
                return;
            }

            // Avoid double-marking a single present packet coming from the MPO API
            if (eventIter->second->ReadyTime == 0) {
                eventIter->second->ReadyTime = EventTime;
                eventIter->second->PlaneIndex = eventInfo.GetData<uint32_t>(L"LayerIndex");
            }

            if (eventIter->second->PresentMode == PresentMode::IndependentFlip ||
                eventIter->second->PresentMode == PresentMode::Composed_Flip) {
                eventIter->second->PresentMode = PresentMode::IndependentFlipMPO;
            }

            break;
        }
        case DxgKrnl_VSyncDPC:
        {
            // The VSyncDPC contains a field telling us what flipped to screen.
            // This is the way to track completion of a fullscreen present.
            auto FlipFenceId = eventInfo.GetData<uint64_t>(L"FlipFenceId");

            uint32_t FlipSubmitSequence = (uint32_t)(FlipFenceId >> 32u);
            auto eventIter = mPresentsBySubmitSequence.find(FlipSubmitSequence);
            if (eventIter == mPresentsBySubmitSequence.end()) {
                return;
            }
            
            eventIter->second->ScreenTime = EventTime;
            eventIter->second->FinalState = PresentResult::Presented;
            if (eventIter->second->PresentMode == PresentMode::Fullscreen) {
                CompletePresent(eventIter->second);
            }
            break;
        }
        case DxgKrnl_Present:
        {
            // This event is emitted at the end of the kernel present, before returning.
            // All other events have already been logged, but this one contains one
            // extra piece of useful information: the hWnd that a present targeted,
            // used to determine when presents are discarded instead of composed.
            auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
            if (eventIter == mPresentByThreadId.end()) {
                return;
            }

            auto hWnd = eventInfo.GetPtr(L"hWindow");

            if (eventIter->second->PresentMode == PresentMode::Windowed_Blit) {
                // Manipulate the map here
                // When DWM is ready to present, we'll query for the most recent blt targeting this window and take it out of the map
                auto hWndIter = mPresentByWindow.find(hWnd);
                if (hWndIter == mPresentByWindow.end()) {
                    mPresentByWindow.emplace(hWnd, eventIter->second);
                } else if (hWndIter->second != eventIter->second) {
                    auto eventPtr = hWndIter->second;
                    hWndIter->second = eventIter->second;

                    eventPtr->FinalState = PresentResult::Discarded;
                    if (eventPtr->ReadyTime != 0) {
                        // This won't make it to screen, go ahead and complete it now
                        CompletePresent(eventPtr);
                    }
                }
            } else {
                // For all other events, just remember the hWnd, we might need it later
                eventIter->second->Hwnd = hWnd;
            }

            if (eventIter->second->TimeTaken != 0 || eventIter->second->Runtime == Runtime::Other) {
                mPresentByThreadId.erase(eventIter);
            }
            break;
        }
        case DxgKrnl_PresentHistoryDetailed:
        {
            // This event is emitted during submission of most windowed presents.
            // In the case of flip and blit model, it is used to find a key to watch for the
            // event which triggers the "ready" state.
            // In the case of blit model, it is also used to distinguish between fs/windowed.
            auto eventIter = mPresentByThreadId.find(hdr.ThreadId);
            if (eventIter == mPresentByThreadId.end()) {
                return;
            }

            if (eventIter->second->PresentMode == PresentMode::Fullscreen_Blit) {
                eventIter->second->PresentMode = PresentMode::Windowed_Blit;
            }
            uint64_t TokenPtr = eventInfo.GetPtr(L"Token");
            mDxgKrnlPresentHistoryTokens[TokenPtr] = eventIter->second;
            break;
        }
        case DxgKrnl_SubmitPresentHistory:
        {
            // This event is emitted during submission of other types of windowed presents.
            // It gives us up to two different types of keys to correlate further.
            auto eventIter = FindOrCreatePresent(pEventRecord);

            if (eventIter->second->PresentMode == PresentMode::Fullscreen_Blit) {
                auto TokenData = eventInfo.GetData<uint64_t>(L"TokenData");
                mPresentsByLegacyBlitToken[TokenData] = eventIter->second;

                eventIter->second->ReadyTime = EventTime;
                eventIter->second->PresentMode = PresentMode::Legacy_Windowed_Blit;
            } else if (eventIter->second->PresentMode == PresentMode::Unknown) {
                enum class TokenModel {
                    Composition = 7,
                };

                auto Model = eventInfo.GetData<TokenModel>(L"Model");
                if (Model == TokenModel::Composition) {
                    eventIter->second->PresentMode = PresentMode::Composition_Buffer;
                    uint64_t TokenPtr = eventInfo.GetPtr(L"Token");
                    mDxgKrnlPresentHistoryTokens[TokenPtr] = eventIter->second;
                }
            }

            if (eventIter->second->Runtime == Runtime::Other ||
                eventIter->second->PresentMode == PresentMode::Composition_Buffer)
            {
                // We're not expecting any other events from this thread (no DxgKrnl Present or EndPresent runtime event)
                mPresentByThreadId.erase(eventIter);
            }
            break;
        }
        case DxgKrnl_PropagatePresentHistory:
        {
            // This event is emitted when a token is being handed off to DWM, and is a good way to indicate a ready state
            uint64_t TokenPtr = eventInfo.GetPtr(L"Token");
            auto eventIter = mDxgKrnlPresentHistoryTokens.find(TokenPtr);
            if (eventIter == mDxgKrnlPresentHistoryTokens.end()) {
                return;
            }
            eventIter->second->ReadyTime = EventTime;

            if (eventIter->second->FinalState == PresentResult::Discarded &&
                eventIter->second->PresentMode == PresentMode::Windowed_Blit) {
                // This won't make it to screen, go ahead and complete it now
                CompletePresent(eventIter->second);
            } else if (eventIter->second->PresentMode == PresentMode::Composition_Buffer) {
                mPresentsWaitingForDWM.emplace_back(eventIter->second);
            }

            mDxgKrnlPresentHistoryTokens.erase(eventIter);
            break;
        }
        case DxgKrnl_Blit:
        {
            auto eventIter = FindOrCreatePresent(pEventRecord);

            eventIter->second->PresentMode = PresentMode::Fullscreen_Blit;
            break;
        }
    }
}

void DxgiConsumer::OnWin32kEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        Win32K_TokenCompositionSurfaceObject = 201,
        Win32K_TokenStateChanged = 301,
    };

    auto& hdr = pEventRecord->EventHeader;
    // Skip constructing the TraceEventInfo if this isn't an event we recognize (helps avoid dropping events by being too slow)
    switch (hdr.EventDescriptor.Id) 
    {
    case Win32K_TokenCompositionSurfaceObject:
    case Win32K_TokenStateChanged:
        break;
    default:
        return;
    }

    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    TraceEventInfo eventInfo(pEventRecord);

    switch (hdr.EventDescriptor.Id)
    {
        case Win32K_TokenCompositionSurfaceObject:
        {
            auto eventIter = FindOrCreatePresent(pEventRecord);
            eventIter->second->PresentMode = PresentMode::Composed_Flip;

            Win32KPresentHistoryTokenKey key(eventInfo.GetPtr(L"pCompositionSurfaceObject"),
                                             eventInfo.GetData<uint64_t>(L"PresentCount"),
                                             eventInfo.GetData<uint32_t>(L"SwapChainIndex"));
            mWin32KPresentHistoryTokens[key] = eventIter->second;
            break;
        }
        case Win32K_TokenStateChanged:
        {
            Win32KPresentHistoryTokenKey key(eventInfo.GetPtr(L"pCompositionSurfaceObject"),
                                             eventInfo.GetData<uint32_t>(L"PresentCount"),
                                             eventInfo.GetData<uint32_t>(L"SwapChainIndex"));
            auto eventIter = mWin32KPresentHistoryTokens.find(key);
            if (eventIter == mWin32KPresentHistoryTokens.end()) {
                return;
            }

            enum class TokenState {
                InFrame = 3,
                Confirmed = 4,
                Retired = 5,
                Discarded = 6,
            };
            
            auto &event = *eventIter->second;
            auto state = eventInfo.GetData<TokenState>(L"NewState");
            switch (state)
            {
                case TokenState::InFrame:
                {
                    // InFrame = composition is starting
                    if (event.Hwnd) {
                        auto hWndIter = mPresentByWindow.find(event.Hwnd);
                        if (hWndIter == mPresentByWindow.end()) {
                            mPresentByWindow.emplace(event.Hwnd, eventIter->second);
                        } else if (hWndIter->second != eventIter->second) {
                            hWndIter->second->FinalState = PresentResult::Discarded;
                            hWndIter->second = eventIter->second;
                        }
                    }

                    bool iFlip = eventInfo.GetData<BOOL>(L"IndependentFlip") != 0;
                    if (iFlip && event.PresentMode == PresentMode::Composed_Flip) {
                        event.PresentMode = PresentMode::IndependentFlip;
                    }

                    break;
                }
                case TokenState::Confirmed:
                {
                    // Confirmed = present has been submitted
                    // If we haven't already decided we're going to discard a token, now's a good time to indicate it'll make it to screen
                    if (event.FinalState == PresentResult::Unknown) {
                        if (event.PresentFlags & DXGI_PRESENT_DO_NOT_SEQUENCE) {
                            // DO_NOT_SEQUENCE presents may get marked as confirmed,
                            // if a frame was composed when this token was completed
                            event.FinalState = PresentResult::Discarded;
                        } else {
                            event.FinalState = PresentResult::Presented;
                        }
                    }
                    break;
                }
                case TokenState::Retired:
                {
                    // Retired = present has been completed, token's buffer is now displayed
                    event.ScreenTime = EventTime;
                    break;
                }
                case TokenState::Discarded:
                {
                    // Discarded = destroyed - discard if we never got any indication that it was going to screen
                    auto sharedPtr = eventIter->second;
                    mWin32KPresentHistoryTokens.erase(eventIter);

                    if (event.FinalState == PresentResult::Unknown) {
                        event.FinalState = PresentResult::Discarded;
                    }

                    CompletePresent(sharedPtr);
                    break;
                }
            }
            break;
        }
    }
}

void DxgiConsumer::OnDWMEvent(PEVENT_RECORD pEventRecord)
{
    enum {
        DWM_DwmUpdateWindow = 46,
        DWM_Schedule_Present_Start = 15,
        DWM_FlipChain_Pending = 69,
        DWM_FlipChain_Complete = 70,
        DWM_FlipChain_Dirty = 101,
    };

    auto& hdr = pEventRecord->EventHeader;
    // Skip constructing the TraceEventInfo if this isn't an event we recognize (helps avoid dropping events by being too slow)
    switch (hdr.EventDescriptor.Id)
    {
    case DWM_DwmUpdateWindow:
    case DWM_Schedule_Present_Start:
    case DWM_FlipChain_Pending:
    case DWM_FlipChain_Complete:
    case DWM_FlipChain_Dirty:
        break;
    default:
        return;
    }

    TraceEventInfo eventInfo(pEventRecord);
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;

    switch (hdr.EventDescriptor.Id)
    {
        case DWM_DwmUpdateWindow:
        {
            auto hWnd = (uint32_t)eventInfo.GetData<uint64_t>(L"hWnd");

            // Piggyback on the next DWM present
            mWindowsBeingComposed.insert(hWnd);
            break;
        }
        case DWM_Schedule_Present_Start:
        {
            DwmPresentThreadId = hdr.ThreadId;
            for (auto hWnd : mWindowsBeingComposed)
            {
                // Pickup the most recent present from a given window
                auto hWndIter = mPresentByWindow.find(hWnd);
                if (hWndIter != mPresentByWindow.end()) {
                    if (hWndIter->second->PresentMode != PresentMode::Windowed_Blit &&
                        hWndIter->second->PresentMode != PresentMode::Legacy_Windowed_Blit) {
                        continue;
                    }
                    mPresentsWaitingForDWM.emplace_back(hWndIter->second);
                    hWndIter->second->FinalState = PresentResult::Presented;
                    mPresentByWindow.erase(hWndIter);
                }
            }
            mWindowsBeingComposed.clear();
            break;
        }
        case DWM_FlipChain_Pending:
        case DWM_FlipChain_Complete:
        case DWM_FlipChain_Dirty:
        {
            // As it turns out, the 64-bit token data from the PHT submission is actually two 32-bit data chunks,
            // corresponding to a "flip chain" id and present id
            uint32_t flipChainId = (uint32_t)eventInfo.GetData<uint64_t>(L"ulFlipChain");
            uint32_t serialNumber = (uint32_t)eventInfo.GetData<uint64_t>(L"ulSerialNumber");
            uint64_t token = ((uint64_t)flipChainId << 32ull) | serialNumber;
            auto flipIter = mPresentsByLegacyBlitToken.find(token);
            if (flipIter == mPresentsByLegacyBlitToken.end()) {
                return;
            }

            // Watch for multiple legacy blits completing against the same window
            auto hWnd = (uint32_t)eventInfo.GetData<uint64_t>(L"hwnd");
            auto hWndIter = mPresentByWindow.find(hWnd);
            if (hWndIter == mPresentByWindow.end()) {
                mPresentByWindow.emplace(hWnd, flipIter->second);
            } else if (hWndIter->second != flipIter->second) {
                auto eventPtr = hWndIter->second;
                hWndIter->second = flipIter->second;

                eventPtr->FinalState = PresentResult::Discarded;
                CompletePresent(eventPtr);
            }

            mPresentsByLegacyBlitToken.erase(flipIter);
            mWindowsBeingComposed.insert(hWnd);
            break;
        }
    }
}

void DxgiConsumer::OnD3D9Event(PEVENT_RECORD pEventRecord)
{
    enum {
        D3D9PresentStart = 1,
        D3D9PresentStop,
    };

    auto& hdr = pEventRecord->EventHeader;
    uint64_t EventTime = *(uint64_t*)&hdr.TimeStamp;
    switch (hdr.EventDescriptor.Id)
    {
        case D3D9PresentStart:
        {
            PresentEvent event;
            event.ProcessId = hdr.ProcessId;
            event.QpcTime = EventTime;
        
            TraceEventInfo eventInfo(pEventRecord);
            event.SwapChainAddress = eventInfo.GetPtr(L"pSwapchain");
            uint32_t D3D9Flags = eventInfo.GetData<uint32_t>(L"Flags");
            event.PresentFlags =
                ((D3D9Flags & D3DPRESENT_DONOTFLIP) ? DXGI_PRESENT_DO_NOT_SEQUENCE : 0) |
                ((D3D9Flags & D3DPRESENT_DONOTWAIT) ? DXGI_PRESENT_DO_NOT_WAIT : 0) |
                ((D3D9Flags & D3DPRESENT_FLIPRESTART) ? DXGI_PRESENT_RESTART : 0);
            event.SyncInterval = (D3D9Flags & D3DPRESENT_FORCEIMMEDIATE) ? 0 : event.SyncInterval;
            event.Runtime = Runtime::D3D9;
        
            mPresentByThreadId[hdr.ThreadId] = std::make_shared<PresentEvent>(event);

#if _DEBUG
            event.Completed = true;
#endif
            break;
        }
        case D3D9PresentStop:
        {
            RuntimePresentStop(pEventRecord);
            break;
        }
    }
}

void DxgiConsumer::OnEventRecord(PEVENT_RECORD pEventRecord)
{
    auto& hdr = pEventRecord->EventHeader;

    if (hdr.ProviderId == DXGI_PROVIDER_GUID)
    {
        OnDXGIEvent(pEventRecord);
    }
    else if (hdr.ProviderId == DXGKRNL_PROVIDER_GUID)
    {
        OnDXGKrnlEvent(pEventRecord);
    }
    else if (hdr.ProviderId == WIN32K_PROVIDER_GUID)
    {
        OnWin32kEvent(pEventRecord);
    }
    else if (hdr.ProviderId == DWM_PROVIDER_GUID)
    {
        OnDWMEvent(pEventRecord);
    }
    else if (hdr.ProviderId == D3D9_PROVIDER_GUID)
    {
        OnD3D9Event(pEventRecord);
    }
}

static void EtwProcessingThread(TraceSession *session)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    session->Process();
}

static GUID GuidFromString(const wchar_t *guidString)
{
    GUID g;
    auto hr = CLSIDFromString(guidString, &g);
    assert(SUCCEEDED(hr));
    return g;
}

void PresentMonEtw(PresentMonArgs args)
{
    TraceSession session(L"PresentMon");
    DxgiConsumer consumer;

    if (!session.Start()) {
        if (session.Status() == ERROR_ALREADY_EXISTS) {
            if (!session.Stop() || !session.Start()) {
                printf("ETW session error. Quitting.\n");
                exit(0);
            }
        }
    }

    session.EnableProvider(DXGI_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);
    session.EnableProvider(DXGKRNL_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 1);
    session.EnableProvider(WIN32K_PROVIDER_GUID, TRACE_LEVEL_INFORMATION, 0x1000);
    session.EnableProvider(DWM_PROVIDER_GUID, TRACE_LEVEL_VERBOSE);
    session.EnableProvider(D3D9_PROVIDER_GUID, TRACE_LEVEL_INFORMATION);

    session.OpenTrace(&consumer);
    uint32_t eventsLost, buffersLost;

    {
        // Launch the ETW producer thread
        std::thread etwThread(EtwProcessingThread, &session);

        // Consume / Update based on the ETW output
        {
            PresentMonData data;

            PresentMon_Init(args, data);

            while (!g_Quit)
            {
                std::vector<std::shared_ptr<PresentEvent>> presents;
                consumer.DequeuePresents(presents);

                PresentMon_Update(data, presents, session.PerfFreq());
                if (session.AnythingLost(eventsLost, buffersLost)) {
                    printf("Lost %u events, %u buffers.", eventsLost, buffersLost);
                }

                Sleep(100);
            }

            PresentMon_Shutdown(data);
        }

        etwThread.join();
    }

    session.CloseTrace();
    session.DisableProvider(DXGI_PROVIDER_GUID);
    session.DisableProvider(DXGKRNL_PROVIDER_GUID);
    session.DisableProvider(WIN32K_PROVIDER_GUID);
    session.DisableProvider(DWM_PROVIDER_GUID);
    session.DisableProvider(D3D9_PROVIDER_GUID);
    session.Stop();
}
