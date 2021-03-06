// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Implementation of the profiler DLL.
#include "syzygy/call_trace/profiler.h"

#include <windows.h>
#include <psapi.h>
#include <intrin.h>

#include <vector>

#include "base/at_exit.h"
#include "base/hash_tables.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/win/pe_image.h"
#include "sawbuck/common/com_utils.h"
#include "syzygy/call_trace/client_utils.h"
#include "syzygy/call_trace/call_trace_defs.h"
#include "syzygy/call_trace/shadow_stack.h"


namespace {
// Our AtExit manager required by base.
base::AtExitManager at_exit;

// All tracing runs through this object.
base::LazyInstance<call_trace::client::Profiler> static_profiler_instance =
    LAZY_INSTANCE_INITIALIZER;

// Helper structure to capture and restore the current thread's last win32
// error-code value.
struct ScopedLastErrorKeeper {
  ScopedLastErrorKeeper() : last_error(::GetLastError()) {
  }

  ~ScopedLastErrorKeeper() {
    ::SetLastError(last_error);
  }

  DWORD last_error;
};

typedef std::pair<RetAddr, FuncAddr> InvocationKey;

class HashInvocationKey {
 public:
  static const size_t bucket_size = 4;
  static const size_t min_buckets = 8;

  size_t operator()(const InvocationKey& key) const {
    return reinterpret_cast<size_t>(key.first) ^
        reinterpret_cast<size_t>(key.second);
  }

  bool operator()(const InvocationKey& a, const InvocationKey& b) const {
    return a < b;
  }
};
typedef base::hash_map<
    InvocationKey, InvocationInfo*, HashInvocationKey> InvocationMap;

typedef base::hash_set<HMODULE> ModuleSet;

}  // namespace

// See client.cc for a description of the unconventional
// calling conventions for this function.
extern "C" void __declspec(naked) _indirect_penter() {
  __asm {
    // Stash volatile registers.
    push eax
    push ecx
    push edx
    pushfd

    // Get the current cycle time.
    rdtsc
    push edx
    push eax

    // Retrieve the original function address, pushed by our caller.
    mov eax, DWORD PTR[esp + 0x18]
    push eax

    // Calculate the position of the return address on stack, and
    // push it. This becomes the EntryFrame argument.
    lea eax, DWORD PTR[esp + 0x20]
    push eax
    call call_trace::client::Profiler::FunctionEntryHook

    // Restore volatile registers.
    popfd
    pop edx
    pop ecx
    pop eax

    // Return to the address pushed by our caller.
    ret
  }
}

extern "C" void __declspec(naked) _indirect_penter_dllmain() {
  __asm {
    // Stash volatile registers.
    push eax
    push ecx
    push edx
    pushfd

    // Get the current cycle time.
    rdtsc
    push edx
    push eax

    // Retrieve the address pushed by our caller.
    mov eax, DWORD PTR[esp + 0x18]
    push eax

    // Calculate the position of the return address on stack, and
    // push it. This becomes the EntryFrame argument.
    lea eax, DWORD PTR[esp + 0x20]
    push eax
    call call_trace::client::Profiler::DllMainEntryHook

    // Restore volatile registers.
    popfd
    pop edx
    pop ecx
    pop eax

    // Return to the address pushed by our caller.
    ret
  }
}

// This instrumentation hook is used on return from a function.
void __declspec(naked) pexit() {
  __asm {
    // Stash the volatile registers.
    push eax
    push ecx
    push edx
    pushfd

    // Get the current cycle timer.
    rdtsc
    push edx
    push eax

    // Calculate the stack pointer prior to our entry.
    lea eax, DWORD PTR[esp + 0x18]
    push eax
    call call_trace::client::Profiler::FunctionExitHook

    popfd
    pop edx
    pop ecx

    // The return value from Client::FunctionExitHook is the real return
    // value. Swap it for the stashed EAX on stack and return to it.
    xchg eax, DWORD PTR[esp]
    ret
  }
}

BOOL WINAPI DllMain(HMODULE instance, DWORD reason, LPVOID reserved) {
  using call_trace::client::Profiler;

  switch (reason) {
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      Profiler::Instance()->OnDetach();
      break;

    default:
      break;
  }

  return TRUE;
}

namespace call_trace {
namespace client {

class Profiler::ThreadState {
 public:
  explicit ThreadState(Profiler* profiler) : profiler_(profiler), batch_(NULL) {
  }

  void OnModuleEntry(EntryFrame* entry_frame,
                     FuncAddr function,
                     uint64 cycles);

  void OnFunctionEntry(EntryFrame* entry_frame,
                       FuncAddr function,
                       uint64 cycles);
  RetAddr OnFunctionExit(const void* stack,
                         uint64 cycles_exit);

  call_trace::client::TraceFileSegment* segment() { return &segment_; }

 private:
  void RecordInvocation(RetAddr caller,
                        FuncAddr function,
                        uint64 cycles);

  void UpdateOverhead(uint64 entry_cycles);
  InvocationInfo* AllocateInvocationInfo();
  bool FlushSegment();

