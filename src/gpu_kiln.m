#import "gpu_kiln.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>

static const NSUInteger GPU_THREAD_COUNT = 262144;
static const NSUInteger GPU_SLOT_COUNT = 3;

static NSString *const GPU_SOURCE =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void corekiln_burn(device float4 *output [[buffer(0)]],\n"
     "                          uint gid [[thread_position_in_grid]]) {\n"
     "  if (gid >= 262144u) return;\n"
     "  float lane = float(gid) * 0.00000011920928955078125f;\n"
     "  float4 state = float4(0.113f, 0.271f, 0.419f, 0.577f) + lane;\n"
     "  for (uint iteration = 0; iteration < 512; ++iteration) {\n"
     "    state = fract(fma(state.yzwx,\n"
     "                      state.wxyz + float4(1.013f, 1.037f, 1.061f, "
     "1.087f),\n"
     "                      state + float4(0.101f, 0.211f, 0.307f, "
     "0.401f)));\n"
     "  }\n"
     "  output[gid] = state;\n"
     "}\n";

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) {
    return;
  }

  va_list arguments;
  va_start(arguments, format);
  vsnprintf(error, error_size, format, arguments);
  va_end(arguments);
}

@interface CorekilnGPUState : NSObject {
 @public
  pthread_t _thread;
  BOOL _thread_started;
  BOOL _joined;
  atomic_bool _stop_requested;
  atomic_uint_fast64_t _completed_commands;

  id<MTLDevice> _device;
  id<MTLLibrary> _library;
  id<MTLComputePipelineState> _pipeline;
  id<MTLCommandQueue> _command_queue;
  id<MTLBuffer> _buffers[3];
  dispatch_semaphore_t _slots[3];
  NSString *_device_name;
  NSLock *_failure_lock;
  NSString *_failure_message;
  int _stop_queue;
  uintptr_t _failure_event;
}

- (instancetype)initWithErrorMessage:(NSString **)error_message;
- (void)submitUntilStopped;
- (void)requestStop;
- (void)recordFailure:(NSString *)message;
- (NSString *)failureMessage;
- (const char *)deviceName;

@end

@implementation CorekilnGPUState

- (instancetype)initWithErrorMessage:(NSString **)error_message {
  self = [super init];
  if (self == nil) {
    return nil;
  }

  atomic_init(&_stop_requested, false);
  atomic_init(&_completed_commands, 0);
  _joined = YES;
  _failure_lock = [[NSLock alloc] init];
  _device = MTLCreateSystemDefaultDevice();
  if (_device == nil) {
    if (error_message != NULL) {
      *error_message = @"no Metal GPU is available";
    }
    return nil;
  }

  NSError *metal_error = nil;
  _library = [_device newLibraryWithSource:GPU_SOURCE
                                   options:nil
                                     error:&metal_error];
  if (_library == nil) {
    if (error_message != NULL) {
      *error_message =
          [NSString stringWithFormat:@"Metal shader compilation failed: %@",
                                     metal_error.localizedDescription];
    }
    return nil;
  }

  id<MTLFunction> function = [_library newFunctionWithName:@"corekiln_burn"];
  if (function == nil) {
    if (error_message != NULL) {
      *error_message = @"Metal shader is missing corekiln_burn";
    }
    return nil;
  }

  _pipeline =
      [_device newComputePipelineStateWithFunction:function error:&metal_error];
  if (_pipeline == nil) {
    if (error_message != NULL) {
      *error_message =
          [NSString stringWithFormat:@"Metal pipeline creation failed: %@",
                                     metal_error.localizedDescription];
    }
    return nil;
  }

  _command_queue = [_device newCommandQueue];
  if (_command_queue == nil) {
    if (error_message != NULL) {
      *error_message = @"unable to create a Metal command queue";
    }
    return nil;
  }

  NSUInteger buffer_length = GPU_THREAD_COUNT * sizeof(float) * 4;
  for (NSUInteger index = 0; index < GPU_SLOT_COUNT; index++) {
    _buffers[index] =
        [_device newBufferWithLength:buffer_length
                            options:MTLResourceStorageModePrivate];
    _slots[index] = dispatch_semaphore_create(1);
    if (_buffers[index] == nil || _slots[index] == nil) {
      if (error_message != NULL) {
        *error_message = @"unable to allocate Metal workload buffers";
      }
      return nil;
    }
  }

  _device_name = [_device.name copy];
  return self;
}