  // The profiler we're attached to.
  Profiler* profiler_;

  // We keep a running tally of the rough amount of wall clock cycles spent
  // inside the profiler. We then subtract the profiler's overhead from the
  // wall clock cycle timer on each measurement. This results in a timer that
  // measures time exclusive of profiling overhead.
  uint64 cycles_overhead_;

  // Our shadow stack.
  ShadowStack stack_;

  // The invocations we've recorded in our buffer.
  InvocationMap invocations_;

  // The trace file segment we're recording to.
  call_trace::client::TraceFileSegment segment_;

  // The current batch record we're writing to, if any.
  InvocationInfoBatch* batch_;

  // The set of modules we've logged.
  ModuleSet logged_modules_;
};

void Profiler::ThreadState::OnModuleEntry(EntryFrame* entry_frame,
                                          FuncAddr function,
                                          uint64 cycles) {
  // Bail early if we're disabled.
  if (profiler_->session_.IsDisabled())
    return;

  // The function invoked has a DllMain-like signature.
  // Get the module and reason from its invocation record.
  HMODULE module = reinterpret_cast<HMODULE>(entry_frame->args[0]);
  DWORD reason = entry_frame->args[1];

  // Only log module additions.
  bool should_log_module = false;
  switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
      should_log_module = true;
      break;

    case DLL_PROCESS_DETACH:
    case DLL_THREAD_DETACH:
      break;

    default:
      LOG(WARNING) << "Unrecognized module event: " << reason << ".";
      break;
  }

  // Make sure we only log each module once.
  if (should_log_module &&
      logged_modules_.find(module) == logged_modules_.end()) {
    logged_modules_.insert(module);

    // Make sure the event we're about to write will fit.
    if (!segment_.CanAllocate(sizeof(TraceModuleData)) || !FlushSegment()) {
      // Failed to allocate a new segment.
      return;
    }

    DCHECK(segment_.CanAllocate(sizeof(TraceModuleData)));

    // Allocate a record in the log.
    TraceModuleData* module_event = reinterpret_cast<TraceModuleData*>(
        segment_.AllocateTraceRecordImpl(ReasonToEventType(reason),
                                         sizeof(TraceModuleData)));
    DCHECK(module_event!= NULL);

    // Populate the log record.
    base::win::PEImage image(module);
    module_event->module_base_addr = module;
    module_event->module_base_size =
        image.GetNTHeaders()->OptionalHeader.SizeOfImage;
    module_event->module_checksum =
        image.GetNTHeaders()->OptionalHeader.CheckSum;
    module_event->module_time_date_stamp =
        image.GetNTHeaders()->FileHeader.TimeDateStamp;
    if (::GetMappedFileName(::GetCurrentProcess(), module,
                            &module_event->module_name[0],
                            arraysize(module_event->module_name)) == 0) {
        DWORD error = ::GetLastError();
        LOG(ERROR) << "Failed to get module name: " << com::LogWe(error) << ".";
    }
    module_event->module_exe[0] = L'\0';

    // We need to flush module events right away, so that the module is
    // defined in the trace file before events using that module start to
    // occur (in another thread).
    FlushSegment();
  }