- (void)submitUntilStopped {
  NSUInteger slot_index = 0;

  while (!atomic_load_explicit(&_stop_requested, memory_order_relaxed)) {
    dispatch_semaphore_t slot = _slots[slot_index];
    dispatch_semaphore_wait(slot, DISPATCH_TIME_FOREVER);
    if (atomic_load_explicit(&_stop_requested, memory_order_relaxed)) {
      dispatch_semaphore_signal(slot);
      break;
    }

    @autoreleasepool {
      id<MTLCommandBuffer> command_buffer = [_command_queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder =
          [command_buffer computeCommandEncoder];
      if (command_buffer == nil || encoder == nil) {
        dispatch_semaphore_signal(slot);
        [self recordFailure:@"unable to create a Metal command buffer"];
        break;
      }

      [encoder setComputePipelineState:_pipeline];
      [encoder setBuffer:_buffers[slot_index] offset:0 atIndex:0];

      NSUInteger threadgroup_width =
          _pipeline.maxTotalThreadsPerThreadgroup;
      NSUInteger threadgroup_count =
          (GPU_THREAD_COUNT + threadgroup_width - 1) / threadgroup_width;
      [encoder dispatchThreadgroups:MTLSizeMake(threadgroup_count, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(threadgroup_width, 1, 1)];
      [encoder endEncoding];

      [command_buffer
          addCompletedHandler:^(id<MTLCommandBuffer> completed_buffer) {
            if (completed_buffer.status == MTLCommandBufferStatusCompleted) {
              atomic_fetch_add_explicit(&self->_completed_commands, 1,
                                        memory_order_relaxed);
            } else {
              NSString *description =
                  completed_buffer.error.localizedDescription ?:
                  @"unknown Metal command failure";
              [self
                  recordFailure:
                      [NSString stringWithFormat:@"Metal command failed: %@",
                                                 description]];
            }
            dispatch_semaphore_signal(slot);
          }];
      [command_buffer commit];
    }

    slot_index = (slot_index + 1) % GPU_SLOT_COUNT;
  }

  for (NSUInteger index = 0; index < GPU_SLOT_COUNT; index++) {
    dispatch_semaphore_wait(_slots[index], DISPATCH_TIME_FOREVER);
    dispatch_semaphore_signal(_slots[index]);
  }
}

- (void)requestStop {
  atomic_store_explicit(&_stop_requested, true, memory_order_relaxed);
}

- (void)recordFailure:(NSString *)message {
  BOOL should_signal = NO;

  [_failure_lock lock];
  if (_failure_message == nil) {
    _failure_message = [message copy];
    should_signal = YES;
  }
  [_failure_lock unlock];

  if (!should_signal) {
    return;
  }

  [self requestStop];
  struct kevent trigger;
  EV_SET(&trigger, _failure_event, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  if (kevent(_stop_queue, &trigger, 1, NULL, 0, NULL) == -1) {
    kill(getpid(), SIGTERM);
  }
}

- (NSString *)failureMessage {
  [_failure_lock lock];
  NSString *message = _failure_message;
  [_failure_lock unlock];
  return message;
}

- (const char *)deviceName {
  return _device_name.UTF8String;
}

@end

static CorekilnGPUState *state_for(gpu_kiln *kiln) {
  return (__bridge CorekilnGPUState *)(void *)kiln;
}

static void *submit_gpu(void *context) {
  @autoreleasepool {
    CorekilnGPUState *state =
        (__bridge_transfer CorekilnGPUState *)context;
    [state submitUntilStopped];
  }
  return NULL;
}

gpu_kiln *gpu_kiln_create(char *error, size_t error_size) {
  @autoreleasepool {
    NSString *error_message = nil;
    CorekilnGPUState *state =
        [[CorekilnGPUState alloc] initWithErrorMessage:&error_message];
    if (state == nil) {
      set_error(error, error_size, "%s",
                (error_message ?: @"unable to initialize Metal").UTF8String);
      return NULL;
    }

    return (gpu_kiln *)(__bridge_retained void *)state;
  }
}

bool gpu_kiln_start(gpu_kiln *kiln, int stop_queue,
                    uintptr_t failure_event, char *error, size_t error_size) {
  if (kiln == NULL) {
    set_error(error, error_size, "GPU engine cannot be started");
    return false;
  }

  CorekilnGPUState *state = state_for(kiln);
  if (state->_thread_started) {
    set_error(error, error_size, "GPU engine cannot be started");
    return false;
  }

  state->_stop_queue = stop_queue;
  state->_failure_event = failure_event;
  atomic_store_explicit(&state->_stop_requested, false, memory_order_relaxed);
  atomic_store_explicit(&state->_completed_commands, 0, memory_order_relaxed);
  state->_joined = NO;

  void *thread_context = (__bridge_retained void *)state;
  int thread_error =
      pthread_create(&state->_thread, NULL, submit_gpu, thread_context);
  if (thread_error != 0) {
    CFBridgingRelease(thread_context);
    state->_joined = YES;
    set_error(error, error_size, "pthread_create: %s", strerror(thread_error));
    return false;
  }

  state->_thread_started = YES;
  return true;
}

void gpu_kiln_request_stop(gpu_kiln *kiln) {
  if (kiln != NULL) {
    [state_for(kiln) requestStop];
  }
}

bool gpu_kiln_join(gpu_kiln *kiln, char *error, size_t error_size) {
  if (kiln == NULL) {
    return true;
  }

  CorekilnGPUState *state = state_for(kiln);
  if (state->_joined) {
    return true;
  }

  [state requestStop];
  int thread_error = pthread_join(state->_thread, NULL);
  state->_thread_started = NO;
  state->_joined = YES;
  if (thread_error != 0) {
    set_error(error, error_size, "pthread_join: %s", strerror(thread_error));
    return false;
  }

  NSString *failure_message = [state failureMessage];
  if (failure_message != nil) {
    set_error(error, error_size, "%s", failure_message.UTF8String);
    return false;
  }

  if (atomic_load_explicit(&state->_completed_commands, memory_order_relaxed) ==
      0) {
    set_error(error, error_size, "GPU engine completed no Metal work");
    return false;
  }

  return true;
}

void gpu_kiln_destroy(gpu_kiln *kiln) {
  if (kiln == NULL) {
    return;
  }

  gpu_kiln_request_stop(kiln);
  char ignored_error[1];
  gpu_kiln_join(kiln, ignored_error, sizeof(ignored_error));
  CFBridgingRelease((void *)kiln);
}

const char *gpu_kiln_device_name(const gpu_kiln *kiln) {
  return kiln == NULL ? "" : [state_for((gpu_kiln *)kiln) deviceName];
}

uint64_t gpu_kiln_completed_dispatches(const gpu_kiln *kiln) {
  if (kiln == NULL) {
    return 0;
  }

  CorekilnGPUState *state = state_for((gpu_kiln *)kiln);
  return atomic_load_explicit(&state->_completed_commands,
                              memory_order_relaxed);
}