  // Now record the function entry.
  OnFunctionEntry(entry_frame, function, cycles);
}

void Profiler::ThreadState::OnFunctionEntry(EntryFrame* entry_frame,
                                            FuncAddr function,
                                            uint64 cycles) {
  if (profiler_->session_.IsDisabled())
    return;

  stack_.TrimOrphansOnEntry(entry_frame);

  // Push this entry event.
  StackEntry& entry = stack_.Push(entry_frame);

  // Record the details of the call.
  // TODO(siggi): On tail-call and tail recursion elmination, the retaddr
  //     here will be penter, figure a way to fix that.
  entry.caller = entry_frame->retaddr;
  entry.function = function;
  entry.cycles_entry = cycles - cycles_overhead_;

  // Arrange to return to the exit hook.
  entry_frame->retaddr = pexit;

  UpdateOverhead(cycles);
}

RetAddr Profiler::ThreadState::OnFunctionExit(const void* stack,
                                              uint64 cycles_exit) {
  stack_.TrimOrphansOnExit(stack);
  StackEntry entry = stack_.Pop();

  // Calculate the number of cycles in the invocation, exclusive our overhead.
  uint64 cycles_executed = cycles_exit - entry.cycles_entry - cycles_overhead_;

  RecordInvocation(entry.caller, entry.function, cycles_executed);

  UpdateOverhead(cycles_exit);
  return entry.return_address;
}

void Profiler::OnDetach() {
  FreeThreadState();
}

void Profiler::ThreadState::RecordInvocation(RetAddr caller,
                                             FuncAddr function,
                                             uint64 duration_cycles) {
  // See whether we've already recorded an entry for this function.
  InvocationKey key(caller, function);
  InvocationMap::iterator it = invocations_.find(key);
  if (it != invocations_.end()) {
    // Yup, we already have an entry. Tally the new data.
    InvocationInfo* info = it->second;
    ++(info->num_calls);
    info->cycles_sum += duration_cycles;
    if (duration_cycles < info->cycles_min) {
      info->cycles_min = duration_cycles;
    } else if (duration_cycles > info->cycles_max) {
      info->cycles_max = duration_cycles;
    }
  } else {
    // Nopes, allocate a new entry for this invocation.
    InvocationInfo* info = AllocateInvocationInfo();
    if (info != NULL) {
      invocations_[key] = info;
      info->caller = caller;
      info->function = function;
      info->num_calls = 1;
      info->cycles_min = info->cycles_max = info->cycles_sum = duration_cycles;
    }
  }
}

void Profiler::ThreadState::UpdateOverhead(uint64 entry_cycles) {
  // TODO(siggi): Measure the fixed overhead on setup,
  //     then add it on every update.
  cycles_overhead_ += __rdtsc() - entry_cycles;
}

InvocationInfo* Profiler::ThreadState::AllocateInvocationInfo() {
  // This is kind of self-evident for the moment, as an invocation info batch
  // contains at least one invocation info as currently declared.
  // If this fails, please recondsider your implementation, or else revisit
  // the allocation code below.
  COMPILE_ASSERT(sizeof(InvocationInfoBatch) >= sizeof(InvocationInfo),
                 invocation_info_batch_must_be_larger_than_invocation_info);

  // Do we have a record that we can grow?
  if (batch_ != NULL && segment_.CanAllocateRaw(sizeof(InvocationInfo))) {
    InvocationInfo* invocation_info =
        reinterpret_cast<InvocationInfo*>(segment_.write_ptr);
    RecordPrefix* prefix = call_trace::client::GetRecordPrefix(batch_);
    prefix->size += sizeof(InvocationInfo);

    // Update the book-keeping.
    segment_.write_ptr += sizeof(InvocationInfo);
    segment_.header->segment_length += sizeof(InvocationInfo);

    return invocation_info;
  }

  // Do we need to scarf a new buffer?
  if (!segment_.CanAllocate(sizeof(InvocationInfoBatch)) && !FlushSegment()) {
    // We failed to allocate a new buffer.
    return NULL;
  }

  DCHECK(segment_.header != NULL);

  batch_ = segment_.AllocateTraceRecord<InvocationInfoBatch>();
  return &batch_->invocations[0];
}

bool Profiler::ThreadState::FlushSegment() {
  batch_ = NULL;
  invocations_.clear();

  return profiler_->session_.ExchangeBuffer(&segment_);
}

Profiler* Profiler::Instance() {
  return static_profiler_instance.Pointer();
}

Profiler::Profiler() {
  // Create our RPC session and allocate our initial trace segment on first use.
  ThreadState* data = GetOrAllocateThreadState();
  CHECK(data != NULL) << "Failed to allocate thread local state.";

  // Create the session (and allocate the first segment).
  session_.CreateSession(data->segment());
}

Profiler::~Profiler() {
}

Profiler::ThreadState* Profiler::GetThreadState() const {
  return tls_.Get();
}

Profiler::ThreadState* Profiler::GetOrAllocateThreadState() {
  ThreadState *data = tls_.Get();
  if (data != NULL)
    return data;

  data = new ThreadState(this);
  if (data == NULL) {
    LOG(ERROR) << "Unable to allocate per-thread data";
    return NULL;
  }
  tls_.Set(data);

  return data;
}

void Profiler::FreeThreadState() {
  ThreadState *data = tls_.Get();
  if (data != NULL) {
    delete data;
    tls_.Set(NULL);
  }
}

void WINAPI Profiler::DllMainEntryHook(EntryFrame* entry_frame,
                                       FuncAddr function,
                                       uint64 cycles) {
  ScopedLastErrorKeeper keep_last_error;

  Profiler* profiler = Profiler::Instance();
  ThreadState* data = profiler->GetOrAllocateThreadState();
  DCHECK(data != NULL);
  if (data != NULL)
    data->OnModuleEntry(entry_frame, function, cycles);
}

void WINAPI Profiler::FunctionEntryHook(EntryFrame* entry_frame,
                                        FuncAddr function,
                                        uint64 cycles) {
  ScopedLastErrorKeeper keep_last_error;

  Profiler* profiler = Profiler::Instance();
  ThreadState* data = profiler->GetOrAllocateThreadState();
  DCHECK(data != NULL);
  if (data != NULL)
    data->OnFunctionEntry(entry_frame, function, cycles);
}

RetAddr WINAPI Profiler::FunctionExitHook(const void* stack,
                                          uint64 cycles_exit) {
  ScopedLastErrorKeeper keep_last_error;

  Profiler* profiler = Profiler::Instance();
  ThreadState* data = profiler->GetThreadState();

  // An exit event implies that we previously had an entry event,
  // and the thread-local state must have been created at that time.
  CHECK(data != NULL);

  return data->OnFunctionExit(stack, cycles_exit);
}

}  // namespace client
}  // namespace call_trace
